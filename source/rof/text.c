/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Text rendering that replaces MainActivity's Android Paint/Canvas code
 * (onTextDraw, getFont*Java) with FreeType.
 *
 * Fonts are tried in order for every character, so Latin text uses the
 * first font and Japanese song titles fall back to the Japanese one:
 *   1. ux0:data/rof/font.ttf        (optional, user supplied - e.g. Roboto)
 *   2. sa0:data/font/pvf/ltn0.pvf   (Vita system Latin font)
 *   3. sa0:data/font/pvf/jpn0.pvf   (Vita system Japanese font)
 *   4. sa0:data/font/pvf/kr0.pvf, cn0.pvf
 *
 * Rendered glyphs are cached per (code point, pixel size): the game measures
 * every centred string several times per frame and draws outlined text as up
 * to 17 offset passes, so FreeType only ever rasterises a glyph once.
 */

#include "rof/rof.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "utils/logger.h"
#include "utils/utils.h"

#define MAX_FACES 6

static FT_Library ft;
static FT_Face faces[MAX_FACES];
static int face_px[MAX_FACES]; // pixel size currently set on each face
static int n_faces = 0;
static pthread_mutex_t text_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- glyph cache -------------------------------------------------------- */

#define GLYPH_SLOTS      4096          // power of two
#define GLYPH_PROBE      16
#define GLYPH_CACHE_MAX  (6 * 1024 * 1024)

typedef struct {
    uint32_t cp;
    uint16_t px;
    uint8_t used;
    int16_t left, top;     // bitmap offset from pen position / baseline
    uint16_t w, h;
    float advance;         // pixels
    uint8_t *cov;          // w*h coverage (0..255), NULL for empty glyphs
} Glyph;

static Glyph glyphs[GLYPH_SLOTS];
static size_t glyph_bytes = 0;

static void glyph_cache_clear(void) {
    for (int i = 0; i < GLYPH_SLOTS; i++) {
        free(glyphs[i].cov);
        glyphs[i].cov = NULL;
        glyphs[i].used = 0;
    }
    glyph_bytes = 0;
}

static inline uint32_t glyph_hash(uint32_t cp, int px) {
    return ((cp * 2654435761u) ^ ((uint32_t)px * 40503u)) & (GLYPH_SLOTS - 1);
}

static void try_add_face(const char *path) {
    if (n_faces >= MAX_FACES || !file_exists(path))
        return;
    FT_Face f;
    if (FT_New_Face(ft, path, 0, &f) == 0) {
        face_px[n_faces] = 0;
        faces[n_faces++] = f;
        l_info("Font loaded: %s", path);
    } else {
        l_warn("Font could not be loaded: %s", path);
    }
}

int text_init(void) {
    if (FT_Init_FreeType(&ft) != 0) {
        l_error("FreeType init failed");
        return -1;
    }
    try_add_face(DATA_PATH "font.ttf");
    try_add_face(DATA_PATH "font.otf");
    try_add_face("sa0:data/font/pvf/ltn0.pvf");
    try_add_face("sa0:data/font/pvf/jpn0.pvf");
    try_add_face("sa0:data/font/pvf/kr0.pvf");
    try_add_face("sa0:data/font/pvf/cn0.pvf");
    if (n_faces == 0) {
        l_error("No usable font found - text will be invisible");
        return -1;
    }
    return 0;
}

// Decode one UTF-8 code point; returns bytes consumed (>= 1).
static int utf8_next(const unsigned char *s, uint32_t *cp) {
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

// Pick the first face that has the glyph and size it. Caller holds the lock.
static FT_Face face_for(uint32_t cp, int px, FT_UInt *glyph) {
    int idx = -1;
    FT_UInt g = 0;
    for (int i = 0; i < n_faces; i++) {
        g = FT_Get_Char_Index(faces[i], cp);
        if (g) { idx = i; break; }
    }
    if (idx < 0) {
        if (n_faces == 0)
            return NULL;
        idx = 0;
        g = FT_Get_Char_Index(faces[0], '?');
    }
    if (face_px[idx] != px) {
        FT_Set_Pixel_Sizes(faces[idx], 0, px);
        face_px[idx] = px;
    }
    *glyph = g;
    return faces[idx];
}

// Cached glyph lookup / rasterisation. Caller holds the lock.
static const Glyph *glyph_get(uint32_t cp, int px) {
    uint32_t h = glyph_hash(cp, px);
    Glyph *slot = NULL;
    for (int i = 0; i < GLYPH_PROBE; i++) {
        Glyph *g = &glyphs[(h + i) & (GLYPH_SLOTS - 1)];
        if (!g->used) { slot = g; break; }
        if (g->cp == cp && g->px == px)
            return g;
    }
    if (!slot || glyph_bytes > GLYPH_CACHE_MAX) {
        // Probe window full or cache too big: start over (rare).
        glyph_cache_clear();
        slot = &glyphs[h];
    }

    slot->used = 1;
    slot->cp = cp;
    slot->px = (uint16_t)px;
    slot->w = slot->h = 0;
    slot->left = slot->top = 0;
    slot->advance = 0.0f;
    slot->cov = NULL;

    FT_UInt gi;
    FT_Face f = face_for(cp, px, &gi);
    if (!f || FT_Load_Glyph(f, gi, FT_LOAD_RENDER) != 0)
        return slot;

    FT_GlyphSlot gs = f->glyph;
    FT_Bitmap *bm = &gs->bitmap;
    slot->advance = gs->advance.x / 64.0f;
    slot->left = (int16_t)gs->bitmap_left;
    slot->top = (int16_t)gs->bitmap_top;
    if (bm->width && bm->rows) {
        slot->cov = malloc(bm->width * bm->rows);
        if (slot->cov) {
            slot->w = (uint16_t)bm->width;
            slot->h = (uint16_t)bm->rows;
            for (unsigned r = 0; r < bm->rows; r++) {
                const unsigned char *src = bm->buffer + r * bm->pitch;
                uint8_t *dst = slot->cov + r * bm->width;
                if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
                    for (unsigned c = 0; c < bm->width; c++)
                        dst[c] = ((src[c >> 3] >> (7 - (c & 7))) & 1) * 255;
                } else {
                    memcpy(dst, src, bm->width);
                }
            }
            glyph_bytes += bm->width * bm->rows + sizeof(Glyph);
        }
    }
    return slot;
}

