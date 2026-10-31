/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Native Ogg Vorbis decoding for the game's sounds.
 *
 * libAppMain.so is ARMv5 code with *software* floating point, so its
 * built-in stb_vorbis decodes every song with emulated float maths. Songs and
 * voices are decoded fully into memory before/while they play, which is what
 * made song loading and the end-of-song -> results transition take seconds.
 *
 * The game's two entry points are replaced with the same stb_vorbis compiled
 * for the Vita's Cortex-A9 (VFP/NEON), keeping their exact contracts:
 *
 *   stb_vorbis_decode_memory_plus_nothread(data, len, &ch, &rate, &out)
 *     decode everything now; returns samples per channel (<0 on error);
 *     out is new[]'d, the caller keeps and frees `data`.
 *
 *   stb_vorbis_decode_memory_plus(data, len, &ch, &rate, &alive_holder,
 *                                 &progress, mutex, &out)
 *     allocates `out` for the whole stream and returns at once; a thread
 *     fills it, publishing the frames decoded so far in *progress under the
 *     game's mutex. *alive_holder points to an int the owner clears to cancel.
 *     When done the thread sets *alive_holder = NULL and frees `data`; when
 *     cancelled it frees `out` and `data` instead (CreatePlayerFromFile /
 *     the original decode thread at 0x539b2).
 */

#pragma GCC optimize("O3")

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../lib/stb/stb_vorbis.c"

#include "rof/rof.h"

#include <psp2/kernel/threadmgr.h>

#include "reimpl/pthr.h"
#include "utils/logger.h"

// The game's operator new[] / delete[] / new / delete (our libstdc++)
extern void *_Znaj(unsigned int);
extern void _ZdaPv(void *);
extern void _ZdlPv(void *);

static int open_stream(const unsigned char *data, int len, stb_vorbis **out_v,
                       int *channels, int *rate, int *samples) {
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(data, len, &err, NULL);
    if (!v)
        return -1;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    *channels = info.channels;
    *rate = (int)info.sample_rate;
    *samples = (int)stb_vorbis_stream_length_in_samples(v);
    *out_v = v;
    return 0;
}

static int rof_decode_nothread(const unsigned char *data, int len, int *channels, int *rate,
                               short **output) {
    stb_vorbis *v;
    int ch, sr, total;
    uint32_t t0 = perf_now();
    if (open_stream(data, len, &v, &ch, &sr, &total) < 0)
        return -1;
    *channels = ch;
    *rate = sr;
    if (total <= 0) {
        stb_vorbis_close(v);
        return -2;
    }
    short *out = _Znaj((unsigned)total * ch * 2);
    if (!out) {
        stb_vorbis_close(v);
        return -2;
    }
    int pos = 0, cap = total * ch;
    for (;;) {
        int n = stb_vorbis_get_frame_short_interleaved(v, ch, out + pos, cap - pos);
        if (n == 0)
            break;
        pos += n * ch;
        if (pos >= cap)
            break;
    }
    if (pos < cap)
        memset(out + pos, 0, (size_t)(cap - pos) * 2);
    stb_vorbis_close(v);
    *output = out;
    l_debug("vorbis: %d frames (%d ch, %d Hz) in %u ms", total, ch, sr, (perf_now() - t0) / 1000);
    return total;
}

typedef struct {
    stb_vorbis *v;
    int channels;
    int total;
    int **alive_holder;  // owner's pointer to the "alive" int
    int *alive;
    int *progress;       // frames decoded so far (read by the game)
    void *mutex;         // game (bionic) pthread mutex
    short *out;
    unsigned char *data; // compressed input, freed when done
} DecodeJob;

