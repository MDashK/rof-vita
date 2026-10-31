/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * FalsoJNI implementation of com.snkplaymore.android009.MainActivity.
 *
 * libAppMain.so calls these static Java methods through
 * GetStaticMethodID/CallStatic*Method. Behaviour mirrors the decompiled
 * MainActivity.java (v1.4.1), with Google Play / Chartboost / Bluetooth
 * stubbed out and in-app billing emulated (every purchase succeeds, which
 * is what the user's original Lucky Patcher setup did).
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/kernel/threadmgr.h>

#include "rof/rof.h"
#include "utils/logger.h"
#include "utils/utils.h"

JNIEnv *rof_env = &jni;
jclass rof_clazz = (jclass)0x42424242;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char *jstr_dup(jstring s) {
    if (!s)
        return NULL;
    const char *c = (*rof_env)->GetStringUTFChars(rof_env, s, NULL);
    char *r = c ? strdup(c) : NULL;
    if (c)
        (*rof_env)->ReleaseStringUTFChars(rof_env, s, (char *)c);
    return r;
}

static jstring jstr_new(const char *s) {
    return s ? (*rof_env)->NewStringUTF(rof_env, s) : NULL;
}

/* ------------------------------------------------------------------ */
/* Deferred callbacks (Android Handler / async listeners)              */
/* ------------------------------------------------------------------ */

typedef enum { CB_PURCHASE_FINISHED } CallbackType;

typedef struct Deferred {
    CallbackType type;
    int code;
    char *str;
    struct Deferred *next;
} Deferred;

static Deferred *deferred_head = NULL, *deferred_tail = NULL;
static pthread_mutex_t deferred_lock = PTHREAD_MUTEX_INITIALIZER;

static void defer(CallbackType type, int code, const char *str) {
    Deferred *d = calloc(1, sizeof(Deferred));
    d->type = type;
    d->code = code;
    d->str = str ? strdup(str) : NULL;
    pthread_mutex_lock(&deferred_lock);
    if (deferred_tail)
        deferred_tail->next = d;
    else
        deferred_head = d;
    deferred_tail = d;
    pthread_mutex_unlock(&deferred_lock);
}

// Called from the main loop, before step(), like the Android UI thread.
void rof_run_deferred(void) {
    pthread_mutex_lock(&deferred_lock);
    Deferred *d = deferred_head;
    deferred_head = deferred_tail = NULL;
    pthread_mutex_unlock(&deferred_lock);

    while (d) {
        Deferred *next = d->next;
        if (d->type == CB_PURCHASE_FINISHED) {
            l_info("purchaseFinished(%i, %s)", d->code, d->str ? d->str : "null");
            rof_native.purchaseFinished(rof_env, rof_clazz, d->code, jstr_new(d->str));
        }
        free(d->str);
        free(d);
        d = next;
    }
}

/* ------------------------------------------------------------------ */
/* Text (Paint / Canvas)                                               */
/* ------------------------------------------------------------------ */

#define TEX_MAX 1024
static JavaDynArray *text_pixels = NULL;  // reused: the game copies it out
static pthread_mutex_t text_draw_lock = PTHREAD_MUTEX_INITIALIZER;

// float getFontWidthJava(int size, String str)
jfloat getFontWidthJava(jmethodID id, va_list args) {
    int size = va_arg(args, int);
    jstring s = va_arg(args, jstring);
    char *str = jstr_dup(s);
    float w = text_measure(size, str);
    free(str);
    return w;
}

// float getFontHeightJava(int size): -ascent + descent
jfloat getFontHeightJava(jmethodID id, va_list args) {
    int size = va_arg(args, int);
    return -text_ascent(size) + text_descent(size);
}

jfloat getFontAscentJava(jmethodID id, va_list args) {
    int size = va_arg(args, int);
    return text_ascent(size);
}

jfloat getFontDescentJava(jmethodID id, va_list args) {
    int size = va_arg(args, int);
    return text_descent(size);
}

// float getFontLineHeightJava(int size): -ascent + bottom
jfloat getFontLineHeightJava(jmethodID id, va_list args) {
    int size = va_arg(args, int);
    return -text_ascent(size) + text_bottom(size);
}

static int pow2_at_least(int v) {
    int p = 32;
    while (p < v)
        p <<= 1;
    return p > TEX_MAX ? TEX_MAX : p;
}

// int[] onTextDraw(int[] info, float scale, int[] size, int[] col,
//                  int[] x, int[] y, boolean[] edge, String[] str)
// Returns ARGB pixels packed with a stride of the power-of-two width.
jobject onTextDraw(jmethodID id, va_list args) {
    JavaDynArray *j_info = va_arg(args, JavaDynArray *);
    float scale = (float)va_arg(args, double); // float is promoted in varargs
    JavaDynArray *j_size = va_arg(args, JavaDynArray *);
    JavaDynArray *j_col = va_arg(args, JavaDynArray *);
    JavaDynArray *j_x = va_arg(args, JavaDynArray *);
    JavaDynArray *j_y = va_arg(args, JavaDynArray *);
    JavaDynArray *j_edge = va_arg(args, JavaDynArray *);
    JavaDynArray *j_str = va_arg(args, JavaDynArray *);

    if (!j_info || !j_size || !j_col || !j_x || !j_y || !j_edge || !j_str) {
        l_error("onTextDraw: missing array argument");
        return NULL;
    }

    uint32_t perf_t0 = perf_now();
    pthread_mutex_lock(&text_draw_lock);
    if (!text_pixels)
        text_pixels = jda_alloc(TEX_MAX * TEX_MAX, FIELD_TYPE_INT);
    if (!text_pixels) {
        pthread_mutex_unlock(&text_draw_lock);
        return NULL;
    }

    const jint *info = j_info->array;
    const jint *size = j_size->array;
    const jint *col = j_col->array;
    const jint *xs = j_x->array;
    const jint *ys = j_y->array;
    const jboolean *edge = j_edge->array;
    const jobject *strs = j_str->array;

    int count = info[0];
    if (count > j_str->len) count = j_str->len;
    int sw = pow2_at_least((int)(info[1] * scale));
    int sh = pow2_at_least((int)(info[2] * scale));
    int frame = info[3] == 1;

    uint32_t *px = (uint32_t *)text_pixels->array;
    memset(px, 0, (size_t)sw * sh * sizeof(uint32_t));

    static const int pad_x[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int pad_y[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

    char **text = calloc(count > 0 ? count : 1, sizeof(char *));
    for (int i = 0; i < count; i++)
        text[i] = jstr_dup((jstring)strs[i]);

    // Black outline pass ("frame" mode).
    if (frame) {
        for (int j = 0; j < 8; j++) {
            for (int i = 0; i < count; i++) {
                if (!text[i] || !*text[i]) continue;
                float asc = -text_ascent(size[i]);
                text_draw(px, sw, sw, sh, scale, size[i], 0xFF000000,
                          xs[i] + pad_x[j], ys[i] + asc + pad_y[j], text[i]);
            }
        }
    }

    for (int i = 0; i < count; i++) {
        if (!text[i] || !*text[i]) continue;
        uint32_t c = (uint32_t)col[i];
        // Java: Color.argb(c>>24, c&255, (c>>8)&255, (c>>16)&255)
        uint32_t argb = (c & 0xFF000000) | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
        float asc = -text_ascent(size[i]);
        if (edge[i]) {
            for (int j = 0; j < 8; j++) {
                int posx = (int)(xs[i] + pad_x[j] * scale);
                int posy = (int)(ys[i] + pad_y[j] * scale);
                text_draw(px, sw, sw, sh, scale, size[i], argb, posx, posy + asc, text[i]);
            }
        } else {
            text_draw(px, sw, sw, sh, scale, size[i], argb, xs[i], ys[i] + asc, text[i]);
        }
    }

    for (int i = 0; i < count; i++)
        free(text[i]);
    free(text);
    pthread_mutex_unlock(&text_draw_lock);
    perf_add(PERF_TEXT_DRAW, perf_now() - perf_t0);
    return (jobject)text_pixels;
}

/* ------------------------------------------------------------------ */
/* In-app billing (emulated)                                           */
/* ------------------------------------------------------------------ */

#define PURCHASES_FILE ROF_SAVE_DIR "/purchases.txt"

static volatile int purchase_state = 0; // APPSTORE_STATE_*

static int purchase_owned(const char *sku) {
    FILE *f = fopen(PURCHASES_FILE, "r");
    if (!f) return 0;
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, sku) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

static void purchase_record(const char *sku) {
    if (!sku || purchase_owned(sku)) return;
    FILE *f = fopen(PURCHASES_FILE, "a");
    if (f) {
        fprintf(f, "%s\n", sku);
        fclose(f);
    }
}

void purchasesInit(jmethodID id, va_list args) {
    l_info("purchasesInit");
    purchase_state = 2; // APPSTORE_STATE_SUCCESS: billing service "connected"
}

void purchasesFinalize(jmethodID id, va_list args) {
    purchase_state = 0;
}

jint getAppStoreState(jmethodID id, va_list args) {
    return purchase_state;
}

// void getPurchasesData(String[] skus) -> purchaseListFinished(...)
void getPurchasesData(jmethodID id, va_list args) {
    JavaDynArray *j_skus = va_arg(args, JavaDynArray *);
    int n = j_skus ? j_skus->len : 0;
    l_info("getPurchasesData(%i skus)", n);
    if (n <= 0) {
        rof_native.purchaseListFinished(rof_env, rof_clazz, 0, NULL, NULL, NULL, NULL, NULL);
        return;
    }
    jobjectArray title = (*rof_env)->NewObjectArray(rof_env, n, NULL, NULL);
    jobjectArray price = (*rof_env)->NewObjectArray(rof_env, n, NULL, NULL);
    jobjectArray type = (*rof_env)->NewObjectArray(rof_env, n, NULL, NULL);
    jobjectArray desc = (*rof_env)->NewObjectArray(rof_env, n, NULL, NULL);
    jobjectArray sku = (*rof_env)->NewObjectArray(rof_env, n, NULL, NULL);
    const jobject *in = j_skus->array;
    for (int i = 0; i < n; i++) {
        char *s = jstr_dup((jstring)in[i]);
        (*rof_env)->SetObjectArrayElement(rof_env, title, i, jstr_new(s ? s : ""));
        (*rof_env)->SetObjectArrayElement(rof_env, price, i, jstr_new("FREE"));
        (*rof_env)->SetObjectArrayElement(rof_env, type, i, jstr_new("inapp"));
        (*rof_env)->SetObjectArrayElement(rof_env, desc, i, jstr_new(""));
        (*rof_env)->SetObjectArrayElement(rof_env, sku, i, jstr_new(s ? s : ""));
        free(s);
    }
    rof_native.purchaseListFinished(rof_env, rof_clazz, n, title, price, type, desc, sku);
}

// void requestBilling(String sku)
void requestBilling(jmethodID id, va_list args) {
    char *sku = jstr_dup(va_arg(args, jstring));
    l_info("requestBilling(%s)", sku ? sku : "null");
    if (sku) {
        purchase_record(sku);
        defer(CB_PURCHASE_FINISHED, 0, sku); // APPSTORE_FUNC_COMP
    } else {
        defer(CB_PURCHASE_FINISHED, 2, NULL); // APPSTORE_FUNC_FAILED
    }
    free(sku);
}

// void restoreRequest(): report previously bought SKUs, then "restore done"
void restoreRequest(jmethodID id, va_list args) {
    l_info("restoreRequest");
    FILE *f = fopen(PURCHASES_FILE, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line)
                defer(CB_PURCHASE_FINISHED, 0, line);
        }
        fclose(f);
    }
    defer(CB_PURCHASE_FINISHED, 3, NULL); // APPSTORE_FUNC_COMP_RESTORE
}

/* ------------------------------------------------------------------ */
/* Storage / device info                                               */
/* ------------------------------------------------------------------ */

jboolean isMediaMounted(jmethodID id, va_list args) { return JNI_TRUE; }

// Free space checks before downloads: report plenty.
jlong getMediaRestSize(jmethodID id, va_list args) { return 4LL * 1024 * 1024 * 1024; }
jlong getDeviceRestSize(jmethodID id, va_list args) { return 4LL * 1024 * 1024 * 1024; }

jobject getAppVersionName(jmethodID id, va_list args) { return jstr_new("1.4.1"); }
jobject getDeviceName(jmethodID id, va_list args) { return jstr_new("PS Vita"); }

jobject getLanguage(jmethodID id, va_list args) {
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    const char *code = "en";
    switch (lang) {
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:   code = "ja"; break;
        case SCE_SYSTEM_PARAM_LANG_FRENCH:     code = "fr"; break;
        case SCE_SYSTEM_PARAM_LANG_SPANISH:    code = "es"; break;
        case SCE_SYSTEM_PARAM_LANG_GERMAN:     code = "de"; break;
        case SCE_SYSTEM_PARAM_LANG_ITALIAN:    code = "it"; break;
        case SCE_SYSTEM_PARAM_LANG_KOREAN:     code = "ko"; break;
        case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
        case SCE_SYSTEM_PARAM_LANG_CHINESE_S:  code = "zh"; break;
        default: break;
    }
    return jstr_new(code);
}

/* ------------------------------------------------------------------ */
/* Frame pacing (EGLView.m_Interval)                                   */
/* ------------------------------------------------------------------ */

static volatile int animation_interval = 33;

int rof_animation_interval(void) { return animation_interval; }

void setAnimationInterval(jmethodID id, va_list args) {
    int v = va_arg(args, int);
    if (v > 0 && v < 1000) {
        animation_interval = v;
        l_info("setAnimationInterval(%i)", v);
    }
}

/* ------------------------------------------------------------------ */
/* Background loading (AsyncTask -> native loadTask)                   */
/* ------------------------------------------------------------------ */

static void *load_task_thread(void *arg) {
    // Keep background loading off the game's core (0) and the decoder's (2).
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);
    l_info("loadTask: start");
    rof_native.loadTask(rof_env, rof_clazz);
    jni_frame_end(); // the native call returned: drop its JNI locals
    l_info("loadTask: end");
    return NULL;
}

