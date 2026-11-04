/* notes.h — fdk-notes: the app state. */
#ifndef FDK_NOTES_APP_H
#define FDK_NOTES_APP_H

#include "../common/app.h"
#include <stdbool.h>
#include <stddef.h>

#define NT_MAX_NOTEBOOKS 32
#define NT_MAX_NOTES 512
#define NT_TITLE_MAX 96
#define NT_BODY_MAX 8192

typedef struct nt_span {
    size_t start, end; /* byte range [start, end)                  */
    bool bold, italic;
} nt_span;

#define NT_MAX_SPANS 64

typedef struct nt_note {
    char title[NT_TITLE_MAX];
    char body[NT_BODY_MAX];
    size_t body_len;
    nt_span spans[NT_MAX_SPANS]; /* round 22: per-range rich text  */
    size_t span_count;
    long reminder_at;  /* unix seconds; 0 = none                   */
    bool reminder_fired;
    size_t notebook;  /* owner index                                */
} nt_note;

typedef struct nt_notebook {
    char name[64];
    size_t first_note; /* index into g_notes (contiguous run)       */
    size_t note_count;
} nt_notebook;

typedef struct nt_app {
    ta_app base;
    nt_notebook books[NT_MAX_NOTEBOOKS];
    size_t book_count;
    nt_note notes[NT_MAX_NOTES];
    size_t note_count;
    size_t current;      /* the selected note                       */
    /* the UI (ui.c owns) */
    fdk_widget *tree;
    fdk_widget *title_entry;
    fdk_widget *body_view;
    fdk_widget *focus_label;
    fdk_timer *reminder_timer;
    fdk_timer *focus_timer;
    long focus_ends_at;  /* unix seconds of the focus run's end     */
    bool focus_running;
    int focus_seconds_left;
    /* the narrator sink's echo (a11y narration is on) */
    char last_utterance[256];
} nt_app;

extern nt_app napp;

/* store.c */
bool nt_book_add(const char *name);
bool nt_note_add(size_t book, const char *title);
nt_note *nt_note_current(void);
void nt_save_all(void);
void nt_load_all(void);
nt_note *nt_note_at(size_t index);
void nt_focus_select(size_t index);
void nt_reminder_check(void);

/* main.c */
void nt_quit(void);

/* ui.c */
void nt_ui_build(void);
void nt_tree_refresh(void);
void nt_a11y_boot(void);
void nt_a11y_shutdown(void);
void nt_notify_boot(void);
void nt_notify_shutdown(void);
void nt_tray_boot(void);
void nt_tray_shutdown(void);
void nt_focus_toggle(void);
void nt_apply_attrs(void);
bool nt_format_selection(bool bold, bool italic);
void nt_focus_select(size_t index);
void nt_notify(const char *summary, const char *body);

#endif /* FDK_NOTES_APP_H */
