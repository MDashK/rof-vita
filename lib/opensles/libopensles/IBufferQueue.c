/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* BufferQueue implementation */

#include "sles_allinclusive.h"


/** Determine the state of the audio player or audio recorder associated with a buffer queue.
 *  Note that PLAYSTATE and RECORDSTATE values are equivalent (where PLAYING == RECORDING).
 */

static SLuint32 getAssociatedState(IBufferQueue *this)
{
    SLuint32 state;
    switch (InterfaceToObjectID(this)) {
    case SL_OBJECTID_AUDIOPLAYER:
        state = ((CAudioPlayer *) this->mThis)->mPlay.mState;
        break;
    case SL_OBJECTID_AUDIORECORDER:
        state = ((CAudioRecorder *) this->mThis)->mRecord.mState;
        break;
    default:
        // unreachable, but just in case we will assume it is stopped
        assert(SL_BOOLEAN_FALSE);
        state = SL_PLAYSTATE_STOPPED;
        break;
    }
    return state;
}

void IBufferQueue_ReleaseArrayBuffers(IBufferQueue *this)
{
    if ((NULL == this) || (NULL == this->mArray)) {
        return;
    }

    for (unsigned i = 0; i < this->mNumBuffers + 1; ++i) {
        BufferHeader_release(&this->mArray[i]);
    }
}


/* ROF: mixer output rate (matches the Vita audio port opened in Vita.c) */
static int rof_output_rate(void)
{
    return (&_opensles_user_freq != NULL && _opensles_user_freq > 0) ? _opensles_user_freq : 44100;
}

static inline int16_t rof_frame_sample(const IBufferQueue *this, const uint8_t *in, SLuint32 frame,
    int ch)
{
    const int c = (this->channels >= 2) ? ch : 0;
    const int stride = this->channels;
    if (this->bps == 8) {
        return (int16_t) (((int) in[frame * stride + c] - 0x80) << 8);
    }
    return ((const int16_t *) in)[frame * stride + c];
}

/** Convert one enqueued buffer (PCM8/PCM16, mono/stereo, any rate) to stereo PCM16 at
 *  outRate. Linear interpolation over the virtual stream [prev, in[0], in[1], ...]; the
 *  fractional read position and the last input frame carry over to the next buffer. */
static void *rof_resample(IBufferQueue *this, const void *pBuffer, SLuint32 size, int inRate,
    int outRate, SLuint32 *outSize)
{
    const int bytesPerFrame = (this->channels >= 2 ? 2 : 1) * (this->bps == 8 ? 1 : 2);
    const SLuint32 inFrames = size / bytesPerFrame;
    const uint8_t *in = (const uint8_t *) pBuffer;
    // input frames advanced per output frame, 32.32 fixed point
    const int64_t step = (int64_t) (((uint64_t) inRate << 32) / (uint64_t) outRate);
    const int64_t end = (int64_t) inFrames << 32; // index inFrames == last input frame
    int64_t pos = this->mResamplePos;

    SLuint32 outFrames = 0;
    if (pos < end) {
        outFrames = (SLuint32) ((end - pos + step - 1) / step);
    }
    if (0 == outFrames) {
        outFrames = 1; // never queue an empty buffer
    }
    int16_t *out = (int16_t *) malloc(outFrames * 4);
    if (NULL == out) {
        return NULL;
    }
    for (SLuint32 n = 0; n < outFrames; ++n, pos += step) {
        SLuint32 i = (SLuint32) (pos >> 32);              // left sample index in virtual stream
        int32_t frac = (int32_t) ((pos >> 16) & 0xFFFF);  // 0..65535
        for (int ch = 0; ch < 2; ++ch) {
            int32_t a = (0 == i) ? this->mResamplePrev[ch]
                : (i - 1 < inFrames ? rof_frame_sample(this, in, i - 1, ch)
                                    : rof_frame_sample(this, in, inFrames - 1, ch));
            int32_t b = (i < inFrames) ? rof_frame_sample(this, in, i, ch)
                : rof_frame_sample(this, in, inFrames - 1, ch);
            out[n * 2 + ch] = (int16_t) (a + (int32_t) (((int64_t) (b - a) * frac) >> 16));
        }
    }
    this->mResamplePos = pos - end;
    if (this->mResamplePos < 0) {
        this->mResamplePos = 0;
    }
    if (inFrames > 0) {
        this->mResamplePrev[0] = rof_frame_sample(this, in, inFrames - 1, 0);
        this->mResamplePrev[1] = rof_frame_sample(this, in, inFrames - 1, 1);
    }
    *outSize = outFrames * 4;
    return out;
}

