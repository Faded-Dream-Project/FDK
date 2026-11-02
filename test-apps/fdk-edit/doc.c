/* doc.c — the documents: open/new/save/close, the tab lifecycle,
 * the modified indicator, the gutter + status wiring. */
#include "edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ed_app eapp;

ed_doc *ed_doc_current(void) {
    if (eapp.current >= eapp.doc_count) {
        return NULL;
    }
    return &eapp.docs[eapp.current];
}

static const char *base_name(const char *path) {
    const char *b = strrchr(path, '/');
    return (b != NULL) ? b + 1 : path;
}

void ed_doc_retitle(ed_doc *d) {
    if (d == NULL) {
        return;
    }
    char label[160];
    if (d->path[0] == '\0') {
        snprintf(label, sizeof label, "%s%s",
                 d->modified ? "● " : "", ta_("Untitled"));
    } else {
        snprintf(label, sizeof label, "%s%s",
                 d->modified ? "● " : "", base_name(d->path));
    }
    ta_copy(d->display, sizeof d->display, label);
    /* find the doc's page index (pages map 1:1 to docs in order) */
    for (size_t i = 0; i < eapp.doc_count; i++) {
        if (&eapp.docs[i] == d && eapp.notebook != NULL) {
            (void)fdk_notebook_set_page_label(eapp.notebook, i,
                                              d->display);
        }
    }
}

void ed_doc_mark_modified(ed_doc *d) {
    if (d == NULL || d->modified) {
        return;
    }
    d->modified = true;
    ed_doc_retitle(d);
    ed_ui_status();
}

/* the gutter paint: line numbers right-aligned in the band */
static void ed_gutter_paint(fdk_widget *tv, fdk_surface *s,
                            fdk_rect band, size_t first_line,
                            size_t line_count, void *user) {
    (void)tv;
    (void)user;
    fdk_font *font = eapp.base.font_mono;
    if (font == NULL) {
        return;
    }
    fdk_color col = fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED);
    /* the band's y0 corresponds to first_line's line-box top; the
     * pad offset matches the text's line math (pad + i*line_h) —
     * derive line height from the band's height share            */
    fdk_i32 line_h = (line_count > 0) ? band.height / (fdk_i32)line_count
                                      : 16;
    for (size_t i = 0; i < line_count; i++) {
        /* the PARAGRAPH number: visual lines of one paragraph share
         * its number (the editor's rule). paragraph_count-of-offset
         * is O(n) per line; the gutter is per-frame — precompute is
         * overkill for PASS A: the visible window is small. */
        char num[16];
        snprintf(num, sizeof num, "%zu", first_line + i + 1);
        fdk_text_metrics m;
        if (!fdk_ok(fdk_font_measure_utf8(font, num, strlen(num),
                                          &m))) {
            continue;
        }
        fdk_i32 x = band.x + band.width - m.advance_width - 4;
        fdk_i32 baseline = band.y + (fdk_i32)i * line_h +
                           (line_h * 3 / 4);
        (void)fdk_surface_draw_utf8(s, font, num, strlen(num), x,
                                    baseline, col);
    }
}

/* one shared changed-callback: marks modified + re-highlights */
static void on_doc_changed(fdk_widget *view, void *user) {
    ed_doc *d = user;
    (void)view;
    /* the textview's buffer is the SOURCE of truth now: resync */
    const char *t = fdk_textview_get_text(d->view);
    if (t != d->text) {
        free(d->text);
        d->text = strdup(t != NULL ? t : "");
        d->text_len = strlen(d->text);
    }
    ed_highlight(d);
    ed_doc_mark_modified(d);
}

static void ed_doc_create(const char *path) {
    if (eapp.doc_count >=
        sizeof eapp.docs / sizeof *eapp.docs) {
        ta_status(&eapp.base, "%s", ta_("Too many tabs"));
        return;
    }
    ed_doc *d = &eapp.docs[eapp.doc_count];
    memset(d, 0, sizeof *d);
    ta_copy(d->path, sizeof d->path, path != NULL ? path : "");
    d->lang = ed_lang_of_path(d->path);
    ta_copy(d->encoding, sizeof d->encoding, "UTF-8");
    d->crlf = false;
    d->bom = false;
    d->modified = false;

    if (path != NULL && path[0] != '\0') {
        char *text = NULL;
        size_t text_len = 0;
        char enc[16] = "UTF-8";
        bool crlf = false, bom = false;
        if (!ed_read_file(path, &text, &text_len, enc, sizeof enc,
                          &crlf, &bom)) {
            ta_status(&eapp.base, ta_("Cannot open %s"), path);
            return;
        }
        d->text = text;
        d->text_len = text_len;
        ta_copy(d->encoding, sizeof d->encoding, enc);
        d->crlf = crlf;
        d->bom = bom;
    } else {
        d->text = strdup("");
        d->text_len = 0;
    }

    /* the page: a plain box parent for the textview (the notebook
     * adopts it) */
    (void)fdk_widget_create(eapp.notebook, NULL,
                            (fdk_rect){0, 0, 100, 100}, &d->page);
    (void)fdk_textview_create(d->page, eapp.base.font_mono, &d->view);
    fdk_widget_set_bounds(d->view, (fdk_rect){0, 0, 100, 100});
    fdk_textview_set_text(d->view, d->text);
    fdk_textview_set_wrap(
        d->view, eapp.wrap ? FDK_TEXTVIEW_WRAP_WORD
                           : FDK_TEXTVIEW_WRAP_NONE);
    if (eapp.show_gutter) {
        (void)fdk_textview_set_gutter(d->view, 48, ed_gutter_paint,
                                      NULL);
    }
    fdk_textview_set_on_changed(d->view, on_doc_changed, d);
    (void)fdk_notebook_append_page(eapp.notebook, d->page, "");
    ed_highlight(d);
    ed_doc_retitle(d);

    eapp.current = eapp.doc_count;
    eapp.doc_count++;
    (void)fdk_notebook_set_current_page(eapp.notebook,
                                        eapp.current);
    if (path != NULL && path[0] != '\0') {
        ed_recent_add(path);
    }
    ed_ui_status();
}

