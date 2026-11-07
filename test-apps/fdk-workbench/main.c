/* main.c — fdk-workbench: the release kitchen-sink.
 *
 * The "any app without another app's APIs" stressor: ONE process on
 * the library ALONE, cross-wiring the subsystems the focused apps
 * use in isolation:
 *
 *   undo/redo      fdk_undo_stack behind every doc mutation, closures
 *                  re-driving the same UI code paths
 *   animation      fdk_widget_animate's BACK_OUT sweep on every undo
 *                  push — the edit you can SEE land
 *   dialogs        open / save / prompt, async, modal
 *   threads        a pthread scans the opened file; fdk_post hands
 *                  the stats to the UI thread (the one legal bridge)
 *   i18n           fdk_locale + fdk_format_* re-render the inspector
 *                  live on EN/DE switch
 *   prefs          the doc persists into the settings store and
 *                  returns next launch
 *   session        opened at boot; X11's NULL is the documented
 *                  degradation, printed as such
 *   DnD            sidebar rows drag into the Pinned list (TEXT)
 *   clipboard      system round trip of the selected row
 *   print          a REAL PDF written to /tmp via fdk_print_job
 *   a11y           accessible names on every interactive widget
 *
 * Exit receipt: undo counts, drops, posts, print pages, session
 * state — what the smoke run and the visual rig assert.
 */
#include "bench.h"

#include <fdk/fdk_a11y.h>
#include <fdk/fdk_animation.h>
#include <fdk/fdk_clipboard.h>
#include <fdk/fdk_dnd.h>
#include <fdk/fdk_dialog.h>
#include <fdk/fdk_i18n.h>
#include <fdk/fdk_print.h>
#include <fdk/fdk_session.h>
#include <fdk/fdk_thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bench B;
static fdk_timer *g_clock;

/* ---- the animated banner ------------------------------------------ */

static void banner_paint(fdk_widget *canvas, fdk_surface *s,
                         fdk_rect bounds, fdk_rect clip, void *user) {
    (void)canvas;
    (void)bounds;
    (void)clip;
    (void)user;
    /* the paint receives the WINDOW's surface: everything draws in
     * WINDOW coordinates, relative to the canvas's own `bounds` —
     * fdk_surface_get_info would hand back the window size and the
     * clip would eat every pixel of it (found visually, again) */
    B.banner_paints++;
    fdk_color bg = fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    fdk_color accent = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    fdk_color text = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    fdk_surface_fill_rect(s, bounds, bg);

    /* the accent sweep: where the last undo/redo landed */
    int x = bounds.x +
            (int)((double)(bounds.width - 8) * B.banner_x);
    fdk_rect bar;
    bar.x = x;
    bar.y = bounds.y + 4;
    bar.width = 8;
    bar.height = bounds.height - 8;
    fdk_surface_fill_rect(s, bar, accent);

    fdk_font *f = B.ta.font_ui;
    const char *title = "fdk-workbench — undo, dialogs, threads, "
                        "i18n, print, dnd, prefs, session";
    fdk_text_metrics m = {0, 0, 0};
    if (fdk_ok(fdk_font_measure_utf8(f, title, strlen(title), &m)) &&
        m.advance_width > 0) {
        fdk_surface_draw_utf8(
            s, f, title, strlen(title),
            bounds.x + (bounds.width - m.advance_width) / 2,
            bounds.y + bounds.height / 2 + 5, text);
    }
}

static void banner_tick(fdk_animation *anim, double e, void *user) {
    (void)anim;
    bench *b = user;
    b->banner_x = e;
    fdk_canvas_invalidate(b->banner);
}

void bench_banner_sweep(bench *b) {
    /* restart from 0: the visible "an edit happened" pulse */
    fdk_widget_animate(b->banner, 420, FDK_EASE_BACK_OUT, banner_tick,
                       NULL, b);
}

/* the keep-alive: canvases paint on damage, and a boot-time
 * invalidate can land before the first layout gives them a size —
 * the clock re-damages both so the views are never stale */