void startLoadTask(jmethodID id, va_list args) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, load_task_thread, NULL) != 0) {
        l_error("startLoadTask: pthread_create failed, running inline");
        load_task_thread(NULL);
    }
    pthread_attr_destroy(&attr);
}

/* ------------------------------------------------------------------ */
/* Movies (MediaPlayer). No movie files ship with the game data, so    */
/* report them as finished right away.                                 */
/* ------------------------------------------------------------------ */

void setMovie(jmethodID id, va_list args) {
    char *f = jstr_dup(va_arg(args, jstring));
    l_info("setMovie(%s) - skipped", f ? f : "null");
    free(f);
}
void playMovie(jmethodID id, va_list args) {}
void stopMovie(jmethodID id, va_list args) {}
jboolean isMovieFinished(jmethodID id, va_list args) { return JNI_TRUE; }
jint getCurrentTime(jmethodID id, va_list args) { return 0; }

/* ------------------------------------------------------------------ */
/* Dialogs / UI helpers                                                */
/* ------------------------------------------------------------------ */

void alertDialog(jmethodID id, va_list args) {
    char *t = jstr_dup(va_arg(args, jstring));
    char *m = jstr_dup(va_arg(args, jstring));
    l_info("alertDialog: [%s] %s", t ? t : "", m ? m : "");
    free(t);
    free(m);
}

