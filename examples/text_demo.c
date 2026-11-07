/* text_demo.c — the complex-script / bidi verification surface.
 *
 * One window, one job: put strings you cannot type on the owner's
 * keyboard IN FRONT OF YOUR EYES in both text widgets, so shaping,
 * bidi, per-script fallback, the caret, and click-to-place-caret can
 * all be checked without an input method:
 *
 *     ./build/examples/text_demo "مرحبا 漢字" "नमस्ते"
 *
 * Each argument becomes one frame holding
 *   - a WRAP Label  — passive layout: shaping, joining, bidi order,
 *                    fallback fonts, wrap behavior;
 *   - an Entry, prefilled — active layout: the same string through
 *     the editor's cluster-aware caret (set_text parks the caret at
 *     the END of the buffer; click anywhere to move it — the caret
 *     snaps to CLUSTER boundaries, not byte boundaries).
 * plus a small info line (bytes / codepoints / invalid sequences —
 * the UTF-8 parse the whole text stack stands on).
 *
 * No arguments: the built-in showcase (Latin, Arabic, CJK,
 * Devanagari, and one deliberately mixed bidi line) — same checks,
 * nothing to remember.
 *
 * The font size comes from the UI metrics (fdk_ui_font_px()) — the
 * same one number every FDK renderer derives — so the demo also
 * exercises TASK C: run it under FDK_UI_FONT_SIZE=12 / 24 / 32 and
 * the whole column grows and shrinks around the same strings.
 *
 * CI / rigs: --screenshot PATH renders one frame off the live
 * window surface and writes it as PNG, then exits 0 — the same
 * widget path, the same shaper, no human pointer needed:
 *
 *     Xvfb :94 & DISPLAY=:94 ./build/examples/text_demo \
 *         "مرحبا 漢字" "नमस्ते" --screenshot out.png
 *
 * (--screenshot=PATH also accepted. Every other dash-flag is
 * rejected with usage, so a typo never silently becomes a string.)
 *
 * Close the window or press ESC to exit.
 */

#include "example_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- argument handling ------------------------------------------------ */

typedef struct {
    const char **items;
    int count;
    const char *shot_path; /* --screenshot PATH, or NULL        */
} args_t;

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--screenshot OUT.png] [STRING...]\n"
            "  each STRING is shown in a Label and a prefilled Entry\n"
            "  no STRING: the built-in script showcase\n",
            argv0);
}

/* Returns 0 on success, nonzero on a usage error (message printed). */
static int args_parse(int argc, char **argv, args_t *out) {
    memset(out, 0, sizeof *out);
    out->items = calloc((size_t)argc + 1, sizeof *out->items);
    if (out->items == NULL) {
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --screenshot needs a path\n",
                        argv[0]);
                return 1;
            }
            out->shot_path = argv[++i];
        } else if (strncmp(argv[i], "--screenshot=", 13) == 0) {
            out->shot_path = argv[i] + 13;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "%s: unknown flag '%s'\n", argv[0],
                    argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            out->items[out->count++] = argv[i];
        }
    }
    if (out->shot_path != NULL && out->shot_path[0] == '\0') {
        fprintf(stderr, "%s: --screenshot needs a non-empty path\n",
                argv[0]);
        return 1;
    }
    return 0;
}

/* ---- the UTF-8 census (the info line) --------------------------------- */

typedef struct {
    size_t bytes;
    size_t codepoints;
    size_t invalid; /* byte positions that are not valid UTF-8    */
} utf8_census;

/* Counts codepoints the way the text stack decodes: sequences are
 * validated, an invalid byte is counted as one replacement and the
 * scan resynchronizes on the next lead byte. */
static void utf8_count(const char *s, utf8_census *out) {
    memset(out, 0, sizeof *out);
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0) {
        out->bytes++;
        unsigned char c = *p;
        size_t len = 1;
        if ((c & 0x80u) == 0x00u) {
            len = 1; /* ASCII                                     */
        } else if ((c & 0xE0u) == 0xC0u && (c & 0xFEu) != 0xC0u) {
            len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            len = 3;
        } else if ((c & 0xF8u) == 0xF0u && (c & 0xF8u) != 0xF8u) {
            len = 4;
        } else {
            out->invalid++; /* stray continuation / overlong lead  */
            p++;
            continue;
        }
        /* Continuation bytes must follow, and no interior NUL. */
        size_t got = 1;
        while (got < len && (p[got] & 0xC0u) == 0x80u) {
            got++;
        }
        if (got < len || p[len - 1] == 0) {
            out->invalid++;
            p++;
            continue;
        }
        out->codepoints++;
        p += len;
    }
}

/* ---- one argument's block ---------------------------------------------- */

static fdk_color dim(void) {
    return (fdk_color){ .r = 0.58f, .g = 0.61f, .b = 0.68f, .a = 1.0f };
}