static void clock_tick(fdk_timer *timer, void *user) {
    (void)timer;
    bench *b = user;
    static int ticks = 0;
    ticks++;
    if (ticks <= 3) {
        fdk_log(FDK_LOG_INFO, "bench", "clock tick #%d", ticks);
    }
    fdk_canvas_invalidate(b->banner);
    fdk_canvas_invalidate(b->doc_canvas);
}

/* ---- the doc canvas (the table view) ---- */

static void doc_paint(fdk_widget *canvas, fdk_surface *s,
                      fdk_rect bounds, fdk_rect clip, void *user) {
    (void)canvas;
    (void)bounds;
    (void)clip;
    (void)user;
    fdk_color bg = fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    fdk_color text = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    fdk_color dim = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    dim.a *= 0.55f;
    fdk_color sel = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    fdk_surface_fill_rect(s, bounds, bg);

    const int line_h = 22;
    fdk_font *f = B.ta.font_ui;
    char hdr[96];
    /* the path may be long; the header clips it — deliberate */
    snprintf(hdr, sizeof hdr, "%.40s%s  (%d rows)",
             B.path[0] ? B.path : "(unsaved doc)",
             (int)strlen(B.path) > 40 ? "…" : "", B.row_count);
    fdk_surface_draw_utf8(s, f, hdr, strlen(hdr), bounds.x + 12,
                          bounds.y + 16, dim);

    int y = 34;
    for (int i = 0;
         i < B.row_count && y + line_h <= (int)bounds.height; i++) {
        int ry = bounds.y + y;
        if (i == B.selected) {
            fdk_rect hl;
            hl.x = bounds.x + 4;
            hl.y = ry - 3;
            hl.width = bounds.width - 8;
            hl.height = line_h;
            fdk_color hls = sel;
            hls.a *= 0.28f;
            fdk_surface_fill_rect(s, hl, hls);
        }
        char num[16];
        snprintf(num, sizeof num, "%3d", i);
        fdk_surface_draw_utf8(s, f, num, strlen(num), bounds.x + 12,
                              ry + 14, dim);
        fdk_surface_draw_utf8(s, f, B.rows[i].text,
                              strlen(B.rows[i].text), bounds.x + 54,
                              ry + 14, text);
        y += line_h;
    }
}

static bool doc_events(fdk_widget *widget, const fdk_widget_event *e,
                       void *user) {
    bench *b = user;
    if (e->type == FDK_WIDGET_POINTER_DOWN &&
        e->pointer.button == 1) {
        int idx = ((int)e->pointer.position.y - 34 + 19) / 22;
        if (idx >= 0 && idx < b->row_count && idx != b->selected) {
            b->selected = idx;
            fdk_list_select(b->side_list, (size_t)idx);
            fdk_widget_invalidate(b->doc_canvas);
        }
        return true;
    }
    (void)widget;
    return false;
}

/* ---- the worker thread -------------------------------------------- */

typedef struct scan_result {
    bench *b;
    long lines, words, bytes;
} scan_result;

static void scan_delivered(void *user) {
    scan_result *r = user;
    bench *b = r->b;
    char buf[160];
    snprintf(buf, sizeof buf, "%ld lines / %ld words", r->lines,
             r->words);
    fdk_label_set_text(b->stat_words, buf);
    snprintf(buf, sizeof buf, "%ld bytes", r->bytes);
    fdk_label_set_text(b->stat_bytes, buf);
    fdk_levelbar_set_value(b->level, (double)r->bytes / 1024.0);
    b->posts++;
    ta_status(&b->ta, "worker delivered: %ld lines, %ld bytes",
              r->lines, r->bytes);
    free(r);
}