static void free_job(DecodeJob *job, int cancelled) {
    pthread_mutex_lock_soloader(job->mutex);
    if (!cancelled && *job->alive != 0) {
        // Finished normally: tell the owner, keep `out` (it is theirs now)
        *job->alive_holder = NULL;
        _ZdlPv(job->alive);
        if (job->data)
            _ZdaPv(job->data);
    } else {
        // Owner gave up on this sound: nobody will free the buffers
        if (job->out)
            _ZdaPv(job->out);
        if (job->data && (void *)job->data != (void *)job->out)
            _ZdaPv(job->data);
        _ZdlPv(job->alive);
    }
    pthread_mutex_unlock_soloader(job->mutex);
    stb_vorbis_close(job->v);
    free(job);
}

static int decode_thread(SceSize args, void *argp) {
    DecodeJob *job = *(DecodeJob **)argp;
    uint32_t t0 = perf_now();
    int pos = 0, frames = 0, cap = job->total * job->channels;
    for (;;) {
        if (*job->alive == 0) {
            free_job(job, 1);
            return sceKernelExitDeleteThread(0);
        }
        int n = pos < cap ? stb_vorbis_get_frame_short_interleaved(job->v, job->channels,
                                                                   job->out + pos, cap - pos)
                          : 0;
        if (n == 0)
            break;
        frames += n;
        pos += n * job->channels;
        pthread_mutex_lock_soloader(job->mutex);
        *job->progress = frames;
        pthread_mutex_unlock_soloader(job->mutex);
    }
    l_debug("vorbis: streamed %d frames in %u ms", frames, (perf_now() - t0) / 1000);
    free_job(job, 0);
    return sceKernelExitDeleteThread(0);
}

static int rof_decode_threaded(unsigned char *data, int len, int *channels, int *rate,
                               int **alive_holder, int *progress, void *mutex, short **output) {
    stb_vorbis *v;
    int ch, sr, total;
    if (open_stream(data, len, &v, &ch, &sr, &total) < 0)
        return -1;
    *channels = ch;
    *rate = sr;
    if (total <= 0) {
        stb_vorbis_close(v);
        return -2;
    }
    short *out = _Znaj((unsigned)total * ch * 2);
    DecodeJob *job = out ? calloc(1, sizeof(*job)) : NULL;
    if (!job) {
        if (out)
            _ZdaPv(out);
        stb_vorbis_close(v);
        return -2;
    }
    memset(out, 0, (size_t)total * ch * 2);
    job->v = v;
    job->channels = ch;
    job->total = total;
    job->alive_holder = alive_holder;
    job->alive = *alive_holder;
    job->progress = progress;
    job->mutex = mutex;
    job->out = out;
    job->data = data;
    *output = out;

    // Game threads busy-wait for the first second of audio (main thread,
    // loadTask). The Vita does not time-slice threads of equal priority, so
    // a waiter on the same core would starve the decoder forever: run it on
    // core 2 *above* the game threads (still below the audio thread, 64).
    SceUID t = sceKernelCreateThread("rof vorbis", decode_thread, 72, 256 * 1024, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (t < 0 || sceKernelStartThread(t, sizeof(job), &job) < 0) {
        l_error("vorbis: cannot start decode thread (0x%x)", t);
        // Decode synchronously instead
        int pos = 0, cap = total * ch, frames = 0;
        for (;;) {
            int n = stb_vorbis_get_frame_short_interleaved(v, ch, out + pos, cap - pos);
            if (n == 0) break;
            frames += n;
            pos += n * ch;
            if (pos >= cap) break;
        }
        *progress = frames;
        job->alive = *alive_holder;
        *alive_holder = NULL;
        _ZdlPv(job->alive);
        _ZdaPv(data);
        stb_vorbis_close(v);
        free(job);
    }
    return total;
}

void vorbis_patch(void) {
    uintptr_t f;
    if ((f = so_symbol(&so_mod, "stb_vorbis_decode_memory_plus_nothread")))
        hook_addr(f, (uintptr_t)&rof_decode_nothread);
    else
        l_warn("vorbis: decode_memory_plus_nothread not found");
    if ((f = so_symbol(&so_mod, "stb_vorbis_decode_memory_plus")))
        hook_addr(f, (uintptr_t)&rof_decode_threaded);
    else
        l_warn("vorbis: decode_memory_plus not found");
}
