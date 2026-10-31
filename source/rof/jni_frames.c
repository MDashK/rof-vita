/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * JNI local reference frames.
 *
 * On Android every string/array the game creates through JNI is a local
 * reference that the JVM frees when the native call (step, loadTask, ...)
 * returns. FalsoJNI never frees them, and the game creates some every frame
 * (TexString::createTexture builds 7 arrays + 1 string per text line), so
 * memory slowly runs out.
 *
 * Here the JNIEnv "New*" functions are wrapped: objects created by code in
 * libAppMain.so are remembered per thread and freed by jni_frame_end(),
 * which the main loop calls after step() and the load thread calls when
 * loadTask() returns - the same lifetime a real JVM gives them.
 * DeleteLocalRef is still ignored on purpose: the game deletes strings
 * right after storing them in arrays it is about to pass to Java.
 */

#include "rof/rof.h"

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_ImplBridge.h>

#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdlib.h>

#include "utils/logger.h"

#define MAX_TRACKED 16384

typedef struct {
    void *obj;
    SceUID thread;
    uint8_t is_string;
} Tracked;

static Tracked tracked[MAX_TRACKED];
static int n_tracked = 0;
static int overflow_warned = 0;
static pthread_mutex_t track_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned freed_total = 0;

static inline int called_from_game(void *ra) {
    uintptr_t a = (uintptr_t)ra;
    return a >= so_mod.text_base && a < so_mod.text_base + so_mod.text_size;
}

static void track(void *obj, int is_string) {
    if (!obj)
        return;
    pthread_mutex_lock(&track_lock);
    if (n_tracked < MAX_TRACKED) {
        tracked[n_tracked].obj = obj;
        tracked[n_tracked].thread = sceKernelGetThreadId();
        tracked[n_tracked].is_string = (uint8_t)is_string;
        n_tracked++;
    } else if (!overflow_warned) {
        overflow_warned = 1;
        l_warn("jni_frames: tracking table full, some JNI objects will leak");
    }
    pthread_mutex_unlock(&track_lock);
}

static void untrack(void *obj) {
    for (int i = 0; i < n_tracked; i++) {
        if (tracked[i].obj == obj) {
            tracked[i] = tracked[--n_tracked];
            return;
        }
    }
}

static void free_object(Tracked *t) {
    if (t->is_string) {
        JavaString *s = (JavaString *)t->obj;
        jda_free(s->utf8);
        jda_free(s->utf16);
        free(s);
    } else {
        jda_free((JavaDynArray *)t->obj);
    }
}

void jni_frame_end(void) {
    SceUID me = sceKernelGetThreadId();
    pthread_mutex_lock(&track_lock);
    int i = 0;
    while (i < n_tracked) {
        if (tracked[i].thread == me) {
            free_object(&tracked[i]);
            freed_total++;
            tracked[i] = tracked[--n_tracked];
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&track_lock);
}

unsigned jni_frames_freed_total(void) { return freed_total; }
int jni_frames_pending(void) { return n_tracked; }

/* ---- wrappers ---------------------------------------------------------- */

static jstring (*o_NewStringUTF)(JNIEnv *, const char *);
static jobject (*o_NewGlobalRef)(JNIEnv *, jobject);
static jobjectArray (*o_NewObjectArray)(JNIEnv *, jsize, jclass, jobject);
static jbooleanArray (*o_NewBooleanArray)(JNIEnv *, jsize);
static jbyteArray (*o_NewByteArray)(JNIEnv *, jsize);
static jcharArray (*o_NewCharArray)(JNIEnv *, jsize);
static jshortArray (*o_NewShortArray)(JNIEnv *, jsize);
static jintArray (*o_NewIntArray)(JNIEnv *, jsize);
static jlongArray (*o_NewLongArray)(JNIEnv *, jsize);
static jfloatArray (*o_NewFloatArray)(JNIEnv *, jsize);
static jdoubleArray (*o_NewDoubleArray)(JNIEnv *, jsize);

static jstring __attribute__((noinline)) w_NewStringUTF(JNIEnv *env, const char *bytes) {
    void *ra = __builtin_return_address(0);
    if (!bytes)
        bytes = ""; // Android throws; FalsoJNI would crash in strlen
    jstring r = o_NewStringUTF(env, bytes);
    if (called_from_game(ra))
        track(r, 1);
    return r;
}

static jobjectArray __attribute__((noinline)) w_NewObjectArray(JNIEnv *env, jsize len, jclass c, jobject init) {
    void *ra = __builtin_return_address(0);
    jobjectArray r = o_NewObjectArray(env, len, c, init);
    if (called_from_game(ra))
        track(r, 0);
    return r;
}

#define WRAP_ARRAY(T, Name) \
    static T __attribute__((noinline)) w_##Name(JNIEnv *env, jsize len) { \
        void *ra = __builtin_return_address(0); \
        T r = o_##Name(env, len); \
        if (called_from_game(ra)) \
            track(r, 0); \
        return r; \
    }