static void *scan_thread(void *user) {
    char (*path)[512] = user;
    FILE *f = fopen(*path, "r");
    long lines = 0, words = 0, bytes = 0;
    if (f != NULL) {
        int c;
        bool in_word = false;
        while ((c = fgetc(f)) != EOF) {
            bytes++;
            if (c == '\n') {
                lines++;
            }
            if (c == ' ' || c == '\t' || c == '\n') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                words++;
            }
        }
        fclose(f);
    }
    scan_result *r = malloc(sizeof *r);
    if (r != NULL) {
        r->b = &B;
        r->lines = lines;
        r->words = words;
        r->bytes = bytes;
        /* THE one legal bridge: a worker thread hands the result to
         * the UI thread through fdk_post; the callback runs inside
         * the next pump */
        fdk_post(B.ta.ctx, scan_delivered, r);
    }
    free(path);
    return NULL;
}

static void bench_scan_start(const char *path) {
    char (*p)[512] = malloc(512);
    if (p == NULL) {
        return;
    }
    snprintf(*p, 512, "%s", path);
    if (B.worker_live) {
        pthread_join(B.worker, NULL);
    }
    B.worker_live = true;
    if (pthread_create(&B.worker, NULL, scan_thread, p) != 0) {
        free(p);
        B.worker_live = false;
    }
}

/* ---- dialogs ------------------------------------------------------- */

static void on_open_done(const fdk_file_dialog_result *result,
                         void *user) {
    bench *b = user;
    if (result->outcome == FDK_FILE_DIALOG_ACCEPTED &&
        result->count > 0) {
        bench_doc_load_from(b, result->paths[0]);
        bench_scan_start(result->paths[0]);
    }
    /* the result is toolkit-owned, read-only during the call — the
     * self-destroying dialog contract; nothing to free here */
}

static void on_save_done(const fdk_file_dialog_result *result,
                         void *user) {
    bench *b = user;
    if (result->outcome == FDK_FILE_DIALOG_ACCEPTED &&
        result->count > 0) {
        bench_doc_save_to(b, result->paths[0]);
        /* prefs remember it for the next launch */
        if (b->ta.prefs != NULL) {
            fdk_prefs_set(b->ta.prefs, "workbench.last_path",
                          result->paths[0]);
        }
    }
}

typedef struct prompt_ctx {
    bench *b;
    int index;
} prompt_ctx;

static void on_prompt(fdk_dialog_response response, const char *text,
                      void *user) {
    prompt_ctx *pc = user;
    if (response == FDK_DIALOG_OK && text != NULL) {
        bench_doc_replace(pc->b, pc->index, text, true);
    }
    free(pc);
}

static void edit_selected(bench *b) {
    if (b->selected < 0 || b->selected >= b->row_count) {
        return;
    }
    fdk_prompt_dialog_options po;
    memset(&po, 0, sizeof po);
    po.title = "Edit row";
    po.text = b->rows[b->selected].text;
    po.modal = true;
    po.parent = b->ta.window;
    prompt_ctx *pc = malloc(sizeof *pc);
    if (pc == NULL) {
        return;
    }
    pc->b = b;
    pc->index = b->selected;
    fdk_window *dlg = NULL;
    if (!fdk_ok(fdk_dialog_show_prompt(b->ta.ctx, &po, on_prompt, pc,
                                       &dlg))) {
        free(pc);
    }
}

/* ---- i18n ----------------------------------------------------------- */

static fdk_locale loc_current;
static bool loc_is_de = false;

static void bench_locale_refresh(bench *b) {
    char buf[160];
    if (fdk_ok(fdk_format_currency(buf, sizeof buf, &loc_current,
                                   1234.56, "EUR"))) {
        char line[224];
        snprintf(line, sizeof line, "currency: %s", buf);
        fdk_label_set_text(b->fmt_label, line);
    }
    char tag[32];
    fdk_locale_to_tag(&loc_current, tag, sizeof tag);
    ta_status(&b->ta, "locale: %s%s", tag,
              b->session == NULL
                  ? "  ·  session: n/a (the documented degradation)"
                  : "");
}

static void on_lang(fdk_widget *button, void *user) {
    (void)button;
    bench *b = user;
    loc_is_de = !loc_is_de;
    fdk_locale_parse(loc_is_de ? "de-DE" : "en-US", &loc_current);
    bench_locale_refresh(b);
}

/* ---- clipboard / print --------------------------------------------- */

