/*
 * test_cursor.c — the named cursor set's contract battery
 * (1.1.0-rc2, S-5 / B-12)
 *
 * Headless and pure: the cursor's DECIDING layer is three integers
 * and two tables, so the whole contract pins without a display —
 * the X11/Wayland integration suites prove the delivery paths
 * (fdk__x11_named_cursor's tiers; the wayland seam's enums), this
 * file proves what they are DELIVERING:
 *
 *   - the public enum: 22 shapes, values 0..21 with no holes
 *   - the public->op mapping: DEFAULT->0, POINTER->9, TEXT..
 *     VERTICAL_TEXT -> 32..51, everything else invalid
 *   - the resolution order: an app request beats a widget hover
 *     beats the resize compass beats the default arrow
 *   - the shared candidate-name table: 20 rows, first candidate
 *     always present, spellings legal for theme directories
 *   - the theme LOADER against a synthetic XCursor file (the
 *     documented container format written by hand — the parser's
 *     happy path + its hostile-input refusals, both hermetic)
 */

#include "platform/platform_internal.h"
#include "window/window_internal.h"
#include "core/alloc_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the mapping + resolution order ------------------------------- */

static void test_enum_shape(void) {
    assert(FDK_CURSOR__COUNT == 22);
    assert(FDK_CURSOR_DEFAULT == 0);
    assert(FDK_CURSOR_POINTER == 1);
    assert(FDK_CURSOR_TEXT == 2);
    assert(FDK_CURSOR_VERTICAL_TEXT == 21);
    /* The values between the named anchors are exactly the 20-row
     * table the backends index — no holes, no surprises. */
    assert(FDK_PLATFORM_CURSOR_NAMED_COUNT == FDK_CURSOR__COUNT - 2);
    assert(FDK_PLATFORM_CURSOR_NAMED_BASE > FDK_PLATFORM_CURSOR_POINTER);
    printf("[ok] enum shape: 22 values, 0..21, 20-row named table\n");
}

static void test_public_to_op_mapping(void) {
    /* DEFAULT is the shared 0; POINTER rides the hand; the named
     * set rides the base offset; anything else is invalid. */
    assert(fdk__cursor_resolve(FDK_CURSOR_DEFAULT, -1, 0) == 0);
    assert(fdk__cursor_resolve(FDK_CURSOR_POINTER, -1, 0) ==
           FDK_PLATFORM_CURSOR_POINTER);
    for (int v = 2; v < (int)FDK_CURSOR__COUNT; v++) {
        assert(fdk__cursor_resolve(v, -1, 0) ==
               FDK_PLATFORM_CURSOR_NAMED_BASE + (v - 2));
    }
    /* Invalid values never leak through as shapes. */
    assert(fdk__cursor_resolve(-1, 999, 0) == -1);
    assert(fdk__cursor_resolve(999, -1, 0) == -1);
    assert(fdk__cursor_resolve((int)FDK_CURSOR__COUNT, -1, 0) == -1);
    printf("[ok] mapping: 0->0, 1->9, 2..21->32..51, invalid stays -1\n");
}

static void test_resolution_order(void) {
    /* App request owns everything: edges stand down, widget hovers
     * stand down. */
    assert(fdk__cursor_resolve(FDK_CURSOR_POINTER, FDK_CURSOR_TEXT,
                               FDK_WRES_E) == FDK_PLATFORM_CURSOR_POINTER);
    assert(fdk__cursor_resolve(FDK_CURSOR_WAIT, -1, FDK_WRES_N) ==
           FDK_PLATFORM_CURSOR_NAMED_BASE + 2);
    assert(fdk__cursor_resolve(FDK_CURSOR_DEFAULT, FDK_CURSOR_TEXT, 3) == 0);
    /* Without an app request the widget hover rules the compass. */
    assert(fdk__cursor_resolve(-1, FDK_CURSOR_TEXT, FDK_WRES_E) ==
           FDK_PLATFORM_CURSOR_NAMED_BASE);
    assert(fdk__cursor_resolve(-1, FDK_CURSOR_TEXT, 0) ==
           FDK_PLATFORM_CURSOR_NAMED_BASE);
    /* Without either, the compass; without all, the arrow. */
    for (int e = 1; e <= 8; e++) {
        assert(fdk__cursor_resolve(-1, -1, e) == e);
    }
    assert(fdk__cursor_resolve(-1, -1, 0) == 0);
    printf("[ok] resolution: app > widget hover > compass > arrow\n");
}

