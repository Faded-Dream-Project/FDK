/* edit.h — fdk-edit: the text editor test-app (§24 spec, §30 PASS A).
 *
 * A real editor over the PUBLIC API only: tabs with modified
 * indicators, open/save/save-as, undo/redo, find/replace (plain +
 * regex), a line-number gutter, syntax highlighting (C, shell,
 * Markdown, JSON, Makefile) through the range-attribute engine,
 * icon toolbar, status bar (line:col, encoding, line endings,
 * language), word wrap, recent files, session restore,
 * drag-file-to-open, UTF-8/BOM/UTF-16 reading and CRLF/LF.
 */
#ifndef FDK_EDIT_APP_H
#define FDK_EDIT_APP_H

#include "../common/app.h"
#include "../common/ta_buf.h"

#include "fdk/fdk.h"
#include "fdk/fdk_attr.h"
#include "fdk/fdk_clipboard.h"
#include "fdk/fdk_dialog.h"
#include "fdk/fdk_dnd.h"
#include "fdk/fdk_theme.h"
#include "fdk/fdk_widgets.h"
#include "fdk/fdk_window.h"

#include <stdbool.h>
#include <stddef.h>

/* ---- the language table ------------------------------------------
 * One entry per highlighted language; the highlighter fills an
 * attr list with (byte range, color) pairs. */
typedef enum ed_lang {
    ED_LANG_NONE = 0,
    ED_LANG_C,
    ED_LANG_SHELL,
    ED_LANG_MARKDOWN,
    ED_LANG_JSON,
    ED_LANG_MAKEFILE,
    ED_LANG_COUNT,
} ed_lang;

const char *ed_lang_name(ed_lang l);
ed_lang ed_lang_of_path(const char *path);

/* ---- a document --------------------------------------------------
 * One file: the bytes (decoded to UTF-8), the metadata, the tab. */
typedef struct ed_doc {
    char path[512];      /* "" = never saved                       */
    char display[128];   /* the tab label                          */
    ed_lang lang;
    char encoding[16];   /* "UTF-8" / "UTF-8 BOM" / "UTF-16LE"...  */
    bool crlf;           /* line endings on load/save              */
    bool modified;
    bool bom;
    char *text;          /* the CURRENT document text (UTF-8)      */
    size_t text_len;
    fdk_widget *page;    /* the notebook page (owns the textview)  */
    fdk_widget *view;    /* the TextView                            */
} ed_doc;

/* ---- the app ----------------------------------------------------- */
typedef struct ed_app {
    ta_app base;         /* ctx/window/menubar/content/statusbar    */
    fdk_widget *toolbar;
    fdk_widget *notebook;
    ed_doc docs[16];
    size_t doc_count;
    size_t current;      /* index into docs                         */
    fdk_attr_list attrs; /* reused scratch for highlighting         */

    /* the find bar */
    fdk_widget *findbar;
    fdk_widget *find_entry;
    fdk_widget *replace_entry;
    bool replace_visible;
    bool regex_mode;

    /* recent files (paths, most recent first) */
    char recent[8][512];
    size_t recent_count;

    /* prefs snapshot */
    bool wrap;
    bool highlight;
    bool show_gutter;
} ed_app;

extern ed_app eapp;

/* doc.c */
void ed_doc_open(const char *path);      /* NULL = new empty tab   */
void ed_doc_new(void);
bool ed_doc_save(ed_doc *d, bool save_as);
void ed_doc_close(size_t index);
void ed_doc_mark_modified(ed_doc *d);
void ed_doc_retitle(ed_doc *d);
ed_doc *ed_doc_current(void);
void ed_docs_shutdown(void);

/* files.c — decode/encode + session */
bool ed_read_file(const char *path, char **out, size_t *out_len,
                  char *encoding, size_t enc_cap, bool *crlf,
                  bool *bom);
bool ed_write_file(const char *path, const char *text, size_t len,
                   bool crlf, bool bom);
void ed_recent_add(const char *path);
void ed_session_save(void);
void ed_session_load(void);

/* highlight.c */
void ed_highlight(ed_doc *d);            /* rebuild d's attr list  */

/* find.c — find/replace; returns the match count */
size_t ed_find_first(const char *haystack, const char *needle,
                     bool regex, bool case_sensitive, size_t *out_len);
const char *ed_find_next(const char *haystack, const char *needle,
                         bool regex, bool case_sensitive,
                         size_t from, size_t *out_len);
bool ed_replace_all(ed_doc *d, const char *find, const char *repl,
                    bool regex, bool case_sensitive, size_t *count);

/* doc.c/ui.c extras */
void ed_arm_gutter(ed_doc *d);
void ed_open_dialog(void);
void ed_prefs_dialog(void);
void ed_ask_unsaved(size_t index);
void on_doc_changed_external(ed_doc *d);

/* ui.c — the Edit-menu twins (round 21's menu wiring) */
void ed_menu_undo(fdk_widget *mi, void *u);
void ed_menu_redo(fdk_widget *mi, void *u);
void ed_menu_find(fdk_widget *mi, void *u);
void ed_menu_replace(fdk_widget *mi, void *u);

/* ui.c — toolbar/menus/statusbar/findbar */
void ed_ui_build(void);
void ed_ui_status(void);
void ed_ui_findbar_toggle(bool with_replace);
void ed_ui_tab_update(void);

#endif /* FDK_EDIT_APP_H */
