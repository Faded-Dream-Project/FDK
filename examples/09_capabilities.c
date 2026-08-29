/* 09_capabilities.c — the real-world capability showcase (1.2.1).
 *
 * One application that answers "what can FDK do with the desktop?"
 * in five sections, because that is the honest way to validate the
 * platform seams:
 *
 *   CLIPBOARD    an Entry wired to Copy / Cut / Paste / Clear (the
 *                Entry's stock Ctrl+X/C/V work too), a "Read
 *                clipboard" button that pulls whatever ANOTHER app
 *                left there into a preview line (length + first
 *                bytes, escaped), and "Set greeting" which plants a
 *                timestamped string for other apps to paste — the
 *                interop story in both directions.
 *
 *   TEXT INPUT   a big box you simply TYPE IN (the live typing
 *                playground): every keystroke updates a status line
 *                with bytes / caret / selection, read straight from
 *                the public Entry APIs; Password and Read-only
 *                toggles flip the box's modes live; Enter reports
 *                the commit.
 *
 *   DROP TARGET  a panel that highlights while a drag hovers it and
 *                reports the last drop: type (text / N files /
 *                folder), names and paths, or the text contents.
 *                Drop things from your file manager or editor.
 *
 *   DIALOGS      [Select File] [Select Files] [Select Folder]
 *                [Select Folders] open the FDK file dialog; the
 *                outcome line distinguishes accepted (every path
 *                listed — multi-selection is never silently
 *                discarded), cancelled, and error. [Ask Yes/No]
 *                shows a message dialog and reports the answer.
 *
 *   DRAG SOURCE  a "drag me" panel: press and drag from it into any
 *                other application — text into an editor, or the two
 *                files it advertises into a file manager. The status
 *                line reports how the drag ended.
 *
 * The window repaints when its tree has damage (the 1.2.0 build
 * forgot, which read as "dead between interactions"), and its root
 * background is the toolkit default (1.2.1) — status lines now
 * CLEAR their old text before drawing the new.
 *
 * For the GUI test rigs the demo prints:
 *   RIG: drop-panel|drag-panel|btn-copy|btn-paste|btn-clipread|
 *        btn-clipset|btn-file|btn-files|btn-folder|btn-folders|
 *        btn-ask|entry-typing <x> <y> <w> <h>
 *   PHASE: clip <op> <n>                 — copy/cut/paste/read/set
 *   PHASE: type <bytes> <caret>          — every typing-box change
 *   PHASE: drop <text|files|folder> <n>  — decoded drops
 *   PHASE: dialog <accepted|cancelled|error|yes|no> <count>
 *   PHASE: drag <succeeded|cancelled|failed>
 *
 * Close the window or press ESC to exit; FDK_DEMO_FRAMES=N exits
 * after N pump iterations (the automation knob every rig uses).
 * Fonts come from fdk_font_load_system_default().
 */

#include "fdk/fdk.h"
#include "fdk/fdk_dialog.h"
#include "fdk/fdk_dnd.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- app state ---- */

static struct {
    fdk_context *ctx;
    fdk_window *window;
    fdk_font *font;
    fdk_widget *root;
    bool quit;
    int frames_left;
    /* clipboard section */
    fdk_widget *entry;
    fdk_widget *clip_btns[6]; /* Copy Cut Paste Clear | Read Set */
    fdk_widget *clip_status;
    fdk_widget *clip_preview;
    /* typing section */
    fdk_widget *typing;
    fdk_widget *toggle_password, *toggle_readonly;
    fdk_widget *typing_status;
    fdk_widget *typing_commit;
    /* drop section */
    fdk_widget *drop_panel;
    fdk_rect drop_rect;
    fdk_widget *drop_status;
    /* dialog section */
    fdk_widget *dlg_btns[5]; /* File Files Folder Folders | Ask */
    fdk_widget *dialog_status;
    /* drag section */
    fdk_widget *drag_panel;
    fdk_rect drag_rect;
    fdk_widget *drag_status;
    /* headings */
    fdk_widget *h_clip, *h_type, *h_drop, *h_dlg, *h_drag;
} app;