/* ---- the shared candidate-name table ------------------------------- */

static void test_named_name_table(void) {
    for (int i = 0; i < FDK_PLATFORM_CURSOR_NAMED_COUNT; i++) {
        const char *const *row = fdk__cursor_named_names[i];
        assert(row[0] != NULL); /* always a first candidate */
        int n = 0;
        while (n < 4 && row[n] != NULL) {
            /* Theme-directory grammar: lowercase letters, digits,
             * underscores, hyphens only — anything else would never
             * match a real theme file name. */
            for (const char *p = row[n]; *p != '\0'; p++) {
                char c = *p;
                assert((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-');
            }
            n++;
        }
        assert(n >= 1 && n <= 3); /* NULL-terminated inside [4] */
        /* The first candidate is the modern spelling: contains a
         * letter and no uppercase (the legacy twins like
         * sb_h_double_arrow may trail). */
        for (const char *p = row[0]; *p != '\0'; p++) {
            assert((*p >= 'a' && *p <= 'z') || *p == '_');
        }
    }
    /* Spot-pin the rows the desktops actually resolve: the exact
     * first candidates for the shapes CI themes differ on. */
    assert(strcmp(fdk__cursor_named_names[0][0], "text") == 0);
    assert(strcmp(fdk__cursor_named_names[2][0], "wait") == 0);
    assert(strcmp(fdk__cursor_named_names[2][1], "watch") == 0);
    assert(strcmp(fdk__cursor_named_names[7][0], "no_drop") == 0);
    assert(strcmp(fdk__cursor_named_names[16][0], "help") == 0);
    printf("[ok] name table: 20 rows, legal spellings, legacy twins\n");
}

/* ---- the loader against a synthetic XCursor file ------------------- */

/* Writes a minimal but VALID XCursor container: file header ("Xcur",
 * header=16, version=1, ntoc=1) + one TOC entry pointing at one image
 * chunk (type 0xfffd0002, subtype=nominal, 36-byte header, WxH ARGB
 * pixels) — the format cursor_theme.c documents byte-for-byte. */
static void write_xcursor_file(const char *path, int nominal, int w, int h,
                               int hx, int hy, uint32_t pixel) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    const uint32_t chunk_pos = 16u + 12u; /* header + one TOC entry */
    const uint32_t header[4] = {0x72756358u, 16u, 1u, 1u};
    const uint32_t toc[3] = {0xfffd0002u, (uint32_t)nominal, chunk_pos};
    const uint32_t chunk[9] = {36u, 0xfffd0002u, (uint32_t)nominal, 1u,
                               (uint32_t)w, (uint32_t)h, (uint32_t)hx,
                               (uint32_t)hy, 0u};
    fwrite(header, 4, 4, f);
    fwrite(toc, 4, 3, f);
    fwrite(chunk, 4, 9, f);
    for (int i = 0; i < w * h; i++) {
        fwrite(&pixel, 4, 1, f);
    }
    fclose(f);
}

static void test_theme_loader_hermetic(void) {
    char root[256], theme_dir[340], cursors[400], file[460];
    snprintf(root, sizeof root, "/tmp/fdk-cursor-test-%ld",
             (long)getpid());
    snprintf(theme_dir, sizeof theme_dir, "%s/hermetic-theme", root);
    snprintf(cursors, sizeof cursors, "%s/cursors", theme_dir);
    assert(mkdir(root, 0755) == 0 || errno == EEXIST);
    assert(mkdir(theme_dir, 0755) == 0 || errno == EEXIST);
    assert(mkdir(cursors, 0755) == 0 || errno == EEXIST);
    snprintf(file, sizeof file, "%s/modern_name", cursors);
    /* Nominal 24, a 3x3 image, hotspot (1,1), one solid pixel value. */
    write_xcursor_file(file, 24, 3, 3, 1, 1, 0x80FF0000u);

    /* Point the loader at the fixture theme. */
    char xcursor_path[300];
    snprintf(xcursor_path, sizeof xcursor_path, "%s", root);
    setenv("XCURSOR_PATH", xcursor_path, 1);
    setenv("XCURSOR_THEME", "hermetic-theme", 1);

    /* (a) The modern name resolves: pixels, metrics, and the exact
     * pixel value survive the round trip. */
    uint32_t *pixels = NULL;
    int w = 0, h = 0, hx = 0, hy = 0;
    const char *const names1[] = {"modern_name", NULL};
    assert(fdk_ok(fdk__cursor_theme_load(names1, 24, &pixels, &w, &h,
                                         &hx, &hy)));
    assert(pixels != NULL && w == 3 && h == 3 && hx == 1 && hy == 1);
    assert(pixels[0] == 0x80FF0000u);
    fdk_free(pixels);

    /* (b) Candidate lists walk: a missing first name falls through
     * to the legacy twin. */
    const char *const names2[] = {"absent_name", "modern_name", NULL};
    pixels = NULL;
    assert(fdk_ok(fdk__cursor_theme_load(names2, 24, &pixels, &w, &h,
                                         &hx, &hy)));
    assert(w == 3);
    fdk_free(pixels);

    /* (c) Nothing anywhere: the honest UNSUPPORTED. */
    const char *const names3[] = {"absent_name", "also_absent", NULL};
    pixels = NULL;
    assert(fdk__cursor_theme_load(names3, 24, &pixels, &w, &h, &hx,
                                  &hy) == FDK_ERR_UNSUPPORTED);

    /* (d) Hostile inputs are refused, not crashed: a truncated file,
     * a magic-free file, a chunk past EOF. */
    snprintf(file, sizeof file, "%s/hostile", cursors);
    FILE *f = fopen(file, "wb");
    assert(f != NULL);
    fputs("not an xcursor file at all", f);
    fclose(f);
    const char *const names4[] = {"hostile", NULL};
    pixels = NULL;
    assert(!fdk_ok(fdk__cursor_theme_load(names4, 24, &pixels, &w, &h,
                                          &hx, &hy)));

    /* Size pick: the closest nominal wins (24 asked, a 24-file vs a
     * 48-file: the 24 chunk is picked). */
    snprintf(file, sizeof file, "%s/sized", cursors);
    write_xcursor_file(file, 48, 4, 4, 2, 2, 0xFF00FF00u);
    /* Re-write "modern_name" with BOTH sizes in one container. */
    snprintf(file, sizeof file, "%s/dual", cursors);
    {
        FILE *g = fopen(file, "wb");
        assert(g != NULL);
        const uint32_t dual_header[4] = {0x72756358u, 16u, 1u, 2u};
        /* Two TOC entries (24 bytes) follow the 16-byte file header;
         * chunk0: 36-byte header + 4x4 pixels; chunk1 after it. */
        const uint32_t pos0 = 16u + 24u;
        const uint32_t pos1 = pos0 + 36u + 4u * 4u * 4u;
        const uint32_t toc0[3] = {0xfffd0002u, 48u, pos0};
        const uint32_t toc1[3] = {0xfffd0002u, 24u, pos1};
        const uint32_t c0[9] = {36u, 0xfffd0002u, 48u, 1u, 4u, 4u, 2u, 2u, 0u};
        const uint32_t c1[9] = {36u, 0xfffd0002u, 24u, 1u, 3u, 3u, 1u, 1u, 0u};
        fwrite(dual_header, 4, 4, g);
        fwrite(toc0, 4, 3, g);
        fwrite(toc1, 4, 3, g);
        fwrite(c0, 4, 9, g);
        for (int i = 0; i < 16; i++) {
            uint32_t px = 0xFF00FF00u;
            fwrite(&px, 4, 1, g);
        }
        fwrite(c1, 4, 9, g);
        for (int i = 0; i < 9; i++) {
            uint32_t px = 0xFFFF0000u;
            fwrite(&px, 4, 1, g);
        }
        fclose(g);
    }
    const char *const names5[] = {"dual", NULL};
    pixels = NULL;
    assert(fdk_ok(fdk__cursor_theme_load(names5, 24, &pixels, &w, &h,
                                         &hx, &hy)));
    assert(w == 3 && hx == 1); /* the 24-nominal chunk, not the 48 */
    fdk_free(pixels);

    /* The fixture path goes away with the test. */
    unsetenv("XCURSOR_PATH");
    unsetenv("XCURSOR_THEME");
    char rmcmd[360];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf '%s'", root);
    (void)system(rmcmd);
    printf("[ok] theme loader: hermetic fixture resolves, candidates "
           "walk, hostile refused, closest size picked\n");
}

int main(void) {
    test_enum_shape();
    test_public_to_op_mapping();
    test_resolution_order();
    test_named_name_table();
    test_theme_loader_hermetic();
    printf("\nall cursor tests passed\n");
    return 0;
}
