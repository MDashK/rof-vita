/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * File system redirection and game-specific libc overrides.
 *
 * The game builds three kinds of paths (see CFileExtend::_readOpen*):
 *   assets  -> AAssetManager_open("<file>")      -> ux0:data/rof/assets/<file>
 *   sdcard  -> "<sdcard>/<package>/<file>"         -> ux0:data/rof/<package>/<file>
 *   local   -> "/data/data/<package>/<file>"       -> ux0:data/rof/save/<file>
 * Only the last one needs rewriting; the sdcard root is handed to the game
 * directly in ApplicationInit.
 */

#include "rof/rof.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <SLES/OpenSLES.h>

#include "reimpl/io.h"
#include "utils/logger.h"
#include "utils/utils.h"

#define LOCAL_PREFIX "/data/data/" ROF_PKG

const char *rof_path(const char *in, char *out, size_t out_size) {
    static const size_t plen = sizeof(LOCAL_PREFIX) - 1;
    if (in && strncmp(in, LOCAL_PREFIX, plen) == 0 && (in[plen] == '/' || in[plen] == '\0')) {
        snprintf(out, out_size, "%s%s", ROF_SAVE_DIR, in + plen);
        return out;
    }
    return in;
}

void rof_fs_init(void) {
    // Make sure the redirected internal storage exists.
    mkdir(ROF_SAVE_DIR, 0777);
}

static FILE *rof_fopen(const char *path, const char *mode) {
    char buf[512];
    const char *p = rof_path(path, buf, sizeof(buf));
    FILE *cached = filecache_open(p, mode); // .bmv animations from RAM
    if (cached)
        return cached;
    FILE *f = fopen_soloader(p, mode);
    // newlib's default 1 KB buffer turns the game's many small reads into
    // thousands of memory card requests; read in 128 KB blocks instead.
    if (f && mode && mode[0] == 'r' && !strchr(mode, '+'))
        setvbuf(f, NULL, _IOFBF, 128 * 1024);
    return f;
}

// Timed fread for the "io" figure in the perf log.
static size_t rof_fread(void *ptr, size_t size, size_t n, FILE *f) {
    uint32_t t0 = perf_now();
    size_t r = fread(ptr, size, n, f);
    perf_add(PERF_IO, perf_now() - t0);
    perf_io_bytes(r * size);
    return r;
}

static int rof_mkdir(const char *path, mode_t mode) {
    char buf[512];
    const char *p = rof_path(path, buf, sizeof(buf));
    int ret = mkdir(p, mode);
    l_debug("mkdir(%s): %i", p, ret);
    return ret;
}

static int rof_remove(const char *path) {
    char buf[512];
    const char *p = rof_path(path, buf, sizeof(buf));
    l_debug("remove(%s)", p);
    return remove(p);
}

static int rof_chmod(const char *path, mode_t mode) {
    // No permissions on the Vita file system.
    return 0;
}

// --- Network: the SNK servers are long gone. Fail immediately so the game
// shows its own "unable to connect" window instead of waiting for timeouts.
static struct hostent *rof_gethostbyname(const char *name) {
    l_info("Network disabled: gethostbyname(%s) -> NULL", name ? name : "(null)");
    return NULL;
}

static struct servent *rof_getservbyname(const char *name, const char *proto) {
    return NULL;
}

static int rof_socket(int domain, int type, int protocol) {
    errno = ENETDOWN;
    return -1;
}

static int rof_connect(int fd, const void *addr, int len) {
    errno = ENETDOWN;
    return -1;
}

static in_addr_t rof_inet_addr(const char *cp) {
    return INADDR_NONE;
}

// --- Misc libc bits the boilerplate doesn't provide
static void rof_bcopy(const void *src, void *dst, size_t n) {
    memmove(dst, src, n);
}

static int rof_scanf(const char *fmt, ...) {
    return 0;
}

// --- C++ exceptions: the game's own unwinder asks where the .ARM.exidx
// table of the code at `pc` is. Answer for the loaded .so.
static uintptr_t rof_find_exidx(uintptr_t pc, int *pcount) {
    if (so_mod.exidx_base && pc >= so_mod.text_base && pc < so_mod.text_base + so_mod.text_size) {
        *pcount = (int)(so_mod.exidx_size / 8);
        return so_mod.exidx_base;
    }
    *pcount = 0;
    return 0;
}

// --- Accelerometer: CAccelerometer::callBack exits early when
// ALooper_prepare() returns NULL, so nothing else is needed.
static int ret0_rof(void) { return 0; }

// perf.c - timing wrappers around GL calls the game makes
void rof_glTexImage2D();
void rof_glTexSubImage2D();
void rof_glCompressedTexImage2D();
void rof_glDrawArrays();
void rof_glDrawElements();
void rof_glGenerateMipmap();
void rof_glDeleteTextures();
void rof_glClear();

so_default_dynlib rof_dynlib[] = {
    { "glTexImage2D", (uintptr_t)&rof_glTexImage2D },
    { "glTexSubImage2D", (uintptr_t)&rof_glTexSubImage2D },
    { "glCompressedTexImage2D", (uintptr_t)&rof_glCompressedTexImage2D },
    { "glDrawArrays", (uintptr_t)&rof_glDrawArrays },
    { "glDrawElements", (uintptr_t)&rof_glDrawElements },
    { "glGenerateMipmap", (uintptr_t)&rof_glGenerateMipmap },
    { "glDeleteTextures", (uintptr_t)&rof_glDeleteTextures },
    { "glClear", (uintptr_t)&rof_glClear },
    { "fopen", (uintptr_t)&rof_fopen },
    { "fread", (uintptr_t)&rof_fread },
    { "mkdir", (uintptr_t)&rof_mkdir },
    { "remove", (uintptr_t)&rof_remove },
    { "chmod", (uintptr_t)&rof_chmod },

    { "gethostbyname", (uintptr_t)&rof_gethostbyname },
    { "getservbyname", (uintptr_t)&rof_getservbyname },
    { "socket", (uintptr_t)&rof_socket },
    { "connect", (uintptr_t)&rof_connect },
    { "inet_addr", (uintptr_t)&rof_inet_addr },

    { "__gnu_Unwind_Find_exidx", (uintptr_t)&rof_find_exidx },

    { "bcopy", (uintptr_t)&rof_bcopy },
    { "scanf", (uintptr_t)&rof_scanf },

    { "ALooper_forThread", (uintptr_t)&ret0_rof },
    { "ALooper_prepare", (uintptr_t)&ret0_rof },
    { "ALooper_pollAll", (uintptr_t)&ret0_rof },
    { "ASensorManager_getInstance", (uintptr_t)&ret0_rof },
    { "ASensorManager_getDefaultSensor", (uintptr_t)&ret0_rof },
    { "ASensorManager_createEventQueue", (uintptr_t)&ret0_rof },
    { "ASensorEventQueue_enableSensor", (uintptr_t)&ret0_rof },
    { "ASensorEventQueue_setEventRate", (uintptr_t)&ret0_rof },
    { "ASensorEventQueue_getEvents", (uintptr_t)&ret0_rof },
    { "ASensor_getMinDelay", (uintptr_t)&ret0_rof },

    { "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
};

const size_t rof_dynlib_count = sizeof(rof_dynlib) / sizeof(rof_dynlib[0]);