WRAP_ARRAY(jbooleanArray, NewBooleanArray)
WRAP_ARRAY(jbyteArray, NewByteArray)
WRAP_ARRAY(jcharArray, NewCharArray)
WRAP_ARRAY(jshortArray, NewShortArray)
WRAP_ARRAY(jintArray, NewIntArray)
WRAP_ARRAY(jlongArray, NewLongArray)
WRAP_ARRAY(jfloatArray, NewFloatArray)
WRAP_ARRAY(jdoubleArray, NewDoubleArray)

// A global ref keeps the object (and, for object arrays, its elements) alive.
static jobject w_NewGlobalRef(JNIEnv *env, jobject obj) {
    if (obj) {
        pthread_mutex_lock(&track_lock);
        // Only objects we track are inspected (others may be fake handles).
        for (int i = 0; i < n_tracked; i++) {
            if (tracked[i].obj != obj)
                continue;
            int is_string = tracked[i].is_string;
            tracked[i] = tracked[--n_tracked];
            // Object arrays: also keep the strings stored in them.
            JavaDynArray *jda = (JavaDynArray *)obj;
            if (!is_string && jda->type == FIELD_TYPE_OBJECT) {
                jobject *el = (jobject *)jda->array;
                for (int k = 0; k < jda->len; k++)
                    if (el[k])
                        untrack(el[k]);
            }
            break;
        }
        pthread_mutex_unlock(&track_lock);
    }
    return o_NewGlobalRef(env, obj);
}

void jni_frames_install(void) {
    struct JNINativeInterface *t = (struct JNINativeInterface *)*rof_env;
    o_NewStringUTF = t->NewStringUTF;         t->NewStringUTF = w_NewStringUTF;
    o_NewGlobalRef = t->NewGlobalRef;         t->NewGlobalRef = w_NewGlobalRef;
    o_NewObjectArray = t->NewObjectArray;     t->NewObjectArray = w_NewObjectArray;
    o_NewBooleanArray = t->NewBooleanArray;   t->NewBooleanArray = w_NewBooleanArray;
    o_NewByteArray = t->NewByteArray;         t->NewByteArray = w_NewByteArray;
    o_NewCharArray = t->NewCharArray;         t->NewCharArray = w_NewCharArray;
    o_NewShortArray = t->NewShortArray;       t->NewShortArray = w_NewShortArray;
    o_NewIntArray = t->NewIntArray;           t->NewIntArray = w_NewIntArray;
    o_NewLongArray = t->NewLongArray;         t->NewLongArray = w_NewLongArray;
    o_NewFloatArray = t->NewFloatArray;       t->NewFloatArray = w_NewFloatArray;
    o_NewDoubleArray = t->NewDoubleArray;     t->NewDoubleArray = w_NewDoubleArray;
}
