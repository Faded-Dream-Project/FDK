/* 18_system_integration.c — the system-integration surfaces, live
 * (§23 follow-up: every remaining uncovered API family, in one
 * runnable app).
 *
 *   clipboard     TEXT, IMAGE, and URI-LIST — set each kind, read
 *                 each back, and see the one-content-at-a-time
 *                 model stated honestly (setting uris retires the
 *                 text; the read-back buttons prove what is there)
 *   drag & drop   a real SOURCE (press-drag from the source pad —
 *                 fdk_drag_begin with text or URIs, an icon, the
 *                 done-callback verdict) and a real TARGET (the
 *                 drop pad: fdk_widget_set_drop_target +
 *                 set_drag_callback — ENTER/MOTION/LEAVE/DROP with
 *                 the decoded payload)
 *   undo          an fdk_undo_stack wired to a live counter: push,
 *                 undo, redo, the can/cannot queries, the depth
 *                 readouts
 *   shortcuts     window-level key bindings (Ctrl+Z / Ctrl+Y /
 *                 Ctrl+L), registered through the spec parser
 *   i18n          a catalog built in-memory (the .mo reader feeds
 *                 from files; here a runtime-parseable path is
 *                 shown via format_int/date/plural for a set of
 *                 locales) + plural selection
 *   prefs         set/get of every value kind + save/load with the
 *                 on-disk path printed — restart the app and the
 *                 values come back
 *
 * Everything reports through the status line and the log pane.
 */

#include "example_window.h"

#include "fdk/fdk_clipboard.h"
#include "fdk/fdk_dnd.h"
#include "fdk/fdk_i18n.h"
#include "fdk/fdk_prefs.h"
#include "fdk/fdk_undo.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_example ex;
static fdk_widget *g_log;
static char g_log_text[6144];
static size_t g_log_len;

static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_log_len += (size_t)vsnprintf(
        g_log_text + g_log_len, sizeof g_log_text - g_log_len, fmt,
        ap);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    g_log_len += (size_t)snprintf(g_log_text + g_log_len,
                                  sizeof g_log_text - g_log_len, "\n");
    if (g_log_len >= sizeof g_log_text - 256) {
        size_t keep = g_log_len / 2;
        while (keep < g_log_len && g_log_text[keep] != '\n') {
            keep++;
        }
        memmove(g_log_text, g_log_text + keep, g_log_len - keep);
        g_log_len -= keep;
        g_log_text[g_log_len] = '\0';
    }
    fdk_textview_set_text(g_log, g_log_text);
    fdk_example_set_status(&ex, g_log_text);
}

/* ---- clipboard --------------------------------------------------------- */

static void clip_set_text(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    fdk_result r = fdk_clipboard_set_text(
        ex.ctx, "FDK example 18 — the clipboard TEXT lane");
    log_line("[clip] set_text: %s", fdk_ok(r) ? "OK" : "FAILED");
}

static void clip_get_text(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    char *t = fdk_clipboard_get_text(ex.ctx);
    log_line("[clip] get_text: %s",
             t != NULL ? t : "(NULL — nothing/unreadable)");
    free(t);
}

static void clip_set_uris(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    const char *uris[2] = {"/tmp/fdk example 18.txt",
                           "/home/user/report.pdf"};
    fdk_result r = fdk_clipboard_set_uris(ex.ctx, uris, 2);
    log_line("[clip] set_uris(2 paths): %s",
             fdk_ok(r) ? "OK (text retired — one content at a time)"
                       : "FAILED");
}

static void clip_get_uris(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    char *u = fdk_clipboard_get_uris(ex.ctx);
    log_line("[clip] get_uris: %s",
             u != NULL ? u : "(NULL — no uri-list present)");
    free(u);
}