void ed_doc_open(const char *path) {
    ed_doc_create(path);
}

void ed_doc_new(void) {
    ed_doc_create(NULL);
}

/* the write-back, shared by save and save-as-after-pick */
static bool doc_write(ed_doc *d, const char *path) {
    /* resync the buffer (unsaved edits live in the textview) */
    const char *t = fdk_textview_get_text(d->view);
    if (t != NULL && t != d->text) {
        free(d->text);
        d->text = strdup(t);
        d->text_len = strlen(d->text);
    }
    if (!ed_write_file(path, d->text, d->text_len, d->crlf,
                       d->bom)) {
        ta_status(&eapp.base, ta_("Cannot save %s"), path);
        return false;
    }
    ta_copy(d->path, sizeof d->path, path);
    d->lang = ed_lang_of_path(path);
    d->modified = false;
    ed_recent_add(path);
    ed_doc_retitle(d);
    ed_highlight(d);
    ed_ui_status();
    ta_status(&eapp.base, ta_("Saved %s"), path);
    return true;
}

/* the save-as pick's callback (async: the dialog runs in the pump) */
static void on_save_as_picked(const fdk_file_dialog_result *r,
                              void *user) {
    ed_doc *d = user;
    if (r == NULL || r->outcome != FDK_FILE_DIALOG_ACCEPTED ||
        r->count == 0 || r->paths == NULL) {
        return; /* cancelled: nothing to do */
    }
    (void)doc_write(d, r->paths[0]);
}

bool ed_doc_save(ed_doc *d, bool save_as) {
    if (d == NULL) {
        return false;
    }
    if (!save_as && d->path[0] != '\0') {
        return doc_write(d, d->path);
    }
    fdk_file_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("Save As");
    o.kind = FDK_FILE_DIALOG_SAVE_FILE;
    o.parent = eapp.base.window;
    o.start_name = (d->path[0] != '\0') ? base_name(d->path)
                                         : "untitled.txt";
    fdk_result rr = fdk_dialog_save_file(
        eapp.base.ctx, &o, on_save_as_picked, d, NULL);
    if (!fdk_ok(rr)) {
        ta_status(&eapp.base, "%s", ta_("Cannot open the save dialog"));
    }
    /* async: the write happens in the callback; report the dialog
     * was launched (the caller's status line says the rest) */
    return fdk_ok(rr);
}

void ed_doc_close(size_t index) {
    if (index >= eapp.doc_count) {
        return;
    }
    ed_doc *d = &eapp.docs[index];
    /* an unsaved doc: ask. The dialog is async (toolkit-owned
     * window, the response callback closes the tab); YES = save &
     * close, NO = close, CANCEL = keep. The close itself therefore
     * happens from the callback (ed_doc_close_deferred). */
    if (d->modified) {
        extern void ed_ask_unsaved(size_t index);
        ed_ask_unsaved(index);
        return; /* the callback finishes the close */
    }
    free(d->text);
    /* the notebook owns the page: removing it destroys the view */
    (void)fdk_notebook_remove_page(eapp.notebook, index);
    /* compact the array */
    for (size_t i = index; i + 1 < eapp.doc_count; i++) {
        eapp.docs[i] = eapp.docs[i + 1];
    }
    eapp.doc_count--;
    if (eapp.current >= eapp.doc_count && eapp.doc_count > 0) {
        eapp.current = eapp.doc_count - 1;
    }
    /* re-point the changed callbacks' user pointers (the array
     * moved) */
    for (size_t i = 0; i < eapp.doc_count; i++) {
        fdk_textview_set_on_changed(eapp.docs[i].view,
                                    on_doc_changed, &eapp.docs[i]);
    }
    ed_ui_tab_update();
    ed_ui_status();
}

/* re-arm the gutter on a doc (the prefs/menu toggles) */
void ed_arm_gutter(ed_doc *d) {
    if (d != NULL && d->view != NULL) {
        (void)fdk_textview_set_gutter(d->view, 48, ed_gutter_paint,
                                      NULL);
    }
}

void ed_docs_shutdown(void) {
    for (size_t i = 0; i < eapp.doc_count; i++) {
        free(eapp.docs[i].text);
        eapp.docs[i].text = NULL;
    }
    eapp.doc_count = 0;
}
