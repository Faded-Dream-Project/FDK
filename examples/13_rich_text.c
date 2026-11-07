/* 13_rich_text.c — the §23/B2c attribute surface, end to end.
 *
 * One window, five rows, every attribute family drawn live through
 * the public entry points (fdk_font_measure_attrs_utf8 /
 * fdk_surface_draw_attrs_utf8 — the same twins the Label rides):
 *
 *   SIZE      "small normal BIG normal" — the <small>/<big> tags
 *   FAMILY    mono / serif / sans — the <span font=...> tag, each
 *             word resolved through fontconfig at its own face
 *   DECORATE  the four underline styles (single, double, low, the
 *             error squiggle), a highlight box, and an underline
 *             in its own color
 *   SCRIPT    x<sup>2</sup> and H<sub>2</sub>O — rise + shrink
 *   LINK      two <a href> links; the console prints hover and
 *             click through fdk_attrs_link_at
 *
 * The console prints each row's measured width — the size/family
 * rows measure at their OWN metrics (the point of the pipeline:
 * a big run is BIG ink, not baseline-stretched ink).
 */

#include "example_window.h"

#include "fdk/fdk_attr.h"
#include "fdk/fdk_text.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *markup;
    char *plain;
    fdk_span *spans;
    size_t n_spans;
    fdk_attr_list attrs;
    fdk_text_metrics metrics;
} row;

static fdk_example app; /* ctx/window/quit — the helper's whole
                          * state; the rows ride below */
static row g_rows[5];
static fdk_font *g_font;
static fdk_color g_fg, g_bg;
static char g_status[160];

static const fdk_i32 ROW_Y[5] = {70, 124, 178, 232, 286};
static const fdk_i32 PEN_X = 28;

static void row_build(row *r, const char *markup) {
    r->markup = markup;
    if (!fdk_ok(fdk_markup_parse_ex(markup, &r->plain, &r->spans,
                                    &r->n_spans, &r->attrs))) {
        r->plain = NULL;
        r->spans = NULL;
        r->n_spans = 0;
        fdk_attr_list_init(&r->attrs);
    }
    fdk_font_measure_attrs_utf8(g_font, r->plain,
                                r->plain ? strlen(r->plain) : 0,
                                r->spans, r->n_spans, &r->attrs,
                                &r->metrics);
}

/* Repaint trigger: invalidate the window's surface whole. */
static void example_repaint(fdk_window *w) {
    fdk_surface *s;
    if (fdk_ok(fdk_window_get_surface(w, &s))) {
        fdk_surface_invalidate_all(s);
    }
}

static void paint(fdk_window *w) {
    fdk_surface *s;
    if (!fdk_ok(fdk_window_get_surface(w, &s))) {
        return;
    }
    fdk_surface_fill(s, g_bg);
    fdk_surface_draw_utf8(s, g_font,
                          "size / family / decoration / script / links",
                          strlen("size / family / decoration / script /"
                                 " links"),
                          46, 40, g_fg);
    for (int i = 0; i < 5; i++) {
        row *r = &g_rows[i];
        if (r->plain == NULL) {
            continue;
        }
        fdk_surface_draw_attrs_utf8(s, g_font, r->plain,
                                    strlen(r->plain), r->spans,
                                    r->n_spans, &r->attrs, PEN_X,
                                    ROW_Y[i], g_fg);
    }
    fdk_surface_draw_utf8(s, g_font, g_status,
                          strlen(g_status), 28, 340, g_fg);
    fdk_surface_present(s);
}

static void window_event(fdk_window *w, const fdk_event_data *ev,
                         void *user) {
    if (ev->type == FDK_EVENT_WINDOW_EXPOSE ||
        ev->type == FDK_EVENT_WINDOW_CONFIGURE) {
        paint(w);
    } else if (ev->type == FDK_EVENT_POINTER_MOTION ||
               ev->type == FDK_EVENT_POINTER_BUTTON_DOWN) {
        fdk_pointf p;
        if (ev->type == FDK_EVENT_POINTER_MOTION) {
            p = ev->pointer.position;
        } else {
            p = ev->pointer_button.position;
        }
        row *r = &g_rows[4]; /* the LINK row */
        char href[128];
        if (r->plain != NULL &&
            fdk_attrs_link_at(g_font, r->plain, strlen(r->plain),
                              r->spans, r->n_spans, &r->attrs, PEN_X,
                              ROW_Y[4], (fdk_i32)p.x, (fdk_i32)p.y,
                              href, sizeof href)) {
            snprintf(g_status, sizeof g_status, "%s: %s",
                     ev->type == FDK_EVENT_POINTER_BUTTON_DOWN
                         ? "clicked"
                         : "hover",
                     href);
            example_repaint(w);
        } else if (g_status[0] != '\0') {
            g_status[0] = '\0';
            example_repaint(w);
        }
    }
    (void)user;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!fdk_example_init(&app.ctx, "13")) {
        return 1;
    }

    g_font = fdk_font_load_system_default(18);
    if (g_font == NULL) {
        fprintf(stderr, "13: no font — the rich-text demo needs "
                        "real glyph metrics\n");
        fdk_shutdown(app.ctx);
        return 1;
    }
    g_fg = (fdk_color){0.92f, 0.92f, 0.95f, 1};
    g_bg = (fdk_color){0.10f, 0.10f, 0.13f, 1};
    g_status[0] = '\0';

    fdk_window_options wopts = {
        .title = "FDK 13 — rich text attributes",
        .width = 640,
        .height = 400,
    };
    if (!fdk_ok(fdk_window_create(app.ctx, &wopts, &app.window))) {
        fprintf(stderr, "fdk_window_create failed\n");
        fdk_shutdown(app.ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);
    (void)fdk_window_set_decorated(app.window, true);

    row_build(&g_rows[0],
              "<small>small</small> normal <big>BIG</big> normal");
    row_build(&g_rows[1],
              "<span font=\"monospace\">mono</span> "
              "<span font=\"serif\">serif</span> "
              "<span font=\"sans-serif\">sans</span>");
    row_build(&g_rows[2],
              "<span underline=\"single\">single</span> "
              "<span underline=\"double\">double</span> "
              "<span underline=\"low\">low</span> "
              "<span underline=\"error\">error</span> "
              "<span bgcolor=\"#3a2a4a\">boxed</span> "
              "<span underline_color=\"#e08040\" "
              "underline=\"single\">warm</span>");
    row_build(&g_rows[3], "x<sup>2</sup> + H<sub>2</sub>O");
    row_build(&g_rows[4],
              "see <a href=\"https://fdk.home\">the FDK home page</a> "
              "or <a href=\"https://fdk.docs\">the docs</a>");

    for (int i = 0; i < 5; i++) {
        if (g_rows[i].plain != NULL) {
            printf("row %d measured %d px: %s\n", i,
                   g_rows[i].metrics.advance_width,
                   g_rows[i].markup);
        }
    }

    fdk_window_show(app.window);
    fdk_example_run(&app);

    for (int i = 0; i < 5; i++) {
        fdk_markup_attrs_free(&g_rows[i].attrs);
        fdk_markup_free(g_rows[i].plain, g_rows[i].spans);
    }
    fdk_font_destroy(g_font);
    fdk_example_close(&app);
    return 0;
}