/* ------------------------------------------------------------------ */
/* status helpers                                                      */
/* ------------------------------------------------------------------ */

static void set_status(fdk_widget *label, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)fdk_label_set_text(label, buf);
}

static void rig_announce(const char *name, fdk_widget *w) {
    if (w == NULL) {
        return;
    }
    fdk_rect r = fdk_widget_get_bounds(w);
    printf("RIG: %s %d %d %d %d\n", name, r.x, r.y, r.width, r.height);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* clipboard section                                                   */
/* ------------------------------------------------------------------ */

static void entry_selection(size_t *lo, size_t *hi) {
    *lo = *hi = 0;
    size_t anchor = 0, caret = 0;
    if (fdk_ok(fdk_entry_get_selection(app.entry, &anchor, &caret))) {
        if (anchor <= caret) {
            *lo = anchor;
            *hi = caret;
        } else {
            *lo = caret;
            *hi = anchor;
        }
    }
}

static void clip_copy_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    size_t lo = 0, hi = 0;
    entry_selection(&lo, &hi);
    const char *text = fdk_entry_get_text(app.entry);
    if (text != NULL && lo < hi) {
        fdk_result r = fdk_clipboard_set_text(app.ctx, text + lo);
        size_t n = hi - lo;
        if (fdk_ok(r)) {
            set_status(app.clip_status, "Copied %zu characters", n);
            printf("PHASE: clip copy %zu\n", n);
        } else {
            set_status(app.clip_status, "Copy failed (%s)",
                       fdk_result_to_string(r));
            printf("PHASE: clip copy-failed\n");
        }
    } else {
        set_status(app.clip_status, "Nothing selected to copy");
        printf("PHASE: clip copy-empty\n");
    }
    fflush(stdout);
}

static void clip_cut_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    size_t lo = 0, hi = 0;
    entry_selection(&lo, &hi);
    const char *text = fdk_entry_get_text(app.entry);
    if (text != NULL && lo < hi) {
        fdk_result r = fdk_clipboard_set_text(app.ctx, text + lo);
        if (fdk_ok(r)) {
            fdk_entry_select_range(app.entry, lo, lo);
            fdk_entry_set_cursor(app.entry, lo);
            char *kept = malloc(strlen(text) + 1);
            if (kept != NULL) {
                memcpy(kept, text, lo);
                strcpy(kept + lo, text + hi);
                (void)fdk_entry_set_text(app.entry, kept);
                free(kept);
            }
            set_status(app.clip_status, "Cut %zu characters", hi - lo);
            printf("PHASE: clip cut %zu\n", hi - lo);
        } else {
            set_status(app.clip_status, "Cut failed (%s)",
                       fdk_result_to_string(r));
            printf("PHASE: clip cut-failed\n");
        }
    } else {
        set_status(app.clip_status, "Nothing selected to cut");
        printf("PHASE: clip cut-empty\n");
    }
    fflush(stdout);
}

static void clip_paste_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    char *text = fdk_clipboard_get_text(app.ctx);
    if (text == NULL) {
        set_status(app.clip_status, "Clipboard is empty or unreadable");
        printf("PHASE: clip paste-empty\n");
        fflush(stdout);
        return;
    }
    size_t lo = 0, hi = 0;
    entry_selection(&lo, &hi);
    const char *cur = fdk_entry_get_text(app.entry);
    if (cur == NULL) {
        cur = "";
    }
    size_t cur_len = strlen(cur), p_len = strlen(text);
    char *merged = malloc(cur_len + p_len + 1);
    if (merged != NULL) {
        memcpy(merged, cur, lo);
        memcpy(merged + lo, text, p_len);
        strcpy(merged + lo + p_len, cur + hi);
        (void)fdk_entry_set_text(app.entry, merged);
        fdk_entry_set_cursor(app.entry, lo + p_len);
        free(merged);
    }
    set_status(app.clip_status, "Pasted %zu characters", p_len);
    printf("PHASE: clip paste %zu\n", p_len);
    fflush(stdout);
    free(text);
}