void showEndDialogView(jmethodID id, va_list args) {
    // Android "close the application?" prompt. Use the PS button instead.
    l_info("showEndDialogView (ignored)");
}

void CreateIndicator(jmethodID id, va_list args) {}
void DeleteIndicator(jmethodID id, va_list args) {}
jboolean DeleteIndicatorZ(jmethodID id, va_list args) { return JNI_TRUE; }
jboolean IsActiveIndicator(jmethodID id, va_list args) { return JNI_FALSE; }

void openURL(jmethodID id, va_list args) {}
void chartboostShowInterstitial(jmethodID id, va_list args) {}
void chartboostShowMoreApps(jmethodID id, va_list args) {}
void eventTracking(jmethodID id, va_list args) {}
void screenMeasurement(jmethodID id, va_list args) {}

/* ------------------------------------------------------------------ */
/* Google Play Games / Bluetooth / JSON (server features)              */
/* ------------------------------------------------------------------ */

void voidStub(jmethodID id, va_list args) {}
jboolean falseStub(jmethodID id, va_list args) { return JNI_FALSE; }
jint minusOneStub(jmethodID id, va_list args) { return -1; }
jobject nullStub(jmethodID id, va_list args) { return NULL; }

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

NameToMethodID nameToMethodId[] = {
    { 1,  "getFontWidthJava",          METHOD_TYPE_FLOAT },
    { 2,  "getFontHeightJava",         METHOD_TYPE_FLOAT },
    { 3,  "getFontAscentJava",         METHOD_TYPE_FLOAT },
    { 4,  "getFontDescentJava",        METHOD_TYPE_FLOAT },
    { 5,  "getFontLineHeightJava",     METHOD_TYPE_FLOAT },
    { 6,  "onTextDraw",                METHOD_TYPE_OBJECT },

    { 10, "purchasesInit",             METHOD_TYPE_VOID },
    { 11, "purchasesFinalize",         METHOD_TYPE_VOID },
    { 12, "getAppStoreState",          METHOD_TYPE_INT },
    { 13, "getPurchasesData",          METHOD_TYPE_VOID },
    { 14, "requestBilling",            METHOD_TYPE_VOID },
    { 15, "restoreRequest",            METHOD_TYPE_VOID },

    { 20, "isMediaMounted",            METHOD_TYPE_BOOLEAN },
    { 21, "getMediaRestSize",          METHOD_TYPE_LONG },
    { 22, "getDeviceRestSize",         METHOD_TYPE_LONG },
    { 23, "getAppVersionName",         METHOD_TYPE_OBJECT },
    { 24, "getDeviceName",             METHOD_TYPE_OBJECT },
    { 25, "getLanguage",               METHOD_TYPE_OBJECT },
    { 26, "setAnimationInterval",      METHOD_TYPE_VOID },
    { 27, "startLoadTask",             METHOD_TYPE_VOID },

    { 30, "setMovie",                  METHOD_TYPE_VOID },
    { 31, "playMovie",                 METHOD_TYPE_VOID },
    { 32, "stopMovie",                 METHOD_TYPE_VOID },
    { 33, "isMovieFinished",           METHOD_TYPE_BOOLEAN },
    { 34, "getCurrentTime",            METHOD_TYPE_INT },

    { 40, "alertDialog",               METHOD_TYPE_VOID },
    { 41, "showEndDialogView",         METHOD_TYPE_VOID },
    { 42, "CreateIndicator",           METHOD_TYPE_VOID },
    { 43, "DeleteIndicator",           METHOD_TYPE_VOID },
    { 44, "IsActiveIndicator",         METHOD_TYPE_BOOLEAN },
    { 45, "openURL",                   METHOD_TYPE_VOID },
    { 46, "chartboostShowInterstitial", METHOD_TYPE_VOID },
    { 47, "chartboostShowMoreApps",    METHOD_TYPE_VOID },
    { 48, "eventTracking",             METHOD_TYPE_VOID },
    { 49, "screenMeasurement",         METHOD_TYPE_VOID },

    { 50, "signIn",                    METHOD_TYPE_VOID },
    { 51, "isSignIn",                  METHOD_TYPE_BOOLEAN },
    { 52, "sendScore",                 METHOD_TYPE_BOOLEAN },
    { 53, "showLeaderboard",           METHOD_TYPE_BOOLEAN },
    { 54, "showAchievementboard",      METHOD_TYPE_BOOLEAN },
    { 55, "unlockAchievemnt",          METHOD_TYPE_VOID },
    { 56, "checkAchievement",          METHOD_TYPE_VOID },
    { 57, "resetAchievements",         METHOD_TYPE_VOID },

    { 60, "checkBluetooth",            METHOD_TYPE_BOOLEAN },
    { 61, "isBluetooth",               METHOD_TYPE_VOID },
    { 62, "stopDiscovery",             METHOD_TYPE_VOID },
    { 63, "BluetoothSendMessage",      METHOD_TYPE_VOID },
    { 64, "BluetoothConnect",          METHOD_TYPE_VOID },

    { 70, "resetJsonKeyDataNe",        METHOD_TYPE_VOID },
    { 71, "getJsonKeyArrayNe",         METHOD_TYPE_INT },
    { 72, "getJsonKeyArrayObjectNe",   METHOD_TYPE_INT },
    { 73, "getJsonKeyStrNe",           METHOD_TYPE_OBJECT },
    { 74, "getJsonKeyIntNe",           METHOD_TYPE_INT },
};