static void on_copy(fdk_widget *button, void *user) {
    (void)button;
    bench *b = user;
    if (b->selected >= 0 && b->selected < b->row_count) {
        fdk_clipboard_set_text(b->ta.ctx, b->rows[b->selected].text);
        ta_status(&b->ta, "copied row %d to the clipboard",
                  b->selected);
    }
}

static void on_paste(fdk_widget *button, void *user) {
    (void)button;
    bench *b = user;
    char *t = fdk_clipboard_get_text(b->ta.ctx);
    if (t != NULL) {
        bench_doc_add(b, t, true);
        free(t);
    }
}

static void on_print(fdk_widget *button, void *user) {
    (void)button;
    bench *b = user;
    fdk_page_setup setup;
    fdk_page_setup_default(&setup);
    fdk_print_job *job = fdk_print_job_create(&setup);
    if (job == NULL) {
        ta_status(&b->ta, "print: job create failed");
        return;
    }
    fdk_surface *s = fdk_print_job_surface(job);
    fdk_color black = {0.05f, 0.05f, 0.05f, 1.0f};
    fdk_color gray = {0.4f, 0.4f, 0.4f, 1.0f};
    if (fdk_ok(fdk_print_job_begin_page(job))) {
        char hdr[128];
        snprintf(hdr, sizeof hdr, "fdk-workbench — %d rows",
                 b->row_count);
        fdk_surface_draw_utf8(s, b->ta.font_ui, hdr, strlen(hdr), 40,
                              60, black);
        int y = 96;
        for (int i = 0; i < b->row_count && y < 700; i++) {
            char line[BENCH_TEXT_CAP + 16];
            snprintf(line, sizeof line, "%d. %s", i, b->rows[i].text);
            fdk_surface_draw_utf8(s, b->ta.font_ui, line,
                                  strlen(line), 40, y, gray);
            y += 18;
        }
    }
    fdk_result r =
        fdk_print_job_finish_to_file(job, "/tmp/fdk-workbench.pdf");
    size_t pages = fdk_print_job_page_count(job);
    fdk_print_job_destroy(job);
    b->prints++;
    if (fdk_ok(r)) {
        ta_status(&b->ta,
                  "printed %zu page(s) -> /tmp/fdk-workbench.pdf",
                  pages);
    } else {
        ta_status(&b->ta, "print write failed");
    }
}

/* ---- dnd: sidebar rows -> Pinned ------------------------------------ */

static void pinned_drag(fdk_widget *widget, fdk_event_type type,
                        const fdk_drag_event *event, void *user) {
    bench *b = user;
    (void)widget;
    if (type == FDK_EVENT_DRAG_DROP && event->text != NULL) {
        char shown[BENCH_TEXT_CAP + 16];
        snprintf(shown, sizeof shown, "pin: %s", event->text);
        fdk_list_append(b->pinned_list, shown, NULL);
        b->drops++;
    }
}

/* ---- the entry + keys ------------------------------------------------ */

static void add_clicked(fdk_widget *button, void *user) {
    (void)button;
    bench *b = user;
    const char *t = fdk_entry_get_text(b->entry);
    if (t != NULL && t[0] != '\0') {
        bench_doc_add(b, t, true);
        fdk_entry_set_text(b->entry, "");
    }
}

static void on_edit(fdk_widget *button, void *user) {
    (void)button;
    edit_selected(user);
}

static void bench_key(bench *b, const fdk_key_event *k) {
    bool ctrl = (k->modifiers & FDK_MOD_CTRL) != 0;
    if (!ctrl) {
        return;
    }
    switch (k->codepoint) {
    case 'z': bench_undo_apply(b); break;
    case 'y': bench_undo_redo(b); break;
    case 'o': {
        fdk_file_dialog_options fo;
        memset(&fo, 0, sizeof fo);
        fo.title = "Open document";
        fo.kind = FDK_FILE_DIALOG_OPEN_FILE;
        fo.modal = true;
        fo.parent = b->ta.window;
        fdk_window *dlg = NULL;
        fdk_dialog_open_file(b->ta.ctx, &fo, on_open_done, b, &dlg);
        break;
    }
    case 's': {
        fdk_file_dialog_options fo;
        memset(&fo, 0, sizeof fo);
        fo.title = "Save document";
        fo.kind = FDK_FILE_DIALOG_SAVE_FILE;
        fo.modal = true;
        fo.parent = b->ta.window;
        fdk_window *dlg = NULL;
        fdk_dialog_save_file(b->ta.ctx, &fo, on_save_done, b, &dlg);
        break;
    }
    case 'p': on_print(NULL, b); break;
    default: break;
    }
}

