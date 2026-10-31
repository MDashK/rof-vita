/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * RAM cache for the game's .bmv animations.
 *
 * OCMediaMovie (menu tunnel background, title, "CLEAR"/"FAILED" overlays,
 * results...) re-opens its .bmv file for every 20 fps animation frame and
 * reads ~0.2-1 MB of almost full-frame deltas each time. On a phone's flash
 * that is fine; from the Vita memory card it costs tens of milliseconds per
 * frame, and as the game falls behind it decodes *more* frames to catch up,
 * so menus spiral down to ~4 fps.
 *
 * Here the movies are read once into RAM by a low priority background thread
 * (preloaded at boot, or on first use) and later fopen() calls are served
 * from memory through funopen(). Until a file is cached the normal file path
 * is used, so nothing ever waits on the loader.
 */

#include "rof/rof.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define CACHE_BUDGET     (96u << 20)  // total RAM for cached movies
#define CACHE_MAX_FILE   (48u << 20)  // bigger files stay on the card
#define CACHE_ENTRIES    16
#define READ_CHUNK       (512u << 10)

enum { ST_QUEUED, ST_LOADING, ST_READY, ST_SKIPPED };

typedef struct {
    char path[256];
    uint8_t *data;
    size_t size;
    int state;
    int refs;          // open FILEs reading from data
    uint32_t last_use;
} CacheEntry;

typedef struct {
    CacheEntry *e;
    size_t pos;
} CacheHandle;

static CacheEntry entries[CACHE_ENTRIES];
static int n_entries = 0;
static size_t cache_used = 0;
static uint32_t use_clock = 0;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cache_cond = PTHREAD_COND_INITIALIZER;
static int loader_started = 0;

static int is_cacheable(const char *path) {
    size_t n = strlen(path);
    return n > 4 && strcasecmp(path + n - 4, ".bmv") == 0;
}

static CacheEntry *find_entry(const char *path) {
    for (int i = 0; i < n_entries; i++)
        if (strcasecmp(entries[i].path, path) == 0)
            return &entries[i];
    return NULL;
}

// Caller holds cache_lock.
static CacheEntry *queue_entry(const char *path) {
    CacheEntry *e = find_entry(path);
    if (e)
        return e;
    if (n_entries >= CACHE_ENTRIES || strlen(path) >= sizeof(entries[0].path))
        return NULL;
    e = &entries[n_entries++];
    memset(e, 0, sizeof(*e));
    strcpy(e->path, path);
    e->state = ST_QUEUED;
    pthread_cond_signal(&cache_cond);
    return e;
}

// Free least recently used, unused movies until `need` more bytes fit.
// Caller holds cache_lock.
static int make_room(size_t need) {
    while (cache_used + need > CACHE_BUDGET) {
        CacheEntry *victim = NULL;
        for (int i = 0; i < n_entries; i++) {
            CacheEntry *e = &entries[i];
            if (e->state == ST_READY && e->refs == 0 &&
                (!victim || e->last_use < victim->last_use))
                victim = e;
        }
        if (!victim)
            return 0;
        l_info("filecache: dropping %s (%u KB)", victim->path, (unsigned)(victim->size >> 10));
        free(victim->data);
        victim->data = NULL;
        cache_used -= victim->size;
        victim->size = 0;
        victim->state = ST_QUEUED; // can be loaded again later
    }
    return 1;
}