static void clip_clear_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    (void)fdk_entry_set_text(app.entry, "");
    fdk_entry_set_cursor(app.entry, 0);
    set_status(app.clip_status, "Cleared the input");
    printf("PHASE: clip clear 0\n");
    fflush(stdout);
}

/* Pulls whatever another application left on the clipboard into the
 * preview line: length + the first bytes, newlines escaped. */
static void clip_read_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    char *text = fdk_clipboard_get_text(app.ctx);
    if (text == NULL) {
        set_status(app.clip_preview, "(unreadable or empty)");
        set_status(app.clip_status, "Read: nothing to read");
        printf("PHASE: clip read 0\n");
        fflush(stdout);
        return;
    }
    size_t len = strlen(text);
    char shown[120];
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < sizeof(shown); i++) {
        if (text[i] == '\n') {
            o += (size_t)snprintf(shown + o, sizeof(shown) - o, "\\n");
        } else if (text[i] == '\r') {
            o += (size_t)snprintf(shown + o, sizeof(shown) - o, "\\r");
        } else if (text[i] == '\t') {
            o += (size_t)snprintf(shown + o, sizeof(shown) - o, "\\t");
        } else {
            shown[o++] = text[i];
        }
    }
    shown[o] = '\0';
    set_status(app.clip_preview, "\"%s%s\"", shown,
               len > 100 ? "..." : "");
    set_status(app.clip_status, "Read %zu characters from the clipboard",
               len);
    printf("PHASE: clip read %zu\n", len);
    fflush(stdout);
    free(text);
}

/* Plants a timestamped string FOR other apps — paste it somewhere
 * else to see the seam work in that direction. */