static void bench_window_event(fdk_window *w, const fdk_event_data *e,
                               void *user) {
    bench *b = user;
    (void)w;
    if (e->type == FDK_EVENT_KEY_DOWN) {
        bench_key(b, &e->key);
    }
}

/* ---- boot ------------------------------------------------------------ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&B, 0, sizeof B);

    if (!ta_init(&B.ta, "workbench")) {
        return 1;
    }
    if (!ta_open(&B.ta, "fdk-workbench", 1000, 640)) {
        return 1;
    }

    /* the session: NULL on X11 is the documented degradation */
    B.session = fdk_session_open(B.ta.ctx, FDK_SESSION_REASON_LAUNCH,
                                 NULL, NULL, NULL);

    fdk_undo_stack_create(0, &B.undo);
    bench_doc_init(&B);
    fdk_locale_parse("en-US", &loc_current);

    /* ---- the layout: sidebar | center | inspector ---- */
    fdk_widget *cols = NULL;
    fdk_box_create(B.ta.content, FDK_HORIZONTAL, &cols);
    fdk_box_set_spacing(cols, 8);
    fdk_widget_set_expand(cols, true, true);

    /* sidebar */
    fdk_widget *side = NULL;
    fdk_box_create(cols, FDK_VERTICAL, &side);
    fdk_box_set_spacing(side, 6);
    fdk_list_create(side, B.ta.font_ui, &B.side_list);
    fdk_widget_set_expand(B.side_list, true, true);
    fdk_widget_set_accessible_name(B.side_list, "document rows");
    fdk_widget *srow = NULL;
    fdk_box_create(side, FDK_VERTICAL, &srow);
    fdk_widget *bcopy = NULL, *bpaste = NULL, *bedit = NULL;
    fdk_button_create(srow, B.ta.font_ui, "Copy", &bcopy);
    fdk_button_set_on_activate(bcopy, on_copy, &B);
    fdk_button_create(srow, B.ta.font_ui, "Paste", &bpaste);
    fdk_button_set_on_activate(bpaste, on_paste, &B);
    fdk_button_create(srow, B.ta.font_ui, "Edit…", &bedit);
    fdk_button_set_on_activate(bedit, on_edit, &B);

    /* center: banner + doc canvas + quick-add */
    fdk_widget *center = NULL;
    fdk_box_create(cols, FDK_VERTICAL, &center);
    fdk_box_set_spacing(center, 6);
    fdk_widget_set_expand(center, true, true);
    fdk_canvas_create(center, banner_paint, NULL, &B.banner);
    fdk_widget_set_natural_size(B.banner, 200, 44);
    fdk_widget_set_expand(B.banner, true, false);
    fdk_widget_set_accessible_name(B.banner, "activity banner");
    fdk_widget *crow = NULL;
    fdk_box_create(center, FDK_HORIZONTAL, &crow);
    fdk_entry_create(crow, B.ta.font_ui, "", &B.entry);
    fdk_widget_set_expand(B.entry, true, false);
    fdk_widget_set_accessible_name(B.entry, "quick add");
    fdk_widget *badd = NULL;
    fdk_button_create(crow, B.ta.font_ui, "Add", &badd);
    fdk_button_set_on_activate(badd, add_clicked, &B);

    fdk_canvas_create(center, doc_paint, NULL, &B.doc_canvas);
    fdk_widget_set_expand(B.doc_canvas, true, true);
    fdk_widget_set_accessible_name(B.doc_canvas, "document view");
    fdk_widget_set_event_callback(B.doc_canvas, doc_events, &B);

    /* inspector */
    fdk_widget *insp = NULL;
    fdk_box_create(cols, FDK_VERTICAL, &insp);
    fdk_box_set_spacing(insp, 6);
    fdk_label_create(insp, B.ta.font_ui, "rows: 0", &B.stat_rows);
    fdk_label_create(insp, B.ta.font_ui, "words: —", &B.stat_words);
    fdk_label_create(insp, B.ta.font_ui, "bytes: —", &B.stat_bytes);
    fdk_levelbar_create(insp, 0.0, 64.0, &B.level);
    fdk_label_create(insp, B.ta.font_ui, "currency: —", &B.fmt_label);
    fdk_widget *lang = NULL;
    fdk_button_create(insp, B.ta.font_ui, "en / de", &lang);
    fdk_button_set_on_activate(lang, on_lang, &B);
    fdk_widget *bprint = NULL;
    fdk_button_create(insp, B.ta.font_ui, "Print to /tmp", &bprint);
    fdk_button_set_on_activate(bprint, on_print, &B);
    fdk_label_create(insp, B.ta.font_ui, "Pinned (drop rows here):",
                     NULL);
    fdk_list_create(insp, B.ta.font_ui, &B.pinned_list);
    fdk_widget_set_expand(B.pinned_list, false, true);
    fdk_widget_set_accessible_name(B.pinned_list, "pinned rows");
    fdk_window_set_drop_formats(B.ta.window, FDK_DRAG_FORMAT_TEXT);
    fdk_widget_set_drop_target(B.pinned_list, FDK_DRAG_FORMAT_TEXT);
    fdk_widget_set_drag_callback(B.pinned_list, pinned_drag, &B);

    /* keys ride the window callback (Ctrl+Z/Y/O/S/P) */
    fdk_window_set_event_callback(B.ta.window, bench_window_event,
                                  &B);

    /* the widget event callback edit: the Edit button needs the
     * prototype above; the canvas already carries doc_events */

    /* seed + restore + refresh */
    bench_doc_refresh_views(&B);
    if (B.ta.prefs != NULL) {
        char last[512];
        if (ta_pref_str(&B.ta, "workbench.last_path", "", last,
                        sizeof last) && last[0] != '\0') {
            bench_doc_load_from(&B, last);
            bench_scan_start(last);
        }
    }
    bench_locale_refresh(&B);
    bench_banner_sweep(&B);
    g_clock = fdk_timer_add(B.ta.ctx, 300, true, clock_tick, &B);

    ta_run(&B.ta);
    fdk_timer_remove(g_clock); /* quiet teardown */

    /* ---- the receipt ---- */
    if (B.worker_live) {
        pthread_join(B.worker, NULL);
    }
    if (B.ta.prefs != NULL) {
        /* persist the doc: rows joined by newline */
        static char blob[BENCH_MAX_ROWS * BENCH_TEXT_CAP / 4];
        size_t off = 0;
        blob[0] = '\0';
        for (int i = 0; i < B.row_count && off < sizeof blob - 160;
             i++) {
            int n = snprintf(blob + off, sizeof blob - off, "%s%s",
                             i ? "\n" : "", B.rows[i].text);
            if (n < 0) {
                break;
            }
            off += (size_t)n;
        }
        fdk_prefs_set(B.ta.prefs, "workbench.doc", blob);
    }
    printf("workbench receipt: undos=%d redos=%d drops=%d posts=%d "
           "prints=%d rows=%d session=%s banner_paints=%ld\n",
           B.undos_applied, B.redos_applied, B.drops, B.posts,
           B.prints, B.row_count,
           B.session == NULL ? "n/a" : fdk_session_id(B.session),
           B.banner_paints);
    fdk_undo_stack_destroy(B.undo);
    if (B.session != NULL) {
        fdk_session_destroy(B.session);
    }
    ta_close(&B.ta);
    printf("WORKBENCH PASS\n");
    return 0;
}
