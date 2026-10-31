/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Shared declarations for the game-specific parts of the loader.
 */

#ifndef ROF_H
#define ROF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <falso_jni/jni.h>
#include <so_util/so_util.h>

#define ROF_PKG "com.snkplaymore.android009"

// Android "external storage" root handed to the game. The game appends
// "/<package>/<file>", so the downloaded songs live in
// ux0:data/rof/com.snkplaymore.android009/
#define ROF_SDCARD_ROOT "ux0:data/rof"
#define ROF_SONGS_DIR   DATA_PATH ROF_PKG "/"

// Android internal storage (/data/data/<package>/) is redirected here.
// The save files (save.dat, tunes.dat, res.dat, login.dat) go in this folder.
#define ROF_SAVE_DIR    DATA_PATH "save"

// Screen setup reported to the game (same maths as MainActivity.onCreate):
// the game is designed for 960x640 and letterboxes itself.
#define ROF_SCREEN_W    960
#define ROF_SCREEN_H    544
#define ROF_GAME_W      960.0f
#define ROF_GAME_H      640.0f

extern so_module so_mod;
extern JNIEnv *rof_env;   // &jni from FalsoJNI
extern jclass rof_clazz;  // fake MainActivity class

// Native entry points exported by libAppMain.so (Java_..._MainActivity_*)
typedef struct {
    void (*ApplicationInit)(JNIEnv *, jclass, jobject, jstring, jstring, jstring);
    void (*init)(JNIEnv *, jclass, int, int, int, int, float, float);
    void (*started)(JNIEnv *, jclass);
    void (*resume)(JNIEnv *, jclass);
    void (*suspend)(JNIEnv *, jclass);
    void (*step)(JNIEnv *, jclass);
    void (*onTouchEvent)(JNIEnv *, jclass, jintArray, jfloatArray);
    void (*SetKeyTrigger)(JNIEnv *, jclass, int);
    void (*loadTask)(JNIEnv *, jclass);
    void (*purchaseFinished)(JNIEnv *, jclass, int, jstring);
    void (*purchaseListFinished)(JNIEnv *, jclass, int, jobjectArray, jobjectArray,
                                 jobjectArray, jobjectArray, jobjectArray);
    void (*achievementFinished)(JNIEnv *, jclass, int, int);
    void (*sendScoreFinished)(JNIEnv *, jclass, jboolean, int);
    void (*GoogleSignInFailed)(JNIEnv *, jclass);
} RofNatives;

extern RofNatives rof_native;
void rof_natives_resolve(void);

// fs.c - path redirection and libc overrides
const char *rof_path(const char *in, char *out, size_t out_size);
void rof_fs_init(void);
extern so_default_dynlib rof_dynlib[];
extern const size_t rof_dynlib_count;

// text.c - replacement for Android Paint/Canvas text drawing
int   text_init(void);
float text_measure(int size, const char *utf8);
float text_ascent(int size);   // negative, like Paint.FontMetrics.ascent
float text_descent(int size);  // positive
float text_bottom(int size);   // positive
void  text_draw(uint32_t *pixels, int stride, int width, int height,
                float scale, int size, uint32_t argb, float x, float y,
                const char *utf8);

// java.c - deferred callbacks into the game (Android Handler equivalent)
void rof_run_deferred(void);
int  rof_animation_interval(void);

// input.c - Vita touch + buttons -> MainActivity.onTouchEvent
void input_init(void);
void input_poll(void);
void input_prime(void); // after init(): give every touch slot a known position

// songs.c - report which song packs are incomplete
void songs_report(void);

// patch.c
void rof_patch(void);

// tutorial.c - which tutorial window / pop-up is on screen
void tutorial_patch(void);
void tutorial_frame(void);
int  tutorial_window(void);   // 0 none, 1 first part, 2 second part
int  tutorial_popup(void);    // tip window open during the practice song
int  tutorial_page(void);
void tutorial_activate(int page_id, int pages);
void tutorial_turn_page(int dir);  // -1 previous, +1 next (applied by the hook)

// vorbis.c - native (VFP) Ogg Vorbis decoding in place of the game's soft-float one
void vorbis_patch(void);

// filecache.c - .bmv animations served from RAM
FILE *filecache_open(const char *path, const char *mode);
void filecache_preload(void);

// jni_frames.c - free game-created JNI objects when the native call returns
void jni_frames_install(void);
void jni_frame_end(void);

// perf.c - frame profiler (one summary line in log.txt every 5 s)
typedef enum {
    PERF_STEP, PERF_SWAP, PERF_TEXT_DRAW, PERF_TEXT_MEASURE, PERF_TEX_UPLOAD,
    PERF_DRAW, PERF_IO, PERF_COUNT
} PerfId;
#include <psp2/kernel/processmgr.h>
static inline uint32_t perf_now(void) { return sceKernelGetProcessTimeLow(); }
void perf_add(PerfId id, uint32_t us);
void perf_upload_bytes(uint32_t bytes);
void perf_io_bytes(uint32_t bytes);
void perf_frame_end(void);
extern volatile int rof_game_drew; // the game has issued a clear/draw call

#endif // ROF_H
