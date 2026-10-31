/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Song availability report.
 *
 * Some DLC songs were never preserved. The game itself already copes with
 * that: before a song starts, AppMain::CheckTuneResData() looks for its
 * chart (.rfn) and music (.rof) files and, if they are missing, tries to
 * download them (which fails cleanly now that the servers are gone).
 * So anyone who still has those files can just drop them into
 * ux0:data/rof/com.snkplaymore.android009/ and the songs become playable.
 *
 * This file only writes ux0:data/rof/missing_songs.txt, listing which songs
 * are incomplete and the exact file names that are needed.
 */

#include "rof/rof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "utils/logger.h"
#include "utils/utils.h"

#define NUM_SONGS 82
#define REPORT_FILE DATA_PATH "missing_songs.txt"

static int load(const char *path, unsigned char **buf, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024) { fclose(f); return -1; }
    *buf = malloc(n);
    *size = fread(*buf, 1, n, f);
    fclose(f);
    return 0;
}

static int has_ext(const char *s, size_t n, const char *ext) {
    size_t e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++)
        if (tolower((unsigned char)s[n - e + i]) != ext[i]) return 0;
    return 1;
}

// The game maps "name.ogg" in song info to "name.rof" on disk.
static void disk_name(const char *ref, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", ref);
    size_t n = strlen(out);
    if (n > 4 && has_ext(out, n, ".ogg"))
        strcpy(out + n - 4, ".rof");
}

static int data_file_exists(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s", ROF_SONGS_DIR, name);
    if (file_exists(path)) return 1;
    snprintf(path, sizeof(path), "%sassets/%s", DATA_PATH, name);
    return file_exists(path);
}

void songs_report(void) {
    FILE *out = fopen(REPORT_FILE, "w");
    if (!out) {
        l_warn("Could not write %s", REPORT_FILE);
        return;
    }
    fprintf(out, "THE RHYTHM OF FIGHTERS - songs that cannot be played yet\n");
    fprintf(out, "=========================================================\n\n");
    fprintf(out, "Copy the files listed below into:\n  %s\n", ROF_SONGS_DIR);
    fprintf(out, "and the song becomes playable. This list is rebuilt every time the game starts.\n\n");

    int missing_songs = 0;
    for (int k = 0; k < NUM_SONGS; k++) {
        char info[64];
        snprintf(info, sizeof(info), "musicinfo_%03d.rmd", k);
        char path[512];
        snprintf(path, sizeof(path), "%s%s", ROF_SONGS_DIR, info);
        unsigned char *b = NULL;
        size_t size = 0;
        if (load(path, &b, &size) < 0) {
            snprintf(path, sizeof(path), "%sassets/%s", DATA_PATH, info);
            if (load(path, &b, &size) < 0) {
                fprintf(out, "[%03d] song info file %s is missing\n\n", k, info);
                missing_songs++;
                continue;
            }
        }

        // Title: length-prefixed string right after the 4-byte version.
        char title[128] = "?";
        if (size > 5 && b[4] > 0 && 5u + b[4] <= size) {
            int n = b[4] < sizeof(title) - 1 ? b[4] : (int)sizeof(title) - 1;
            memcpy(title, b + 5, n);
            title[n] = 0;
        }

        // Every length-prefixed string ending in .rfn/.ogg/.obm is a file.
        char needed[16][96];
        int n_needed = 0, essential_missing = 0;
        for (size_t i = 5; i < size && n_needed < 16; i++) {
            size_t n = b[i];
            if (n < 5 || n > 80 || i + 1 + n > size) continue;
            const char *s = (const char *)b + i + 1;
            int printable = 1;
            for (size_t j = 0; j < n; j++)
                if (s[j] < 32 || s[j] > 126) { printable = 0; break; }
            if (!printable) continue;
            int is_chart = has_ext(s, n, ".rfn");
            int is_music = has_ext(s, n, ".ogg");
            if (!is_chart && !is_music && !has_ext(s, n, ".obm")) continue;

            char ref[96], name[96];
            snprintf(ref, sizeof(ref), "%.*s", (int)n, s);
            disk_name(ref, name, sizeof(name));
            if (!data_file_exists(name)) {
                // Charts and music are what the game checks before playing.
                if (is_chart || is_music) essential_missing = 1;
                snprintf(needed[n_needed++], sizeof(needed[0]), "%s%s", name,
                         (is_chart || is_music) ? "" : "  (image, optional)");
            }
            i += n;
        }
        free(b);

        if (essential_missing) {
            missing_songs++;
            fprintf(out, "[%03d] %s\n", k, title);
            for (int i = 0; i < n_needed; i++)
                fprintf(out, "      - %s\n", needed[i]);
            fprintf(out, "\n");
        }
    }

    if (missing_songs == 0)
        fprintf(out, "All songs are complete!\n");
    fclose(out);
    l_info("Song check: %i of %i songs are incomplete (see %s)", missing_songs, NUM_SONGS, REPORT_FILE);
}