static void clip_set_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    char buf[128];
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(buf, sizeof(buf), "Hello from FDK at %H:%M:%S!", &tm_buf);
    fdk_result r = fdk_clipboard_set_text(app.ctx, buf);
    if (fdk_ok(r)) {
        set_status(app.clip_status, "Set: \"%s\" — paste it elsewhere",
                   buf);
        printf("PHASE: clip set %zu\n", strlen(buf));
    } else {
        set_status(app.clip_status, "Set failed (%s)",
                   fdk_result_to_string(r));
        printf("PHASE: clip set-failed\n");
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* text input section (the typing playground)                          */
/* ------------------------------------------------------------------ */

static void typing_report(void) {
    const char *text = fdk_entry_get_text(app.typing);
    size_t bytes = (text != NULL) ? strlen(text) : 0;
    size_t caret = fdk_entry_get_cursor(app.typing);
    size_t anchor = 0, caret2 = 0;
    size_t lo = 0, hi = 0;
    if (fdk_ok(fdk_entry_get_selection(app.typing, &anchor, &caret2))) {
        lo = anchor <= caret2 ? anchor : caret2;
        hi = anchor <= caret2 ? caret2 : anchor;
    }
    set_status(app.typing_status,
               "%zu bytes · caret @ %zu · selection %zu..%zu%s",
               bytes, caret, lo, hi, lo != hi ? "" : " (none)");
    printf("PHASE: type %zu %zu\n", bytes, caret);
    fflush(stdout);
}

static void typing_changed(fdk_widget *entry, void *user) {
    (void)entry; (void)user;
    typing_report();
}

static void typing_activated(fdk_widget *entry, void *user) {
    (void)entry; (void)user;
    const char *text = fdk_entry_get_text(app.typing);
    set_status(app.typing_commit, "Committed: \"%s\"",
               (text != NULL && text[0]) ? text : "(empty)");
}

static void password_toggled(fdk_widget *w, bool checked, void *user) {
    (void)w; (void)user;
    fdk_entry_set_password(app.typing, checked);
    typing_report();
}

static void readonly_toggled(fdk_widget *w, bool checked, void *user) {
    (void)w; (void)user;
    fdk_entry_set_read_only(app.typing, checked);
}

/* ------------------------------------------------------------------ */
/* drop target section                                                 */
/* ------------------------------------------------------------------ */

static void report_drop(const fdk_event_data *ev) {
    const fdk_drag_event *d = &ev->drag;
    char summary[512];
    if (d->uris != NULL && d->uri_count > 0) {
        size_t files = 0, folders = 0;
        for (size_t i = 0; i < d->uri_count; i++) {
            struct stat st;
            if (stat(d->uris[i], &st) == 0 && S_ISDIR(st.st_mode)) {
                folders++;
            } else {
                files++;
            }
        }
        const char *kind = (folders > 0 && files == 0) ? "folder" : "files";
        set_status(app.drop_status, "Last drop: %s (%zu folder(s), "
                                    "%zu file(s)) — first: %s",
                   kind, folders, files, d->uris[0]);
        snprintf(summary, sizeof(summary), "%s %zu", kind, d->uri_count);
        (void)fdk_label_set_text(app.drop_panel, summary);
    } else if (d->text != NULL) {
        set_status(app.drop_status, "Last drop: text \"%s\"", d->text);
        snprintf(summary, sizeof(summary), "text: %s", d->text);
        (void)fdk_label_set_text(app.drop_panel, summary);
    } else {
        set_status(app.drop_status, "Last drop: empty payload");
        (void)fdk_label_set_text(app.drop_panel, "(empty)");
        snprintf(summary, sizeof(summary), "text 0");
    }
    printf("PHASE: drop %s\n", summary);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* dialogs section                                                     */
/* ------------------------------------------------------------------ */

static const char *kind_verb(fdk_file_dialog_kind kind) {
    switch (kind) {
    case FDK_FILE_DIALOG_OPEN_FILES: return "Files";
    case FDK_FILE_DIALOG_OPEN_FOLDER: return "Folder";
    case FDK_FILE_DIALOG_OPEN_FOLDERS: return "Folders";
    case FDK_FILE_DIALOG_OPEN_FILE:
    default: return "File";
    }
}

static void file_dialog_done(const fdk_file_dialog_result *result,
                             void *user) {
    fdk_file_dialog_kind kind = (fdk_file_dialog_kind)(intptr_t)user;
    const char *verb = kind_verb(kind);
    switch (result->outcome) {
    case FDK_FILE_DIALOG_ACCEPTED:
        {
            char buf[512];
            int off = snprintf(buf, sizeof(buf), "Selected (%s): ",
                               verb);
            for (size_t i = 0;
                 i < result->count && off < (int)sizeof(buf) - 2;
                 i++) {
                off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                                "%s%s", i ? ", " : "",
                                result->paths[i]);
            }
            set_status(app.dialog_status, "%s", buf);
            printf("PHASE: dialog accepted %zu\n", result->count);
        }
        break;
    case FDK_FILE_DIALOG_CANCELLED:
        set_status(app.dialog_status, "Cancelled (%s)", verb);
        printf("PHASE: dialog cancelled 0\n");
        break;
    case FDK_FILE_DIALOG_ERROR:
    default:
        set_status(app.dialog_status, "Error (%s)", verb);
        printf("PHASE: dialog error 0\n");
        break;
    }
    fflush(stdout);
}

static void open_dialog(fdk_file_dialog_kind kind) {
    fdk_file_dialog_options opts = {0};
    opts.kind = kind;
    opts.title = kind_verb(kind);
    opts.modal = true;
    fdk_result r = fdk_dialog_open_file(app.ctx, &opts, file_dialog_done,
                                        (void *)(intptr_t)kind, NULL);
    if (!fdk_ok(r)) {
        set_status(app.dialog_status, "Dialog failed to open (%s)",
                   fdk_result_to_string(r));
        printf("PHASE: dialog error 0\n");
        fflush(stdout);
    }
}

static void dlg_file_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    open_dialog(FDK_FILE_DIALOG_OPEN_FILE);
}
static void dlg_files_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    open_dialog(FDK_FILE_DIALOG_OPEN_FILES);
}
static void dlg_folder_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    open_dialog(FDK_FILE_DIALOG_OPEN_FOLDER);
}
static void dlg_folders_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    open_dialog(FDK_FILE_DIALOG_OPEN_FOLDERS);
}

