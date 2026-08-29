/* 09_capabilities.c — the real-world capability showcase (1.2.0).
 *
 * One application that answers "what can FDK do with the desktop?"
 * in four sections, because that is the honest way to validate the
 * platform seams this milestone added:
 *
 *   CLIPBOARD   an Entry wired to Copy / Cut / Paste / Clear buttons
 *               (the Entry's stock Ctrl+X/C/V work too) and a
 *               "last operation" line narrating every action. Paste
 *               from a REAL other application to test interop.
 *
 *   DROP TARGET a big panel that highlights while a drag hovers it
 *               and reports the last drop: type (text / N files /
 *               folder), names and paths, or the text contents.
 *               Drop things from your file manager or editor.
 *
 *   DIALOGS     [Select File] [Select Files] [Select Folder]
 *               [Select Folders] open the FDK file dialog; the
 *               outcome line distinguishes accepted (with every
 *               path listed — multi-selection is never silently
 *               discarded), cancelled, and error.
 *
 *   DRAG SOURCE a "drag me" panel: press and drag from it into any
 *               other application — text into an editor, or the two
 *               files it advertises into a file manager. The status
 *               line reports how the drag ended.
 *
 * For the GUI test rigs the demo prints:
 *   RIG: drop-panel <x> <y> <w> <h>      — where to drop
 *   RIG: drag-panel <x> <y> <w> <h>      — where to press-drag from
 *   RIG: btn-copy|btn-paste|btn-file|... <x> <y> <w> <h>
 *   PHASE: clip <op> <n>                 — copy/cut/paste results
 *   PHASE: drop <text|files|folder> <n>  — decoded drops
 *   PHASE: dialog <accepted|cancelled|error> <count>
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

/* ---- app state ---- */