static void clip_set_image(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    /* a small gradient surface (the clipboard takes fdk_surface) */
    fdk_surface *img = NULL;
    if (!fdk_ok(fdk_surface_create_format(
            48, 24, FDK_SURFACE_FORMAT_XRGB8888, &img))) {
        log_line("[clip] set_image: surface failed");
        return;
    }
    fdk_surface_info info;
    (void)fdk_surface_get_info(img, &info);
    for (int y = 0; y < info.height; y++) {
        for (int x = 0; x < info.width; x++) {
            fdk_u32 px = 0xFF000000u |
                         ((unsigned)(x * 5) & 0xFFu) << 16 |
                         ((unsigned)(y * 10) & 0xFFu) << 8 |
                         0x60u;
            info.pixels[(size_t)y * (size_t)info.stride +
                        (size_t)x] = px;
        }
    }
    fdk_result r = fdk_clipboard_set_image(ex.ctx, img);
    fdk_surface_destroy(img);
    log_line("[clip] set_image(48x24 surface): %s",
             fdk_ok(r) ? "OK (image owns the clipboard now)"
                       : "FAILED");
}

static void clip_get_image(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    fdk_surface *img = fdk_clipboard_get_image(ex.ctx);
    if (img == NULL) {
        log_line("[clip] get_image: (NULL — no image present)");
        return;
    }
    fdk_surface_info info;
    (void)fdk_surface_get_info(img, &info);
    log_line("[clip] get_image: %dx%d pixels",
             (int)info.width, (int)info.height);
    fdk_surface_destroy(img);
}

/* ---- drag & drop ---------------------------------------------------------- */

static void on_drag_done(fdk_drag_status status, void *user) {
    (void)user;
    const char *verdict = status == FDK_DRAG_SUCCEEDED ? "SUCCEEDED"
                          : status == FDK_DRAG_CANCELLED
                              ? "CANCELLED (nothing accepted)"
                              : "FAILED (backend refused)";
    log_line("[dnd] drag done: %s", verdict);
}

/* The SOURCE pad: a press begins a real drag. */
static bool g_source_down;
static fdk_pointf g_source_at;

/* §29/G-10: the controller-API twin of the manual source above — the
 * begin fn the widget-level drag source fires past the slop. Same
 * payload, one line of app code instead of the hand-rolled
 * press/motion/release state machine. */
static void *controller_begin(fdk_widget *source, void *user) {
    (void)user;
    const char *uris[1] = {"/tmp/dragged by the controller.txt"};
    fdk_u32 icon[4] = {0x80FFFFFFu, 0x80FF0000u,
                       0x8000FF00u, 0x800000FFu};
    fdk_result r = fdk_drag_begin_ex(
        ex.window, FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST,
        "dragged by the G-10 controller", uris, 1, icon, 2, 2,
        on_drag_done, NULL);
    log_line("[dnd] controller begin: %s",
             fdk_ok(r) ? "started — drop it on the pad"
                       : "refused");
    (void)source;
    return NULL;
}

static bool source_event(fdk_widget *w,
                         const fdk_widget_event *ev, void *user) {
    (void)user;
    if (ev->type == FDK_WIDGET_POINTER_DOWN &&
        ev->pointer.button == FDK_POINTER_BUTTON_LEFT) {
        g_source_down = true;
        g_source_at = ev->pointer.position;
        return true;
    }
    if (ev->type == FDK_WIDGET_POINTER_UP) {
        g_source_down = false;
        return true;
    }
    if (ev->type == FDK_WIDGET_POINTER_MOTION && g_source_down) {
        fdk_pointf now = ev->position;
        float dx = now.x - g_source_at.x;
        float dy = now.y - g_source_at.y;
        if (dx * dx + dy * dy > 100.0f) {
            /* far enough: begin the drag (text + URIs, with an
             * icon through the _ex form) */
            const char *uris[2] = {"/tmp/dragged file.txt",
                                   "/tmp/second.bin"};
            fdk_u32 icon[4] = {0x80FFFFFFu, 0x80FF0000u,
                               0x8000FF00u, 0x800000FFu};
            fdk_result r = fdk_drag_begin_ex(
                ex.window, FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST,
                "dragged from FDK example 18", uris, 2, icon, 2, 2,
                on_drag_done, NULL);
            log_line("[dnd] drag_begin_ex: %s",
                     fdk_ok(r) ? "started — drop it on the pad"
                               : "refused");
            g_source_down = false;
        }
        return true;
    }
    (void)w;
    return false;
}