/* Builds one frame: [info Label][wrap Label][prefilled Entry]. */
static bool build_block(fdk_widget *parent, fdk_font *font,
                        const char *text, int index) {
    utf8_census census;
    utf8_count(text, &census);

    char title[96];
    snprintf(title, sizeof title,
             "argument %d — %zu bytes, %zu codepoints%s", index,
             census.bytes, census.codepoints,
             census.invalid > 0 ? ", INVALID UTF-8" : "");

    fdk_widget *frame = NULL;
    if (!fdk_ok(fdk_frame_create(parent, font, title, &frame))) {
        return false;
    }
    fdk_widget_set_background_token(frame, FDK_TK_CONTROL_BACKGROUND);

    fdk_widget *info = NULL;
    if (!fdk_ok(fdk_label_create(frame, font, "", &info))) {
        return false;
    }
    fdk_label_set_color(info, dim());
    (void)fdk_label_set_text(info, "Label (passive layout) then Entry "
                                   "(click to place the caret):");
    fdk_widget_set_expand(info, true, false);

    /* The passive check: wrapping layout through the same shaping
     * walk that measures — what you see is what was measured. */
    fdk_widget *label = NULL;
    if (!fdk_ok(fdk_label_create(frame, font, text, &label))) {
        return false;
    }
    fdk_label_set_mode(label, FDK_LABEL_WRAP);
    fdk_widget_set_natural_size(label, 320, 0);
    fdk_widget_set_expand(label, true, false);

    /* The active check: prefilled editor. create parks the caret
     * at the END (after the last codepoint); clicking inside snaps
     * it to cluster boundaries — the shaped caret mapping. */
    fdk_widget *entry = NULL;
    if (!fdk_ok(fdk_entry_create(frame, font, text, &entry))) {
        return false;
    }
    fdk_widget_set_expand(entry, true, false);
    return true;
}

/* ---- main ---------------------------------------------------------------- */

static const char *k_showcase[] = {
    "Hello FDK — shaping, kerning, fallback in one column",
    "مرحبا",
    "漢字",
    "नमस्ते",
    "مرحبا 漢字 mixed bidi line — RTL run, CJK, then Latin",
};

int main(int argc, char **argv) {
    args_t args;
    if (args_parse(argc, argv, &args) != 0) {
        free(args.items);
        return 2;
    }
    const char **strings = args.items;
    int n_strings = args.count;
    if (n_strings == 0) {
        strings = k_showcase;
        n_strings = (int)(sizeof k_showcase / sizeof k_showcase[0]);
    }

    /* The UI metrics own the size (TASK C): fdk_ui_font_px() is the
     * one number every FDK renderer derives from. */
    fdk_font *ui = fdk_font_load_system_default(fdk_ui_font_px());
    if (ui == NULL) {
        fprintf(stderr,
                "text_demo: no system TrueType font found — the demo "
                "shapes real glyphs and needs a face (DejaVu Sans, "
                "Noto Sans, or $FDK_FONT_FILE pointing at one).\n");
        free(args.items);
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "text")) {
        fdk_font_destroy(ui);
        free(args.items);
        return 1;
    }

    /* Height follows the content: ~112 px per block + chrome. */
    fdk_i32 height = 150 + (fdk_i32)n_strings * 112;
    if (height > 1000) {
        height = 1000;
    }

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "", "text_demo", 720, height)) {
        fdk_font_destroy(ui);
        free(args.items);
        fdk_shutdown(ctx);
        return 1;
    }
    /* The helper's label would read "FDK  — text_demo" with an empty
     * number slot; the demo is a verification tool, not gallery
     * piece 13 — state its purpose instead. */
    (void)fdk_window_set_title(ex.window,
                               "FDK — text_demo: shaping / bidi / "
                               "fallback / caret");
    fdk_box_set_spacing(ex.content, 10);

    for (int i = 0; i < n_strings; i++) {
        if (!build_block(ex.content, ui, strings[i], i + 1)) {
            fprintf(stderr, "text_demo: block %d failed to build\n",
                    i + 1);
            fdk_example_close(&ex);
            fdk_font_destroy(ui);
            free(args.items);
            return 1;
        }
    }

    fdk_example_set_status(&ex, "click inside an Entry to place the "
                                "caret — no typing needed; ESC exits");

    if (args.shot_path != NULL) {
        /* CI mode: settle the window (map/configure/expose arrive
         * through the pump), then paint whatever is still damaged
         * and write the LIVE window surface out as PNG. */
        for (int i = 0; i < 8 && !ex.quit; i++) {
            (void)fdk_pump_events(ctx, 15);
        }
        if (fdk_widget_tree_has_damage(ex.root)) {
            (void)fdk_window_paint(ex.window);
        }
        fdk_surface *surf = NULL;
        if (!fdk_ok(fdk_window_get_surface(ex.window, &surf))) {
            fprintf(stderr, "text_demo: no window surface to save\n");
            fdk_example_close(&ex);
            fdk_font_destroy(ui);
            free(args.items);
            return 1;
        }
        fdk_result r = fdk_surface_save_png(surf, args.shot_path);
        if (!fdk_ok(r)) {
            fprintf(stderr, "text_demo: --screenshot failed (%d)\n",
                    (int)r);
            fdk_example_close(&ex);
            fdk_font_destroy(ui);
            free(args.items);
            return 1;
        }
        fdk_surface_info info;
        if (fdk_ok(fdk_surface_get_info(surf, &info))) {
            printf("[screenshot] %s (%dx%d, %d block%s)\n",
                   args.shot_path, (int)info.width, (int)info.height,
                   n_strings, n_strings == 1 ? "" : "s");
        } else {
            printf("[screenshot] %s (%d blocks)\n", args.shot_path,
                   n_strings);
        }
    } else {
        fdk_example_run(&ex);
    }

    fdk_example_close(&ex);
    fdk_font_destroy(ui);
    free(args.items);
    return 0;
}