static struct {
    fdk_context *ctx;
    fdk_window *window;
    fdk_font *font;
    bool quit;
    int frames_left;
    /* clipboard section */
    fdk_widget *entry;
    fdk_widget *clip_status;
    /* drop section */
    fdk_widget *drop_panel;   /* the highlight target (a Frame) */
    fdk_rect drop_rect;       /* panel bounds in WINDOW coords   */
    fdk_widget *drop_status;
    /* dialog section */
    fdk_widget *dialog_status;
    /* drag section */
    fdk_widget *drag_panel;
    fdk_rect drag_rect;
    fdk_widget *drag_status;
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

/* Announces a widget's absolute bounds for the rigs: walks it up to
 * window coordinates the same way the widget layer lays children
 * out (parents carry absolute bounds). */
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

/* Entry selection extent via the public get_selection API. */
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
            /* delete the selected bytes: re-set the text around them */
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

/* ------------------------------------------------------------------ */
/* drop target section                                                 */
/* ------------------------------------------------------------------ */

static void report_drop(const fdk_event_data *ev) {
    const fdk_drag_event *d = &ev->drag;
    char summary[512];
    if (d->uris != NULL && d->uri_count > 0) {
        /* Classify honestly: folders vs files via stat, per entry. */
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
                   kind, folders, files,
                   d->uris[0]);
        snprintf(summary, sizeof(summary), "%s %zu", kind,
                 d->uri_count);
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
        /* List EVERY path — the multi contract: nothing discarded. */
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

/* ------------------------------------------------------------------ */
/* drag source section                                                 */
/* ------------------------------------------------------------------ */

/* The payload the drag panel offers: a short text plus two REAL
 * files (this example's own source lives beside the binary; the
 * Makefile copies nothing, so resolve defensively). */
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

static void window_event(fdk_window *window, const fdk_event_data *event,
                         void *user) {
    (void)window; (void)user;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (event->type == FDK_EVENT_KEY_DOWN &&
         event->key.scancode == FDK_KEY_ESC)) {
        app.quit = true;
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
/* layout                                                              */
/* ------------------------------------------------------------------ */

/* The full layout lives in main() below — the sections are placed
 * once (fixed default window size) and the RIG lines are printed
 * after the first configure. */

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
        .width = 640,
        .height = 660,
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

    fdk_widget *root = NULL;
    (void)fdk_window_get_root(app.window, &root);

    fdk_i32 y = 10;
    fdk_i32 x = 12;

    /* ---- clipboard section ---- */
    fdk_widget *h1 = NULL;
    (void)fdk_label_create(root, app.font, "CLIPBOARD", &h1);
    fdk_widget_set_bounds(h1, (fdk_rect){x, y, 300, 20});
    y += 24;
    (void)fdk_entry_create(root, app.font, NULL, &app.entry);
    fdk_widget_set_bounds(app.entry, (fdk_rect){x, y, 480, 32});
    (void)fdk_entry_set_text(app.entry, "select some of this text");
    fdk_entry_select_range(app.entry, 0, 9);
    y += 40;

    fdk_widget *buttons[4] = {0};
    (void)fdk_button_create(root, app.font, "Copy", &buttons[0]);
    (void)fdk_button_create(root, app.font, "Cut", &buttons[1]);
    (void)fdk_button_create(root, app.font, "Paste", &buttons[2]);
    (void)fdk_button_create(root, app.font, "Clear", &buttons[3]);
    fdk_button_set_on_activate(buttons[0], clip_copy_clicked, NULL);
    fdk_button_set_on_activate(buttons[1], clip_cut_clicked, NULL);
    fdk_button_set_on_activate(buttons[2], clip_paste_clicked, NULL);
    fdk_button_set_on_activate(buttons[3], clip_clear_clicked, NULL);
    fdk_i32 bx = x;
    for (int i = 0; i < 4; i++) {
        fdk_size n = {0, 0};
        fdk_widget_measure(buttons[i], &n);
        fdk_widget_set_bounds(buttons[i],
                              (fdk_rect){bx, y, n.width, 28});
        bx += n.width + 8;
    }
    y += 34;
    (void)fdk_label_create(root, app.font, "Last operation: -",
                           &app.clip_status);
    fdk_widget_set_bounds(app.clip_status, (fdk_rect){x, y, 600, 20});
    y += 30;

    /* ---- drop target section ---- */
    fdk_widget *h2 = NULL;
    (void)fdk_label_create(root, app.font, "DROP TARGET", &h2);
    fdk_widget_set_bounds(h2, (fdk_rect){x, y, 300, 20});
    y += 24;
    (void)fdk_label_create(root, app.font, "DROP FILES OR TEXT HERE",
                           &app.drop_panel);
    app.drop_rect = (fdk_rect){x, y, 600, 90};
    fdk_widget_set_bounds(app.drop_panel, app.drop_rect);
    fdk_widget_set_background(
        app.drop_panel,
        fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));
    y += 96;
    (void)fdk_label_create(root, app.font, "Last drop: -",
                           &app.drop_status);
    fdk_widget_set_bounds(app.drop_status, (fdk_rect){x, y, 610, 20});
    y += 30;

    /* ---- dialogs section ---- */
    fdk_widget *h3 = NULL;
    (void)fdk_label_create(root, app.font, "FILE SELECTION", &h3);
    fdk_widget_set_bounds(h3, (fdk_rect){x, y, 300, 20});
    y += 24;
    fdk_widget *db[4] = {0};
    (void)fdk_button_create(root, app.font, "Select File", &db[0]);
    (void)fdk_button_create(root, app.font, "Select Files", &db[1]);
    (void)fdk_button_create(root, app.font, "Select Folder", &db[2]);
    (void)fdk_button_create(root, app.font, "Select Folders", &db[3]);
    fdk_button_set_on_activate(db[0], dlg_file_clicked, NULL);
    fdk_button_set_on_activate(db[1], dlg_files_clicked, NULL);
    fdk_button_set_on_activate(db[2], dlg_folder_clicked, NULL);
    fdk_button_set_on_activate(db[3], dlg_folders_clicked, NULL);
    bx = x;
    for (int i = 0; i < 4; i++) {
        fdk_size n = {0, 0};
        fdk_widget_measure(db[i], &n);
        fdk_widget_set_bounds(db[i], (fdk_rect){bx, y, n.width, 28});
        bx += n.width + 8;
    }
    y += 34;
    (void)fdk_label_create(root, app.font, "Outcome: -",
                           &app.dialog_status);
    fdk_widget_set_bounds(app.dialog_status,
                          (fdk_rect){x, y, 610, 20});
    y += 30;

    /* ---- drag source section ---- */
    fdk_widget *h4 = NULL;
    (void)fdk_label_create(root, app.font, "DRAG SOURCE", &h4);
    fdk_widget_set_bounds(h4, (fdk_rect){x, y, 300, 20});
    y += 24;
    (void)fdk_label_create(root, app.font,
                           "press and drag from here -> another app",
                           &app.drag_panel);
    app.drag_rect = (fdk_rect){x, y, 600, 56};
    fdk_widget_set_bounds(app.drag_panel, app.drag_rect);
    fdk_widget_set_event_callback(app.drag_panel, drag_panel_event,
                                  NULL);
    y += 62;
    (void)fdk_label_create(root, app.font, "Last drag: -",
                           &app.drag_status);
    fdk_widget_set_bounds(app.drag_status, (fdk_rect){x, y, 610, 20});

    fdk_window_show(app.window);

    /* Announce interactive geometry for the rigs. */
    rig_announce("drop-panel", app.drop_panel);
    rig_announce("drag-panel", app.drag_panel);
    rig_announce("btn-copy", buttons[0]);
    rig_announce("btn-paste", buttons[2]);
    rig_announce("btn-file", db[0]);
    rig_announce("btn-files", db[1]);
    rig_announce("btn-folder", db[2]);
    rig_announce("btn-folders", db[3]);

    const char *frames = getenv("FDK_DEMO_FRAMES");
    app.frames_left = frames != NULL ? atoi(frames) : -1;

    while (!app.quit) {
        (void)fdk_pump_events(app.ctx, 15);
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