static void ask_done(fdk_dialog_response response, void *user) {
    (void)user;
    const char *answer = (response == FDK_DIALOG_YES)   ? "yes"
                         : (response == FDK_DIALOG_NO)  ? "no"
                                                        : "dismissed";
    set_status(app.dialog_status, "You answered: %s", answer);
    printf("PHASE: dialog %s 0\n",
           response == FDK_DIALOG_YES    ? "yes"
           : response == FDK_DIALOG_NO   ? "no"
                                         : "cancelled");
    fflush(stdout);
}

static void ask_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    fdk_dialog_options opts = {0};
    opts.title = "Question";
    opts.text = "Does the message dialog answer honestly?";
    opts.buttons = FDK_DIALOG_BUTTONS_YES_NO;
    fdk_result r = fdk_dialog_show_message(app.ctx, &opts, ask_done,
                                           NULL, NULL);
    if (!fdk_ok(r)) {
        set_status(app.dialog_status, "Message dialog failed (%s)",
                   fdk_result_to_string(r));
    }
}

/* ------------------------------------------------------------------ */
/* drag source section                                                 */
/* ------------------------------------------------------------------ */

static void drag_payload_files(char *a, size_t a_cap, char *b,
                               size_t b_cap) {
    snprintf(a, a_cap, "%s", "/etc/hostname");
    snprintf(b, b_cap, "%s", "/etc/os-release");
}

static void drag_done(fdk_drag_status status, void *user) {
    (void)user;
    const char *s = (status == FDK_DRAG_SUCCEEDED)   ? "succeeded"
                    : (status == FDK_DRAG_CANCELLED) ? "cancelled"
                                                     : "failed";
    set_status(app.drag_status, "Drag %s", s);
    printf("PHASE: drag %s\n", s);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* window events                                                       */
/* ------------------------------------------------------------------ */

static bool in_drop_panel(fdk_f32 x, fdk_f32 y) {
    return x >= app.drop_rect.x &&
           x < app.drop_rect.x + app.drop_rect.width &&
           y >= app.drop_rect.y &&
           y < app.drop_rect.y + app.drop_rect.height;
}

static void relayout(void); /* defined in the layout section below */

static void window_event(fdk_window *window, const fdk_event_data *event,
                         void *user) {
    (void)window; (void)user;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (event->type == FDK_EVENT_KEY_DOWN &&
         event->key.scancode == FDK_KEY_ESC)) {
        app.quit = true;
        return;
    }
    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        relayout(); /* re-flow every section to the new size */
        return;
    }
    if (event->type >= FDK_EVENT_DRAG_ENTER &&
        event->type <= FDK_EVENT_DRAG_DROP) {
        const fdk_drag_event *d = &event->drag;
        switch (event->type) {
        case FDK_EVENT_DRAG_ENTER:
        case FDK_EVENT_DRAG_MOTION:
            if (in_drop_panel(d->position.x, d->position.y)) {
                (void)fdk_label_set_text(app.drop_panel, "drop it!");
            }
            break;
        case FDK_EVENT_DRAG_LEAVE:
            (void)fdk_label_set_text(app.drop_panel,
                                     "DROP FILES OR TEXT HERE");
            break;
        case FDK_EVENT_DRAG_DROP:
            (void)fdk_label_set_text(app.drop_panel,
                                     "DROP FILES OR TEXT HERE");
            report_drop(event);
            break;
        default:
            break;
        }
    }
}

/* Press on the drag panel + motion = start the drag (the standard
 * threshold-free v1 gesture: press-and-move from the panel). */
static bool drag_panel_event(fdk_widget *w, const fdk_widget_event *ev,
                             void *user) {
    (void)w; (void)user;
    static bool pressed = false;
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        pressed = true;
        break;
    case FDK_WIDGET_POINTER_UP:
        pressed = false;
        break;
    case FDK_WIDGET_POINTER_MOTION:
        if (pressed) {
            pressed = false;
            char file_a[256], file_b[256];
            drag_payload_files(file_a, sizeof(file_a), file_b,
                               sizeof(file_b));
            const char *uris[2] = { file_a, file_b };
            const char *text = "Hello from FDK capabilities!";
            fdk_result r = fdk_drag_begin(
                app.window,
                FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST, text,
                uris, 2, drag_done, NULL);
            if (!fdk_ok(r)) {
                set_status(app.drag_status,
                           "Drag could not start (%s)",
                           fdk_result_to_string(r));
                printf("PHASE: drag failed\n");
                fflush(stdout);
            } else {
                set_status(app.drag_status, "Drag running...");
            }
        }
        break;
    default:
        break;
    }
    return false; /* let the motion keep flowing (hover etc.) */
}

