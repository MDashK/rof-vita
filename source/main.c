/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Entry point: replays MainActivity / EGLView's lifecycle against
 * libAppMain.so and drives the render loop.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>

#include <pthread.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "rof/rof.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;

so_module so_mod;
RofNatives rof_native;

#define JNI_NAME(n) "Java_com_snkplaymore_android009_MainActivity_" n

static void *native_symbol(const char *name, int required) {
    void *p = (void *)so_symbol(&so_mod, name);
    if (!p && required)
        fatal_error("Missing symbol in libAppMain.so:\n%s", name);
    return p;
}

void rof_natives_resolve(void) {
    rof_native.ApplicationInit      = native_symbol(JNI_NAME("ApplicationInit"), 1);
    rof_native.init                 = native_symbol(JNI_NAME("init"), 1);
    rof_native.started              = native_symbol(JNI_NAME("started"), 1);
    rof_native.resume               = native_symbol(JNI_NAME("resume"), 1);
    rof_native.suspend              = native_symbol(JNI_NAME("suspend"), 1);
    rof_native.step                 = native_symbol(JNI_NAME("step"), 1);
    rof_native.onTouchEvent         = native_symbol(JNI_NAME("onTouchEvent"), 1);
    rof_native.SetKeyTrigger        = native_symbol(JNI_NAME("SetKeyTrigger"), 1);
    rof_native.loadTask             = native_symbol(JNI_NAME("loadTask"), 1);
    rof_native.purchaseFinished     = native_symbol(JNI_NAME("purchaseFinished"), 1);
    rof_native.purchaseListFinished = native_symbol(JNI_NAME("purchaseListFinished"), 1);
    rof_native.achievementFinished  = native_symbol(JNI_NAME("achievementFinished"), 0);
    rof_native.sendScoreFinished    = native_symbol(JNI_NAME("sendScoreFinished"), 0);
    rof_native.GoogleSignInFailed   = native_symbol(JNI_NAME("GoogleSignInFailed"), 0);
}

static void check_data(void) {
    SceIoStat st;
    if (sceIoGetstat(DATA_PATH "assets", &st) < 0)
        fatal_error("The game assets are missing.\n\nCopy the \"assets\" folder from the "
                    "APK to:\n" DATA_PATH "assets/");
    if (sceIoGetstat(ROF_SONGS_DIR, &st) < 0)
        fatal_error("The downloaded game data is missing.\n\nCopy the \"" ROF_PKG
                    "\" folder (songs) to:\n" DATA_PATH);
}

static void *game_main(void *arg);

int main() {
    // The game keeps large buffers on the stack; give it a roomy thread.
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_create(&t, &attr, game_main, NULL);
    pthread_join(t, NULL);
    return 0;
}

static void *game_main(void *arg) {
    // Keep the game on core 0; song decoding runs on core 2 (vorbis.c).
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    soloader_init_all();
    jni_frames_install();

    rof_natives_resolve();
    check_data();
    rof_fs_init();
    text_init();
    songs_report();
    filecache_preload();

    gl_init();
    input_init();

    JNIEnv *env = rof_env;
    jclass clazz = rof_clazz;

    // MainActivity.onCreate(): the asset manager object is opaque to us
    // (AAssetManager_fromJava returns a dummy), so any non-null value works.
    jstring pkg = (*env)->NewStringUTF(env, ROF_PKG);
    jstring sdcard = (*env)->NewStringUTF(env, ROF_SDCARD_ROOT);
    jstring device_id = (*env)->NewStringUTF(env, "0123456789abcdef");
    rof_native.ApplicationInit(env, clazz, (jobject)0x69696969, pkg, sdcard, device_id);
    l_success("ApplicationInit done.");

    // onStart() / onResume()
    rof_native.started(env, clazz);
    rof_native.resume(env, clazz);

    // EGLView.onSurfaceChanged(): same letterbox maths as MainActivity
    float wscale = ROF_SCREEN_W / ROF_GAME_W;
    float hscale = ROF_SCREEN_H / ROF_GAME_H;
    float scale = wscale > hscale ? hscale : wscale;
    int real_w = (int)(ROF_GAME_W * scale);
    int real_h = (int)(ROF_GAME_H * scale);
    rof_native.init(env, clazz, ROF_SCREEN_W, ROF_SCREEN_H, real_w, real_h, scale, scale);
    l_success("Game initialized (%ix%i, scale %.3f).", real_w, real_h, scale);
    input_prime();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    // EGLView.run() asks for a frame every m_Interval ms (32 ms in this game,
    // ~31 fps). Instead of sleeping, let the display pace us: 2 vblanks per
    // frame gives a steady 30 fps, 1 vblank gives 60 fps for short intervals.
    int vsync = 0;
    while (1) {
        int want = rof_animation_interval() >= 25 ? 2 : 1;
        if (want != vsync) {
            vsync = want;
            eglSwapInterval(0, vsync);
            l_info("Frame pacing: %i vblank(s) per frame.", vsync);
        }

        input_poll();
        rof_run_deferred();

        // EGLView.onDrawFrame(). The clear starts a vitaGL scene, which ends
        // the animated boot splash - so skip it until the game itself draws,
        // keeping the splash alive while it compiles shaders and loads.
        if (rof_game_drew)
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        uint32_t t0 = perf_now();
        rof_native.step(env, clazz);
        uint32_t t1 = perf_now();
        perf_add(PERF_STEP, t1 - t0);

        // The native calls of this frame returned: drop their JNI locals.
        jni_frame_end();

        gl_swap();
        perf_add(PERF_SWAP, perf_now() - t1);
        perf_frame_end();
    }

    return NULL;
}