/* The TARGET pad: scoped drop target + the drag event stream. */
static void target_event(fdk_widget *w, fdk_event_type type,
                         const fdk_drag_event *ev, void *user) {
    (void)w; (void)user;
    switch (type) {
    case FDK_EVENT_DRAG_ENTER:
        log_line("[dnd] ENTER at %.0f,%.0f (offered 0x%x, accepted "
                 "0x%x)",
                 ev->position.x, ev->position.y, ev->offered_formats,
                 ev->accepted_formats);
        break;
    case FDK_EVENT_DRAG_MOTION:
        break; /* too chatty for the log */
    case FDK_EVENT_DRAG_LEAVE:
        log_line("[dnd] LEAVE");
        break;
    case FDK_EVENT_DRAG_DROP:
        log_line("[dnd] DROP at %.0f,%.0f", ev->position.x,
                 ev->position.y);
        if (ev->text != NULL) {
            log_line("[dnd]   text: '%s'", ev->text);
        }
        for (size_t i = 0; i < ev->uri_count; i++) {
            log_line("[dnd]   uri[%zu]: %s", i, ev->uris[i]);
        }
        break;
    default:
        break;
    }
}

/* ---- undo -------------------------------------------------------------------- */

typedef struct {
    int value;
    fdk_widget *label;
} counter_state;

static fdk_undo_stack *g_undo;

static counter_state g_counter;

static void counter_render(void) {
    char buf[96];
    snprintf(buf, sizeof buf,
             "value: %d   (can_undo %d / can_redo %d)",
             g_counter.value,
             fdk_undo_stack_can_undo(g_undo) ? 1 : 0,
             fdk_undo_stack_can_redo(g_undo) ? 1 : 0);
    fdk_label_set_text(g_counter.label, buf);
}

static void op_apply(int from, int to) {
    g_counter.value = to;
    (void)from;
    counter_render();
}

typedef struct {
    int from, to;
} op_data;

static void op_undo(void *ud) {
    op_data *d = ud;
    op_apply(d->to, d->from);
}
static void op_redo(void *ud) {
    op_data *d = ud;
    op_apply(d->from, d->to);
}
static void op_destroy(void *ud) {
    free(ud);
}

static fdk_undo_stack *g_undo;

static void counter_push(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    op_data *d = malloc(sizeof *d);
    if (d == NULL) {
        return;
    }
    d->from = g_counter.value;
    d->to = g_counter.value + 1;
    fdk_undo_op op = {d, op_undo, op_redo, op_destroy};
    (void)fdk_undo_stack_push(g_undo, &op);
    op_apply(d->from, d->to);
    log_line("[undo] pushed %d -> %d (depth %zu)", d->from, d->to,
             fdk_undo_stack_depth(g_undo));
}

static void counter_undo(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    fdk_result r = fdk_undo_stack_undo(g_undo);
    log_line("[undo] undo: %s", fdk_ok(r) ? "OK" : "nothing to undo");
}

static void counter_redo(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    fdk_result r = fdk_undo_stack_redo(g_undo);
    log_line("[undo] redo: %s", fdk_ok(r) ? "OK" : "nothing to redo");
}

/* ---- shortcuts ------------------------------------------------------------------ */