// A method may be listed in more than one table: FalsoJNI picks the table
// by the CallStatic<Type>Method the game uses, so this covers both.
MethodsBoolean methodsBoolean[] = {
    { 20, isMediaMounted },
    { 33, isMovieFinished },
    { 43, DeleteIndicatorZ },
    { 44, IsActiveIndicator },
    { 51, falseStub },
    { 52, falseStub },
    { 53, falseStub },
    { 54, falseStub },
    { 60, falseStub },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
    { 1, getFontWidthJava },
    { 2, getFontHeightJava },
    { 3, getFontAscentJava },
    { 4, getFontDescentJava },
    { 5, getFontLineHeightJava },
};
MethodsInt methodsInt[] = {
    { 12, getAppStoreState },
    { 34, getCurrentTime },
    { 71, minusOneStub },
    { 72, minusOneStub },
    { 74, minusOneStub },
};
MethodsLong methodsLong[] = {
    { 21, getMediaRestSize },
    { 22, getDeviceRestSize },
};
MethodsObject methodsObject[] = {
    { 6, onTextDraw },
    { 23, getAppVersionName },
    { 24, getDeviceName },
    { 25, getLanguage },
    { 73, nullStub },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 10, purchasesInit },
    { 11, purchasesFinalize },
    { 13, getPurchasesData },
    { 14, requestBilling },
    { 15, restoreRequest },
    { 26, setAnimationInterval },
    { 27, startLoadTask },
    { 30, setMovie },
    { 31, playMovie },
    { 32, stopMovie },
    { 40, alertDialog },
    { 41, showEndDialogView },
    { 42, CreateIndicator },
    { 43, DeleteIndicator },
    { 45, openURL },
    { 46, chartboostShowInterstitial },
    { 47, chartboostShowMoreApps },
    { 48, eventTracking },
    { 49, screenMeasurement },
    { 50, voidStub },
    { 55, voidStub },
    { 56, voidStub },
    { 57, voidStub },
    { 61, voidStub },
    { 62, voidStub },
    { 63, voidStub },
    { 64, voidStub },
    { 70, voidStub },
};

/*
 * JNI Fields (none are used by this game)
 */
NameToFieldID nameToFieldId[] = {
    { 0, "dummy", FIELD_TYPE_INT },
};
FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    { 0, 0 },
};
FieldsObject fieldsObject[] = {};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