SLresult IBufferQueue_Enqueue(SLBufferQueueItf self, const void *pBuffer, SLuint32 size)
{
    SL_ENTER_INTERFACE
    //SL_LOGV("IBufferQueue_Enqueue(%p, %p, %lu)", self, pBuffer, size);

    // Note that Enqueue while a Clear is pending is equivalent to Enqueue followed by Clear
    
    if (NULL == pBuffer || 0 == size) {
        result = SL_RESULT_PARAMETER_INVALID;
    } else {
        IBufferQueue *this = (IBufferQueue *) self;
        interface_lock_exclusive(this);
        BufferHeader *oldRear = this->mRear, *newRear;
        if ((newRear = oldRear + 1) == &this->mArray[this->mNumBuffers + 1]) {
            newRear = this->mArray;
        }
        if (newRear == this->mFront) {
            result = SL_RESULT_BUFFER_INSUFFICIENT;
        } else {
            // ROF: convert to the mixer format (stereo PCM16 at the output rate)
            // with a linear resampler whose position and last frame are carried
            // over between buffers, so tiny streamed buffers join seamlessly.
            void *ownedBuffer = NULL;
            SLuint32 outSize = size;
            const int inRate = (int) (this->samplerate / 1000);
            const int outRate = rof_output_rate();
            if (inRate > 0 && (inRate != outRate || this->channels != 2 || this->bps != 16)) {
                ownedBuffer = rof_resample(this, pBuffer, size, inRate, outRate, &outSize);
                if (NULL == ownedBuffer) {
                    interface_unlock_exclusive(this);
                    result = SL_RESULT_RESOURCE_ERROR;
                    goto fail;
                }
                pBuffer = ownedBuffer;
            }
            assert(NULL == oldRear->mOwnedBuffer);
            oldRear->mBuffer = pBuffer;
            oldRear->mSize = outSize;
            oldRear->mOwnedBuffer = ownedBuffer;
            this->mRear = newRear;
            ++this->mState.count;
            if (SL_OBJECTID_AUDIOPLAYER == InterfaceToObjectID(this)) {
                CAudioPlayer *audioPlayer = (CAudioPlayer *) this->mThis;
                audioPlayer->mPlay.mHeadAtEnd = SL_BOOLEAN_FALSE;
                audioPlayer->mPlay.mHeadStalled = SL_BOOLEAN_FALSE;
            }
            result = SL_RESULT_SUCCESS;
        }
        // set enqueue attribute if state is PLAYING and the first buffer is enqueued
        interface_unlock_exclusive_attributes(this, ((SL_RESULT_SUCCESS == result) &&
            (1 == this->mState.count) && (SL_PLAYSTATE_PLAYING == getAssociatedState(this))) ?
            ATTR_ENQUEUE : ATTR_NONE);
    }
fail:
    SL_LEAVE_INTERFACE
}