static float face_metric(int size, int which) {
    if (n_faces == 0 || size <= 0)
        return 0.0f;
    FT_Face f = faces[0];
    float upem = f->units_per_EM ? (float)f->units_per_EM : 1000.0f;
    switch (which) {
        case 0: return -(float)f->ascender * size / upem;   // ascent (negative)
        case 1: return -(float)f->descender * size / upem;  // descent (positive)
        default: {
            float b = -(float)f->bbox.yMin * size / upem;   // bottom (positive)
            float d = -(float)f->descender * size / upem;
            return b > d ? b : d;
        }
    }
}

float text_ascent(int size)  { return face_metric(size, 0); }
float text_descent(int size) { return face_metric(size, 1); }
float text_bottom(int size)  { return face_metric(size, 2); }

float text_measure(int size, const char *utf8) {
    if (!utf8 || size <= 0 || n_faces == 0)
        return 0.0f;
    uint32_t t0 = perf_now();
    pthread_mutex_lock(&text_lock);
    float w = 0.0f;
    const unsigned char *s = (const unsigned char *)utf8;
    while (*s) {
        uint32_t cp;
        s += utf8_next(s, &cp);
        w += glyph_get(cp, size)->advance;
    }
    pthread_mutex_unlock(&text_lock);
    perf_add(PERF_TEXT_MEASURE, perf_now() - t0);
    return w;
}

/* ---- blending ----------------------------------------------------------- */

// Exact round(x / 255) for 0 <= x <= 65535, without a division (the Vita's
// Cortex-A9 has no hardware divider).
static inline unsigned div255(unsigned x) {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

static uint32_t inv_alpha[256]; // round(65536 / a)

static void blend_init(void) {
    if (inv_alpha[1])
        return;
    for (unsigned a = 1; a < 256; a++)
        inv_alpha[a] = (65536 + a / 2) / a;
}

// "Source over" blend of a coverage value into a non-premultiplied ARGB
// pixel, which is what Bitmap.getPixels() hands back on Android.
static inline uint32_t blend(uint32_t dst, uint32_t argb, unsigned cov) {
    unsigned sa = div255((argb >> 24) * cov);
    if (sa == 0)
        return dst;
    unsigned da = dst >> 24;
    if (sa == 255 || da == 0)
        return (sa << 24) | (argb & 0xFFFFFF);
    unsigned dw = div255(da * (255 - sa));
    unsigned oa = sa + dw;
    uint32_t inv = inv_alpha[oa];
    unsigned sr = (argb >> 16) & 0xFF, sg = (argb >> 8) & 0xFF, sb = argb & 0xFF;
    unsigned dr = (dst >> 16) & 0xFF,  dg = (dst >> 8) & 0xFF,  db = dst & 0xFF;
    unsigned r = ((sr * sa + dr * dw) * inv + 32768) >> 16;
    unsigned gg = ((sg * sa + dg * dw) * inv + 32768) >> 16;
    unsigned b = ((sb * sa + db * dw) * inv + 32768) >> 16;
    if (r > 255) r = 255;
    if (gg > 255) gg = 255;
    if (b > 255) b = 255;
    return (oa << 24) | (r << 16) | (gg << 8) | b;
}

void text_draw(uint32_t *pixels, int stride, int width, int height,
               float scale, int size, uint32_t argb, float x, float y,
               const char *utf8) {
    if (!utf8 || !*utf8 || size <= 0 || n_faces == 0)
        return;
    int px = (int)lroundf(size * scale);
    if (px <= 0)
        return;
    blend_init();

    pthread_mutex_lock(&text_lock);
    float pen_x = x * scale;
    float baseline = y * scale;
    const unsigned char *s = (const unsigned char *)utf8;
    while (*s) {
        uint32_t cp;
        s += utf8_next(s, &cp);
        const Glyph *g = glyph_get(cp, px);
        if (g->cov) {
            int ox = (int)lroundf(pen_x) + g->left;
            int oy = (int)lroundf(baseline) - g->top;
            int c0 = ox < 0 ? -ox : 0;
            int c1 = ox + g->w > width ? width - ox : g->w;
            for (int row = 0; row < g->h; row++) {
                int ty = oy + row;
                if (ty < 0 || ty >= height)
                    continue;
                const uint8_t *src = g->cov + row * g->w;
                uint32_t *dst = pixels + ty * stride + ox;
                for (int col = c0; col < c1; col++) {
                    unsigned cov = src[col];
                    if (cov)
                        dst[col] = blend(dst[col], argb, cov);
                }
            }
        }
        pen_x += g->advance;
    }
    pthread_mutex_unlock(&text_lock);
}