/* ------------------------------------------------------------------ */
/* layout (resize-aware; every widget is placed from window size)      */
/* ------------------------------------------------------------------ */

/* Places a measured button row left-to-right from x. */
static void lay_button_row(fdk_widget **btns, size_t n, fdk_i32 x,
                           fdk_i32 y, fdk_i32 h) {
    for (size_t i = 0; i < n; i++) {
        fdk_size m = {0, 0};
        fdk_widget_measure(btns[i], &m);
        fdk_widget_set_bounds(btns[i], (fdk_rect){x, y, m.width, h});
        x += m.width + 8;
    }
}

static void relayout(void) {
    fdk_size ws;
    (void)fdk_window_get_size(app.window, &ws);
    fdk_i32 x = 12;
    fdk_i32 w = ws.width - 24;
    if (w < 320) {
        w = 320;
    }
    fdk_i32 y = 10;

    /* CLIPBOARD */
    fdk_widget_set_bounds(app.h_clip, (fdk_rect){x, y, w, 20});
    y += 24;
    fdk_widget_set_bounds(app.entry, (fdk_rect){x, y, w, 32});
    y += 40;
    lay_button_row(app.clip_btns, 6, x, y, 28);
    y += 34;
    fdk_widget_set_bounds(app.clip_status, (fdk_rect){x, y, w, 20});
    y += 22;
    fdk_widget_set_bounds(app.clip_preview, (fdk_rect){x, y, w, 20});
    y += 30;

    /* TEXT INPUT */
    fdk_widget_set_bounds(app.h_type, (fdk_rect){x, y, w, 20});
    y += 24;
    fdk_widget_set_bounds(app.typing, (fdk_rect){x, y, w, 36});
    y += 44;
    fdk_i32 tx = x;
    fdk_size tm = {0, 0};
    fdk_widget_measure(app.toggle_password, &tm);
    fdk_widget_set_bounds(app.toggle_password,
                          (fdk_rect){tx, y, tm.width, 28});
    tx += tm.width + 8;
    fdk_widget_measure(app.toggle_readonly, &tm);
    fdk_widget_set_bounds(app.toggle_readonly,
                          (fdk_rect){tx, y, tm.width, 28});
    y += 34;
    fdk_widget_set_bounds(app.typing_status, (fdk_rect){x, y, w, 20});
    y += 22;
    fdk_widget_set_bounds(app.typing_commit, (fdk_rect){x, y, w, 20});
    y += 30;

    /* DROP TARGET */
    fdk_widget_set_bounds(app.h_drop, (fdk_rect){x, y, w, 20});
    y += 24;
    app.drop_rect = (fdk_rect){x, y, w, 84};
    fdk_widget_set_bounds(app.drop_panel, app.drop_rect);
    y += 90;
    fdk_widget_set_bounds(app.drop_status, (fdk_rect){x, y, w, 20});
    y += 30;

    /* DIALOGS */
    fdk_widget_set_bounds(app.h_dlg, (fdk_rect){x, y, w, 20});
    y += 24;
    lay_button_row(app.dlg_btns, 5, x, y, 28);
    y += 34;
    fdk_widget_set_bounds(app.dialog_status, (fdk_rect){x, y, w, 20});
    y += 30;

    /* DRAG SOURCE */
    fdk_widget_set_bounds(app.h_drag, (fdk_rect){x, y, w, 20});
    y += 24;
    app.drag_rect = (fdk_rect){x, y, w, 52};
    fdk_widget_set_bounds(app.drag_panel, app.drag_rect);
    y += 58;
    fdk_widget_set_bounds(app.drag_status, (fdk_rect){x, y, w, 20});
}

/* ------------------------------------------------------------------ */