static void load_entry(CacheEntry *e) {
    // Runs without the lock held for the actual reading.
    FILE *f = fopen(e->path, "rb");
    if (!f) {
        pthread_mutex_lock(&cache_lock);
        e->state = ST_SKIPPED;
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    pthread_mutex_lock(&cache_lock);
    int ok = size > 0 && (size_t)size <= CACHE_MAX_FILE && make_room((size_t)size);
    uint8_t *data = ok ? malloc((size_t)size) : NULL;
    if (data)
        cache_used += (size_t)size;
    pthread_mutex_unlock(&cache_lock);

    if (!data) {
        fclose(f);
        l_info("filecache: not caching %s (%ld KB)", e->path, size >> 10);
        pthread_mutex_lock(&cache_lock);
        e->state = ST_SKIPPED;
        pthread_mutex_unlock(&cache_lock);
        return;
    }

    uint32_t t0 = perf_now();
    size_t got = 0;
    while (got < (size_t)size) {
        size_t want = (size_t)size - got;
        if (want > READ_CHUNK)
            want = READ_CHUNK;
        size_t r = fread(data + got, 1, want, f);
        if (r == 0)
            break;
        got += r;
    }
    fclose(f);

    pthread_mutex_lock(&cache_lock);
    if (got == (size_t)size) {
        e->data = data;
        e->size = (size_t)size;
        e->last_use = ++use_clock;
        e->state = ST_READY;
        l_info("filecache: %s cached (%u KB in %u ms, %u MB used)", e->path,
               (unsigned)(size >> 10), (unsigned)((perf_now() - t0) / 1000),
               (unsigned)(cache_used >> 20));
    } else {
        free(data);
        cache_used -= (size_t)size;
        e->state = ST_SKIPPED;
        l_warn("filecache: short read on %s", e->path);
    }
    pthread_mutex_unlock(&cache_lock);
}

static void *loader_thread(void *arg) {
    (void)arg;
    // Lowest normal user priority: never compete with the game or audio.
    sceKernelChangeThreadPriority(0, 180);
    pthread_mutex_lock(&cache_lock);
    for (;;) {
        CacheEntry *next = NULL;
        for (int i = 0; i < n_entries; i++) {
            if (entries[i].state == ST_QUEUED) {
                next = &entries[i];
                break;
            }
        }
        if (!next) {
            pthread_cond_wait(&cache_cond, &cache_lock);
            continue;
        }
        next->state = ST_LOADING;
        pthread_mutex_unlock(&cache_lock);
        load_entry(next);
        pthread_mutex_lock(&cache_lock);
    }
    return NULL;
}

static void start_loader(void) {
    if (loader_started)
        return;
    loader_started = 1;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, loader_thread, NULL);
}

/* ---- FILE interface over a cached buffer --------------------------------- */

static int h_read(void *cookie, char *buf, int n) {
    CacheHandle *h = cookie;
    if (n <= 0 || h->pos >= h->e->size)
        return 0;
    size_t left = h->e->size - h->pos;
    if ((size_t)n > left)
        n = (int)left;
    memcpy(buf, h->e->data + h->pos, (size_t)n);
    h->pos += (size_t)n;
    return n;
}

static fpos_t h_seek(void *cookie, fpos_t off, int whence) {
    CacheHandle *h = cookie;
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)h->pos : (long)h->e->size;
    long np = base + (long)off;
    if (np < 0)
        return (fpos_t)-1;
    h->pos = (size_t)np;
    return (fpos_t)np;
}

static int h_close(void *cookie) {
    CacheHandle *h = cookie;
    pthread_mutex_lock(&cache_lock);
    h->e->refs--;
    pthread_mutex_unlock(&cache_lock);
    free(h);
    return 0;
}

FILE *filecache_open(const char *path, const char *mode) {
    if (!path || !mode || mode[0] != 'r' || strchr(mode, '+') || !is_cacheable(path))
        return NULL;

    pthread_mutex_lock(&cache_lock);
    start_loader();
    CacheEntry *e = queue_entry(path);
    if (!e || e->state != ST_READY) {
        pthread_mutex_unlock(&cache_lock);
        return NULL; // not cached (yet): use the card
    }
    e->refs++;
    e->last_use = ++use_clock;
    pthread_mutex_unlock(&cache_lock);

    CacheHandle *h = malloc(sizeof(*h));
    if (!h)
        goto undo;
    h->e = e;
    h->pos = 0;
    FILE *f = funopen(h, h_read, NULL, h_seek, h_close);
    if (!f) {
        free(h);
        goto undo;
    }
    setvbuf(f, NULL, _IONBF, 0); // reads go straight to h_read
    return f;

undo:
    pthread_mutex_lock(&cache_lock);
    e->refs--;
    pthread_mutex_unlock(&cache_lock);
    return NULL;
}

// Queue the animations the menus and results use, so they are in RAM
// before they are first shown.
void filecache_preload(void) {
    static const char *names[] = {
        "title.bmv", "back.bmv", "StageClear_anim.bmv", "Failed_anim.bmv",
        "FullChain.bmv", "unlock.bmv",
    };
    pthread_mutex_lock(&cache_lock);
    start_loader();
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s", ROF_SONGS_DIR, names[i]);
        queue_entry(path);
    }
    pthread_mutex_unlock(&cache_lock);
}