SLresult IBufferQueue_Clear(SLBufferQueueItf self)
{
    SL_ENTER_INTERFACE

    result = SL_RESULT_SUCCESS;
    IBufferQueue *this = (IBufferQueue *) self;
    interface_lock_exclusive(this);

#ifdef ANDROID
    if (SL_OBJECTID_AUDIOPLAYER == InterfaceToObjectID(this)) {
        CAudioPlayer *audioPlayer = (CAudioPlayer *) this->mThis;
        // flush associated audio player
        result = android_audioPlayer_bufferQueue_onClear(audioPlayer);
        if (SL_RESULT_SUCCESS == result) {
            IBufferQueue_ReleaseArrayBuffers(this);
            this->mFront = &this->mArray[0];
            this->mRear = &this->mArray[0];
            this->mState.count = 0;
            this->mState.playIndex = 0;
            this->mSizeConsumed = 0;
        }
    }
#endif

#ifdef USE_OUTPUTMIXEXT
    // mixer might be reading from the front buffer, so tread carefully here
    // NTH asynchronous cancel instead of blocking until mixer acknowledges
    this->mClearRequested = SL_BOOLEAN_TRUE;
    do {
        interface_cond_wait(this);
    } while (this->mClearRequested);
#endif
    // ROF: a cleared queue starts a new stream for the resampler
    this->mResamplePos = 0;
    this->mResamplePrev[0] = this->mResamplePrev[1] = 0;

    interface_unlock_exclusive(this);

    SL_LEAVE_INTERFACE
}


static SLresult IBufferQueue_GetState(SLBufferQueueItf self, SLBufferQueueState *pState)
{
    SL_ENTER_INTERFACE

    // Note that GetState while a Clear is pending is equivalent to GetState before the Clear

    if (NULL == pState) {
        result = SL_RESULT_PARAMETER_INVALID;
    } else {
        IBufferQueue *this = (IBufferQueue *) self;
        SLBufferQueueState state;
        interface_lock_shared(this);
    #ifdef __cplusplus // Avoid aggregate assignment trouble seen in C++ builds.
        state.count = this->mState.count;
        state.playIndex = this->mState.playIndex;
#else
        state = this->mState;
#endif
        interface_unlock_shared(this);
        *pState = state;
        result = SL_RESULT_SUCCESS;
    }

    SL_LEAVE_INTERFACE
}


SLresult IBufferQueue_RegisterCallback(SLBufferQueueItf self,
    slBufferQueueCallback callback, void *pContext)
{
    SL_ENTER_INTERFACE

    IBufferQueue *this = (IBufferQueue *) self;
    interface_lock_exclusive(this);
    // verify pre-condition that media object is in the SL_PLAYSTATE_STOPPED state
    if (SL_PLAYSTATE_STOPPED == getAssociatedState(this)) {
        this->mCallback = callback;
        this->mContext = pContext;
        result = SL_RESULT_SUCCESS;
    } else {
        result = SL_RESULT_PRECONDITIONS_VIOLATED;
    }
    interface_unlock_exclusive(this);

    SL_LEAVE_INTERFACE
}


static const struct SLBufferQueueItf_ IBufferQueue_Itf = {
    IBufferQueue_Enqueue,
    IBufferQueue_Clear,
    IBufferQueue_GetState,
    IBufferQueue_RegisterCallback
};

void IBufferQueue_init(void *self)
{
    //SL_LOGV("IBufferQueue_init(%p) entering", self);
    IBufferQueue *this = (IBufferQueue *) self;
    this->mItf = &IBufferQueue_Itf;
    this->mState.count = 0;
    this->mState.playIndex = 0;
    this->mCallback = NULL;
    this->mContext = NULL;
    this->mNumBuffers = 0;
    this->mClearRequested = SL_BOOLEAN_FALSE;
    this->mArray = NULL;
    this->mFront = NULL;
    this->mRear = NULL;
#ifdef ANDROID
    this->mSizeConsumed = 0;
#endif
    this->mResamplePos = 0;
    this->mResamplePrev[0] = this->mResamplePrev[1] = 0;
    BufferHeader *bufferHeader = this->mTypical;
    unsigned i;
    for (i = 0; i < BUFFER_HEADER_TYPICAL+1; ++i, ++bufferHeader) {
        BufferHeader_reset(bufferHeader);
    }
}


/** \brief Free the buffer queue, if it was larger than typical.
  * Called by CAudioPlayer_Destroy and CAudioRecorder_Destroy.
  */

void IBufferQueue_Destroy(IBufferQueue *this)
{
    IBufferQueue_ReleaseArrayBuffers(this);
    if ((NULL != this->mArray) && (this->mArray != this->mTypical)) {
        free(this->mArray);
        this->mArray = NULL;
    }
}