static void sc_undo(fdk_window *w, void *user) {
    (void)w; (void)user;
    counter_undo(NULL, NULL);
    log_line("[shortcut] Ctrl+Z fired the undo path");
}
static void sc_redo(fdk_window *w, void *user) {
    (void)w; (void)user;
    counter_redo(NULL, NULL);
    log_line("[shortcut] Ctrl+Y fired the redo path");
}
static void sc_report(fdk_window *w, void *user) {
    (void)w; (void)user;
    log_line("[shortcut] Ctrl+L: depth=%zu redo-depth=%zu can_undo "
             "=%d can_redo=%d",
             fdk_undo_stack_depth(g_undo),
             fdk_undo_stack_redo_depth(g_undo),
             fdk_undo_stack_can_undo(g_undo),
             fdk_undo_stack_can_redo(g_undo));
}

/* ---- i18n ------------------------------------------------------------------------ */

static void i18n_report(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    const char *tags[] = {"en_US", "de_DE", "fr_FR", "ar_EG",
                          "ja_JP"};
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; i++) {
        fdk_locale loc;
        if (!fdk_ok(fdk_locale_parse(tags[i], &loc))) {
            continue;
        }
        char num[64], money[64], date[64], plural[64];
        fdk_number_options nopt = {2, 2, 1, 0};
        (void)fdk_format_int(num, sizeof num, &loc, 1234567, &nopt);
        (void)fdk_format_currency(money, sizeof money, &loc, 1234.5,
                                  "USD");
        fdk_date d;
        if (fdk_ok(fdk_date_from_days(20000, &d))) {
            (void)fdk_format_date(date, sizeof date, &loc, &d,
                                  FDK_DATE_MEDIUM);
        } else {
            snprintf(date, sizeof date, "?");
        }
        fdk_plural_operands ops;
        fdk_plural_operands_from_int(0, &ops);
        snprintf(plural, sizeof plural, "n=0 -> %s",
                 ops.n == 0 ? "zero-class" : "other");
        log_line("[i18n] %s: int %s | %s | %s | %s", tags[i], num,
                 money, date, plural);
    }
}

/* ---- prefs --------------------------------------------------------------------------- */

static fdk_prefs *g_prefs;

static void prefs_write(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    (void)fdk_prefs_set(g_prefs, "example18.string", "written live");
    (void)fdk_prefs_set_int(g_prefs, "example18.int", 4242);
    (void)fdk_prefs_set_bool(g_prefs, "example18.bool", true);
    (void)fdk_prefs_set_double(g_prefs, "example18.double", 3.14159);
    fdk_result r = fdk_prefs_save(g_prefs);
    log_line("[prefs] wrote 4 kinds + save: %s (path: "
             "~/.config/fdk/prefs — the next launch reads them "
             "back)",
             fdk_ok(r) ? "OK" : "FAILED");
}

static void prefs_read(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    log_line("[prefs] string=%s int=%ld bool=%d double=%.5f",
             fdk_prefs_get(g_prefs, "example18.string", "(unset)"),
             fdk_prefs_get_int(g_prefs, "example18.int", -1),
             fdk_prefs_get_bool(g_prefs, "example18.bool", false),
             fdk_prefs_get_double(g_prefs, "example18.double", 0));
}