int main(void) {
    memset(&app, 0, sizeof(app));

    fdk_init_options init = {0};
    init.app_id = "09_capabilities";
    if (!fdk_ok(fdk_init(&app.ctx, &init))) {
        fprintf(stderr, "09_capabilities: no display — see docs\n");
        return 1;
    }
    app.font = fdk_font_load_system_default(15);
    if (app.font == NULL) {
        fprintf(stderr, "09_capabilities: no system font\n");
        fdk_shutdown(app.ctx);
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK Capabilities",
        .width = 660,
        .height = 780,
    };
    if (!fdk_ok(fdk_window_create(app.ctx, &wopts, &app.window))) {
        fdk_font_destroy(app.font);
        fdk_shutdown(app.ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);
    fdk_window_set_drop_formats(app.window,
                                FDK_DRAG_FORMAT_TEXT |
                                    FDK_DRAG_FORMAT_URI_LIST);

    (void)fdk_window_get_root(app.window, &app.root);
    fdk_color panel =
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);

    /* ---- CLIPBOARD ---- */
    (void)fdk_label_create(app.root, app.font, "CLIPBOARD", &app.h_clip);
    (void)fdk_entry_create(app.root, app.font, NULL, &app.entry);
    (void)fdk_entry_set_text(app.entry, "select some of this text");
    fdk_entry_select_range(app.entry, 0, 9);

    (void)fdk_button_create(app.root, app.font, "Copy", &app.clip_btns[0]);
    (void)fdk_button_create(app.root, app.font, "Cut", &app.clip_btns[1]);
    (void)fdk_button_create(app.root, app.font, "Paste",
                            &app.clip_btns[2]);
    (void)fdk_button_create(app.root, app.font, "Clear",
                            &app.clip_btns[3]);
    (void)fdk_button_create(app.root, app.font, "Read clipboard",
                            &app.clip_btns[4]);
    (void)fdk_button_create(app.root, app.font, "Set greeting",
                            &app.clip_btns[5]);
    fdk_button_set_on_activate(app.clip_btns[0], clip_copy_clicked, NULL);
    fdk_button_set_on_activate(app.clip_btns[1], clip_cut_clicked, NULL);
    fdk_button_set_on_activate(app.clip_btns[2], clip_paste_clicked, NULL);
    fdk_button_set_on_activate(app.clip_btns[3], clip_clear_clicked, NULL);
    fdk_button_set_on_activate(app.clip_btns[4], clip_read_clicked, NULL);
    fdk_button_set_on_activate(app.clip_btns[5], clip_set_clicked, NULL);

    (void)fdk_label_create(app.root, app.font, "Last operation: -",
                           &app.clip_status);
    (void)fdk_label_create(app.root, app.font, "Clipboard: (never read)",
                           &app.clip_preview);
    fdk_label_set_mode(app.clip_preview, FDK_LABEL_ELLIPSIZE);

    /* ---- TEXT INPUT (the typing playground) ---- */
    (void)fdk_label_create(app.root, app.font, "TEXT INPUT", &app.h_type);
    (void)fdk_entry_create(app.root, app.font, NULL, &app.typing);
    fdk_entry_set_on_changed(app.typing, typing_changed, NULL);
    fdk_entry_set_on_activate(app.typing, typing_activated, NULL);
    (void)fdk_toggle_create(app.root, app.font, "Password",
                            &app.toggle_password);
    fdk_toggle_set_on_change(app.toggle_password, password_toggled, NULL);
    (void)fdk_toggle_create(app.root, app.font, "Read-only",
                            &app.toggle_readonly);
    fdk_toggle_set_on_change(app.toggle_readonly, readonly_toggled, NULL);
    (void)fdk_label_create(app.root, app.font, "0 bytes · caret @ 0",
                           &app.typing_status);
    (void)fdk_label_create(app.root, app.font,
                           "Committed: (press Enter inside the box)",
                           &app.typing_commit);

    /* ---- DROP TARGET ---- */
    (void)fdk_label_create(app.root, app.font, "DROP TARGET", &app.h_drop);
    (void)fdk_label_create(app.root, app.font,
                           "DROP FILES OR TEXT HERE", &app.drop_panel);
    fdk_widget_set_background(app.drop_panel, panel);
    fdk_label_set_alignment(app.drop_panel, FDK_ALIGN_CENTER);
    (void)fdk_label_create(app.root, app.font, "Last drop: -",
                           &app.drop_status);

    /* ---- DIALOGS ---- */
    (void)fdk_label_create(app.root, app.font, "FILE SELECTION",
                           &app.h_dlg);
    (void)fdk_button_create(app.root, app.font, "Select File",
                            &app.dlg_btns[0]);
    (void)fdk_button_create(app.root, app.font, "Select Files",
                            &app.dlg_btns[1]);
    (void)fdk_button_create(app.root, app.font, "Select Folder",
                            &app.dlg_btns[2]);
    (void)fdk_button_create(app.root, app.font, "Select Folders",
                            &app.dlg_btns[3]);
    (void)fdk_button_create(app.root, app.font, "Ask Yes/No",
                            &app.dlg_btns[4]);
    fdk_button_set_on_activate(app.dlg_btns[0], dlg_file_clicked, NULL);
    fdk_button_set_on_activate(app.dlg_btns[1], dlg_files_clicked, NULL);
    fdk_button_set_on_activate(app.dlg_btns[2], dlg_folder_clicked, NULL);
    fdk_button_set_on_activate(app.dlg_btns[3], dlg_folders_clicked, NULL);
    fdk_button_set_on_activate(app.dlg_btns[4], ask_clicked, NULL);
    (void)fdk_label_create(app.root, app.font, "Outcome: -",
                           &app.dialog_status);
    fdk_label_set_mode(app.dialog_status, FDK_LABEL_ELLIPSIZE);

    /* ---- DRAG SOURCE ---- */
    (void)fdk_label_create(app.root, app.font, "DRAG SOURCE", &app.h_drag);
    (void)fdk_label_create(app.root, app.font,
                           "press and drag from here -> another app",
                           &app.drag_panel);
    fdk_widget_set_background(app.drag_panel, panel);
    fdk_label_set_alignment(app.drag_panel, FDK_ALIGN_CENTER);
    fdk_widget_set_event_callback(app.drag_panel, drag_panel_event, NULL);
    (void)fdk_label_create(app.root, app.font, "Last drag: -",
                           &app.drag_status);

    relayout();
    fdk_window_show(app.window);

    /* Announce interactive geometry for the rigs. */
    rig_announce("drop-panel", app.drop_panel);
    rig_announce("drag-panel", app.drag_panel);
    rig_announce("entry-typing", app.typing);
    rig_announce("clip-status", app.clip_status);
    rig_announce("typing-status", app.typing_status);
    rig_announce("btn-copy", app.clip_btns[0]);
    rig_announce("btn-paste", app.clip_btns[2]);
    rig_announce("btn-clipread", app.clip_btns[4]);
    rig_announce("btn-clipset", app.clip_btns[5]);
    rig_announce("btn-file", app.dlg_btns[0]);
    rig_announce("btn-files", app.dlg_btns[1]);
    rig_announce("btn-folder", app.dlg_btns[2]);
    rig_announce("btn-folders", app.dlg_btns[3]);
    rig_announce("btn-ask", app.dlg_btns[4]);

    const char *frames = getenv("FDK_DEMO_FRAMES");
    app.frames_left = frames != NULL ? atoi(frames) : -1;

    while (!app.quit) {
        (void)fdk_pump_events(app.ctx, 15);
        /* The 1.2.0 build never painted here — every interaction's
         * damage waited for a resize to reach the screen (and the
         * resize-time repaint stamped new text over old, the ghosting
         * report). Paint the tree whenever it has damage: the
         * documented app pattern, same as 04_widgets.c. */
        if (fdk_widget_tree_has_damage(app.root)) {
            (void)fdk_window_paint(app.window);
        }
        if (app.frames_left > 0 && --app.frames_left == 0) {
            break;
        }
    }

    printf("PHASE: quit\n");
    fflush(stdout);
    fdk_font_destroy(app.font);
    fdk_shutdown(app.ctx);
    return 0;
}
