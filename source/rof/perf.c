/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Lightweight frame profiler: every few seconds one line goes to log.txt with
 * the frame rate and where the time went (game step, swap, text, texture
 * uploads, draw calls, memory). Cheap enough to leave on.
 */

#include "rof/rof.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "utils/logger.h"

#define PERF_REPORT_US (5 * 1000 * 1000)

typedef struct {
    uint32_t count;
    uint64_t us;
    uint32_t max_us;
} PerfSlot;

static PerfSlot slots[PERF_COUNT];
static uint64_t upload_bytes;
static uint64_t io_bytes;
static uint32_t frames;
static uint32_t frame_max_us;
static uint32_t last_frame;
static uint32_t period_start;

static const char *names[PERF_COUNT] = {
    "step", "swap", "textdraw", "measure", "texupload", "draw", "io",
};

void perf_add(PerfId id, uint32_t us) {
    PerfSlot *s = &slots[id];
    s->count++;
    s->us += us;
    if (us > s->max_us)
        s->max_us = us;
}

void perf_upload_bytes(uint32_t bytes) {
    upload_bytes += bytes;
}

void perf_io_bytes(uint32_t bytes) {
    io_bytes += bytes;
}

extern unsigned jni_frames_freed_total(void);
extern int jni_frames_pending(void);

void perf_frame_end(void) {
    uint32_t now = perf_now();
    if (!period_start) {
        period_start = last_frame = now;
        return;
    }
    uint32_t ft = now - last_frame;
    last_frame = now;
    if (ft > frame_max_us)
        frame_max_us = ft;
    frames++;

    uint32_t elapsed = now - period_start;
    if (elapsed < PERF_REPORT_US)
        return;

    char line[512];
    int n = snprintf(line, sizeof(line), "fps %.1f (worst %ums)",
                     frames * 1000000.0f / elapsed, frame_max_us / 1000);
    for (int i = 0; i < PERF_COUNT && n < (int)sizeof(line); i++) {
        PerfSlot *s = &slots[i];
        if (!s->count)
            continue;
        // avg ms per frame, calls per frame, worst single call
        n += snprintf(line + n, sizeof(line) - n, " | %s %.2fms/f x%.1f max%.1f",
                      names[i], (float)s->us / frames / 1000.0f,
                      (float)s->count / frames, s->max_us / 1000.0f);
    }
    struct mallinfo mi = mallinfo();
    if (n < (int)sizeof(line))
        snprintf(line + n, sizeof(line) - n, " | upload %uKB/f | read %uKB | heap %uMB | jni freed %u pending %d",
                 (unsigned)(upload_bytes / frames / 1024), (unsigned)(io_bytes / 1024),
                 (unsigned)(mi.uordblks >> 20),
                 jni_frames_freed_total(), jni_frames_pending());
    l_info("perf: %s", line);

    memset(slots, 0, sizeof(slots));
    upload_bytes = 0;
    io_bytes = 0;
    frames = 0;
    frame_max_us = 0;
    period_start = now;
}

/* ---- GL wrappers (installed through rof_dynlib) -------------------------- */

#include <vitaGL.h>

static uint32_t texel_bytes(GLenum format, GLenum type) {
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1)
        return 2;
    switch (format) {
        case GL_ALPHA: case GL_LUMINANCE: return 1;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_RGB: return 3;
        default: return 4;
    }
}

/*
 * Texture shape tracking. vitaGL's glTexSubImage2D first copies the whole old
 * texture to fresh memory when the texture was used in a recent frame (so
 * the GPU can keep reading the old copy). That copy *reads* GPU-mapped,
 * uncached memory - ~15 ms for the 1 MB menu background movie, every frame.
 * The game always replaces those textures entirely, so a full-size, level-0
 * sub-image update is turned into glTexImage2D, which only writes.
 */
#define TEX_TRACK 8192

typedef struct {
    uint16_t w, h;
    GLenum internalformat, format, type;
    uint8_t valid, has_mips;
} TexShape;

static TexShape tex_shapes[TEX_TRACK];

static TexShape *bound_shape(GLenum target) {
    if (target != GL_TEXTURE_2D)
        return NULL;
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    return (id > 0 && id < TEX_TRACK) ? &tex_shapes[id] : NULL;
}

void rof_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                      GLsizei height, GLint border, GLenum format, GLenum type,
                      const void *data) {
    uint32_t t0 = perf_now();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
    perf_add(PERF_TEX_UPLOAD, perf_now() - t0);
    perf_upload_bytes(width * height * texel_bytes(format, type));

    TexShape *ts = bound_shape(target);
    if (ts) {
        if (level == 0) {
            ts->valid = 1;
            ts->has_mips = 0;
            ts->w = (uint16_t)width;
            ts->h = (uint16_t)height;
            ts->internalformat = (GLenum)internalformat;
            ts->format = format;
            ts->type = type;
        } else {
            ts->has_mips = 1;
        }
    }
}

void rof_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLsizei width, GLsizei height, GLenum format, GLenum type,
                         const void *data) {
    uint32_t t0 = perf_now();
    TexShape *ts = (level == 0 && xoffset == 0 && yoffset == 0) ? bound_shape(target) : NULL;
    if (ts && ts->valid && !ts->has_mips && ts->w == width && ts->h == height &&
        ts->format == format && ts->type == type) {
        glTexImage2D(target, 0, (GLint)ts->internalformat, width, height, 0, format, type, data);
    } else {
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, data);
    }
    perf_add(PERF_TEX_UPLOAD, perf_now() - t0);
    perf_upload_bytes(width * height * texel_bytes(format, type));
}

void rof_glGenerateMipmap(GLenum target) {
    TexShape *ts = bound_shape(target);
    if (ts)
        ts->has_mips = 1;
    glGenerateMipmap(target);
}

void rof_glDeleteTextures(GLsizei n, const GLuint *textures) {
    for (GLsizei i = 0; textures && i < n; i++)
        if (textures[i] > 0 && textures[i] < TEX_TRACK)
            tex_shapes[textures[i]].valid = 0;
    glDeleteTextures(n, textures);
}

void rof_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                GLsizei width, GLsizei height, GLint border,
                                GLsizei imageSize, const void *data) {
    uint32_t t0 = perf_now();
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    TexShape *ts = bound_shape(target);
    if (ts)
        ts->valid = 0; // compressed: never take the full-update shortcut
    perf_add(PERF_TEX_UPLOAD, perf_now() - t0);
    perf_upload_bytes(imageSize);
}

// Set once the game has drawn anything (see main.c: the vitaGL splash keeps
// animating until the first scene begins).
volatile int rof_game_drew = 0;

void rof_glClear(GLbitfield mask) {
    rof_game_drew = 1;
    glClear(mask);
}

void rof_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    rof_game_drew = 1;
    uint32_t t0 = perf_now();
    glDrawArrays(mode, first, count);
    perf_add(PERF_DRAW, perf_now() - t0);
}

void rof_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    rof_game_drew = 1;
    uint32_t t0 = perf_now();
    glDrawElements(mode, count, type, indices);
    perf_add(PERF_DRAW, perf_now() - t0);
}