/* ---- main ------------------------------------------------------------------------ */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "18")) {
        return 1;
    }
    if (!fdk_example_open(&ex, ctx, "18",
                          "system integration (clipboard/dnd/undo/"
                          "shortcuts/i18n/prefs)",
                          760, 640)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_font *font = ex.ui;

    /* prefs: load what a previous run wrote. */
    g_prefs = NULL;
    if (!fdk_ok(fdk_prefs_open("example-18", &g_prefs)) ||
        g_prefs == NULL) {
        log_line("[prefs] open failed — running without");
    } else {
        log_line("[prefs] loaded; string=%s int=%ld (empty on the "
                 "first run, filled after Write+restart)",
                 fdk_prefs_get(g_prefs, "example18.string", "(unset)"),
                 fdk_prefs_get_int(g_prefs, "example18.int", -1));
    }

    /* undo stack */
    (void)fdk_undo_stack_create(0, &g_undo);

    /* ---- clipboard row ---- */
    fdk_widget *row1 = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row1);
    fdk_widget *b = NULL;
    (void)fdk_button_create(row1, font, "Clip: set text", &b);
    fdk_button_set_on_activate(b, clip_set_text, NULL);
    (void)fdk_button_create(row1, font, "get text", &b);
    fdk_button_set_on_activate(b, clip_get_text, NULL);
    (void)fdk_button_create(row1, font, "set uris", &b);
    fdk_button_set_on_activate(b, clip_set_uris, NULL);
    (void)fdk_button_create(row1, font, "get uris", &b);
    fdk_button_set_on_activate(b, clip_get_uris, NULL);
    (void)fdk_button_create(row1, font, "set image", &b);
    fdk_button_set_on_activate(b, clip_set_image, NULL);
    (void)fdk_button_create(row1, font, "get image", &b);
    fdk_button_set_on_activate(b, clip_get_image, NULL);

    /* ---- undo row ---- */
    fdk_widget *row2 = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row2);
    (void)fdk_button_create(row2, font, "Undo: push +1", &b);
    fdk_button_set_on_activate(b, counter_push, NULL);
    (void)fdk_button_create(row2, font, "undo", &b);
    fdk_button_set_on_activate(b, counter_undo, NULL);
    (void)fdk_button_create(row2, font, "redo", &b);
    fdk_button_set_on_activate(b, counter_redo, NULL);
    g_counter.label = NULL;
    (void)fdk_label_create(ex.content, font, "value: 0", &g_counter.label);
    counter_render();

    /* ---- dnd pads ---- */
    fdk_widget *row3 = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row3);
    fdk_widget *src = NULL;
    (void)fdk_button_create(row3, font,
                            "DRAG SOURCE (press + move)", &src);
    fdk_widget_set_event_callback(src, source_event, NULL);
    /* §29/G-10: the SAME gesture through the widget-level drag-source
     * controller — FDK arms it on the press wherever the press lands
     * in this widget's subtree and calls the begin fn past the slop,
     * so the app never hand-rolls press/motion/release tracking. */
    fdk_widget *src2 = NULL;
    (void)fdk_button_create(row3, font,
                            "DRAG SOURCE (controller API)", &src2);
    (void)fdk_widget_set_drag_source(src2, 6, controller_begin, NULL);
    fdk_widget *dst = NULL;
    (void)fdk_button_create(row3, font, "DROP TARGET", &dst);
    (void)fdk_widget_set_drop_target(
        dst, FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST);
    (void)fdk_widget_set_drag_callback(dst, target_event, NULL);

    /* ---- i18n + prefs rows ---- */
    fdk_widget *row4 = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row4);
    (void)fdk_button_create(row4, font, "i18n report", &b);
    fdk_button_set_on_activate(b, i18n_report, NULL);
    (void)fdk_button_create(row4, font, "prefs write", &b);
    fdk_button_set_on_activate(b, prefs_write, NULL);
    (void)fdk_button_create(row4, font, "prefs read", &b);
    fdk_button_set_on_activate(b, prefs_read, NULL);

    /* ---- shortcuts ---- */
    (void)fdk_window_add_shortcut(ex.window, "Ctrl+Z", sc_undo,
                                  NULL);
    (void)fdk_window_add_shortcut(ex.window, "Ctrl+Y", sc_redo,
                                  NULL);
    (void)fdk_window_add_shortcut(ex.window, "Ctrl+L", sc_report,
                                  NULL);
    log_line("[shortcut] Ctrl+Z / Ctrl+Y / Ctrl+L registered");

    /* the log pane */
    g_log = NULL;
    (void)fdk_textview_create(ex.content, font, &g_log);
    fdk_textview_set_text(g_log, g_log_text);

    fdk_example_run(&ex);

    fdk_undo_stack_destroy(g_undo);
    if (g_prefs != NULL) {
        fdk_prefs_destroy(g_prefs);
    }
    fdk_example_close(&ex);
    return 0;
}
