/*
 * fdk_dialog.h — Faded Dream ToolKit: message dialogs (Phase 9)
 *
 * A dialog is a real top-level window (decorated like any other —
 * WM/compositor title bar, taskbar presence, stacking above its
 * siblings) built from the stock widget catalog: a text label, a
 * hairline, and a button row. FDK owns the whole lifecycle:
 * auto-paint keeps it on screen without the application driving it,
 * the response callback fires once, and the dialog destroys itself
 * afterwards (fire-and-forget message boxes — the standard shape;
 * long-lived tool palettes are ordinary windows, not dialogs).
 *
 * MODALITY: on X11, modal=true takes a pointer+keyboard grab on the
 * dialog — no other window of the process receives input until it
 * closes, and presses outside the dialog are swallowed (the modal
 * contract: input waits for the dialog). On Wayland there is no
 * protocol for a client to grab input to a toplevel (xdg-dialog-v1
 * is a compositor hint, not a grab), so dialogs there are always
 * non-modal — the option is accepted and ignored, documented
 * honestly rather than faked with a fake "modal" that isn't.
 *
 * Keyboard: Enter activates the affirmative button (OK / Yes),
 * Escape answers CANCEL (or NO, in a YES_NO dialog), Tab/arrows
 * walk the buttons, and the WM close button answers CANCEL/CLOSE —
 * all before the response callback runs.
 *
 * The application's event loop keeps pumping (fdk_pump_events) —
 * that is what delivers the dialog its input and paints it; a modal
 * dialog does NOT block inside fdk_dialog_show_message (nothing in
 * FDK ever blocks: no nested event loops, docs/threading.md). Apps
 * wanting blocking semantics gate their own loop on a flag the
 * response callback clears.
 */

#ifndef FDK_DIALOG_H
#define FDK_DIALOG_H

#include "fdk_core.h"
#include "fdk_error.h"
#include "fdk_text.h"
#include "fdk_types.h"
#include "fdk_window.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which button set a message dialog shows. */
typedef enum fdk_dialog_buttons {
    FDK_DIALOG_BUTTONS_OK = 0,          /* [ OK ]                    */
    FDK_DIALOG_BUTTONS_OK_CANCEL = 1,   /* [ OK ] [ Cancel ]         */
    FDK_DIALOG_BUTTONS_YES_NO = 2,      /* [ Yes ] [ No ]            */
    FDK_DIALOG_BUTTONS_YES_NO_CANCEL = 3, /* [ Yes ] [ No ] [ Cancel] */
    FDK_DIALOG_BUTTONS_CLOSE = 4,       /* [ Close ]                 */
} fdk_dialog_buttons;

/* The answer a dialog reports. Negative = dismissed without an
 * affirmative choice (window closed, Escape, Cancel). */
typedef enum fdk_dialog_response {
    FDK_DIALOG_CANCEL = -1,
    FDK_DIALOG_OK     = 0,
    FDK_DIALOG_YES    = 1,
    FDK_DIALOG_NO     = 2,
    FDK_DIALOG_CLOSE  = 3,
} fdk_dialog_response;

/* Input struct (zero-init = title "Message", empty text, OK button,
 * non-modal, system-default font, no parent). Fields only append —
 * see docs/abi-policy.md. */
typedef struct fdk_dialog_options {
    const char *title;        /* copied; NULL = "Message"          */
    const char *text;         /* copied; NULL = ""                 */
    fdk_dialog_buttons buttons;
    bool modal;               /* X11: input grab; Wayland: ignored */
    fdk_font *font;           /* borrowed; NULL = system default   */
    fdk_window *parent;       /* borrowed; anchors stacking/positioning
                                 where the backend supports it     */
} fdk_dialog_options;

/* The response callback: runs once (from inside event dispatch),
 * after which FDK destroys the dialog window. Destroying other
 * widgets/windows from it is safe (the usual reentrancy rules). */
typedef void (*fdk_dialog_response_fn)(fdk_dialog_response response,
                                       void *user_data);

/* Shows a message dialog on `ctx` (the application's context — same
 * connection, same event loop). The dialog window is TOOLKIT-OWNED:
 * auto-painted, self-destroying after the response; *out_window may
 * be NULL (fire-and-forget) or used to fdk_window_destroy() it early
 * (e.g. when the parent window is closing — answers CANCEL, fires
 * the callback, cleans up).
 *
 * Can fail with FDK_ERR_INVALID_ARGUMENT (NULL ctx/callback-target
 * combination — user_data without fn is fine), FDK_ERR_OUT_OF_MEMORY,
 * FDK_ERR_NOT_INITIALIZED, or FDK_ERR_WINDOW_CREATE — in which case
 * no dialog exists.
 */
fdk_result fdk_dialog_show_message(fdk_context *ctx,
                                   const fdk_dialog_options *options,
                                   fdk_dialog_response_fn on_response,
                                   void *user_data,
                                   fdk_window **out_window);

/* ---- Prompt (text input) dialogs (1.2.1) ----
 *
 * The message dialog's twin for the other half of the ask-the-user
 * job: one question, one text box, OK / Cancel. Built from the same
 * stock catalog (wrapping Label, an Entry, a button row) with the
 * same lifecycle: toolkit-owned window, auto-painted, exactly one
 * callback, self-destroying afterwards.
 *
 * The Entry takes the initial focus — type immediately; Enter in the
 * Entry answers OK (the stock Entry activation), Escape answers
 * CANCEL (an Entry with an active selection collapses it first, then
 * a second Escape bubbles), Tab walks Entry -> OK -> Cancel. The
 * initial value, when given, starts SELECTED so typing replaces it —
 * the rename-everywhere convention.
 *
 * The text contract mirrors the file dialog's explicitness: OK hands
 * `text` pointing at the (possibly empty, possibly edited) answer,
 * valid only during the callback; every other response hands NULL.
 * Never "empty string means maybe cancel". */

typedef struct fdk_prompt_dialog_options {
    const char *title;    /* copied; NULL = "Input"               */
    const char *text;     /* the prompt/question; NULL = ""       */
    const char *value;    /* initial Entry contents; NULL = empty */
    bool modal;           /* X11 input grab, like message dialogs */
    fdk_font *font;       /* borrowed; NULL = system default      */
    fdk_window *parent;   /* borrowed; anchors stacking where the
                              backend supports it                 */
} fdk_prompt_dialog_options;

/* `text` is FDK-owned and only valid during the call — copy what you
 * need. Destroying other widgets/windows from here is safe (same
 * reentrancy rules as every dialog callback). */
typedef void (*fdk_prompt_dialog_fn)(fdk_dialog_response response,
                                     const char *text, void *user_data);

fdk_result fdk_dialog_show_prompt(fdk_context *ctx,
                                  const fdk_prompt_dialog_options *options,
                                  fdk_prompt_dialog_fn on_response,
                                  void *user_data,
                                  fdk_window **out_window);

/* ---- File / folder selection dialogs (1.2.0) ----
 *
 * FDK is a self-contained toolkit — there is no portal, no GTK, no
 * external dialog process to defer to. So the file dialog is a real
 * FDK window built from the stock catalog (a path bar, an Up button,
 * a scrolling list of the current directory, Open/Cancel buttons, a
 * status line), owned by the toolkit like a message dialog: it
 * auto-paints, fires one callback, destroys itself. The browsing is
 * done with real directory scans (opendir/readdir), directories
 * listed first alphabetically, hidden entries behind an explicit
 * toggle — the dialog is a small file manager, deliberately, because
 * that is the honest way for a toolkit without a portal to offer
 * file selection.
 *
 * The result model is explicit — never "empty string means maybe
 * cancel": outcome is one of ACCEPTED (paths[] holds count entries),
 * CANCELLED (count 0), ERROR (count 0; the dialog could not even
 * browse a fallback directory). Paths are absolute POSIX paths. For
 * OPEN_FOLDER / OPEN_FOLDERS every returned path IS a directory —
 * verified with stat() at accept time, not assumed from the listing;
 * for OPEN_FILE / OPEN_FILES every path is a regular file. The
 * multiple kinds refuse to silently discard extra selections: the
 * callback receives every selected entry. */

/* What the dialog selects. */
typedef enum fdk_file_dialog_kind {
    FDK_FILE_DIALOG_OPEN_FILE   = 0, /* one existing file             */
    FDK_FILE_DIALOG_OPEN_FILES  = 1, /* one or more existing files    */
    FDK_FILE_DIALOG_OPEN_FOLDER = 2, /* one existing directory        */
    FDK_FILE_DIALOG_OPEN_FOLDERS= 3, /* one or more existing dirs     */
    FDK_FILE_DIALOG_SAVE_FILE   = 4, /* one target path to WRITE      */
} fdk_file_dialog_kind;

/* SAVE_FILE (1.2.3) differs from the OPEN kinds on one axis,
 * honestly: paths[0] is where the application SHOULD write — the
 * file may not exist yet (the usual save-as case), so no existence
 * is promised, only that the PARENT directory existed and was a
 * directory at accept time. When the target existed as a regular
 * file, the user answered an explicit overwrite confirmation first
 * (a nested Yes/No message dialog — declining it returns to the
 * dialog; it does not cancel). */

/* How the interaction concluded. */
typedef enum fdk_file_dialog_outcome {
    FDK_FILE_DIALOG_ERROR     = -2, /* could not browse anything     */
    FDK_FILE_DIALOG_CANCELLED = -1, /* Cancel / Escape / WM close    */
    FDK_FILE_DIALOG_ACCEPTED  =  0, /* Open pressed; paths[] valid   */
} fdk_file_dialog_outcome;

typedef struct fdk_file_dialog_result {
    fdk_file_dialog_outcome outcome;
    char **paths;  /* FDK-allocated; NULL when count == 0           */
    size_t count;  /* valid entries; 0 unless ACCEPTED              */
} fdk_file_dialog_result;

/* Releases a result's paths and the array itself. NULL is legal. */
void fdk_file_dialog_result_free(fdk_file_dialog_result *result);

/* Name filters (1.2.3): a ";"-separated list of glob patterns, e.g.
 * "*.png;*.jpg;*.jpeg". Matching is case-insensitive (the GTK file
 * chooser convention: *.png matches photo.PNG), '*' matches any run
 * of characters, '?' one character, everything else is literal.
 * Directories are never filtered (you can always navigate). The
 * dialog offers the patterns in order plus an "All files" row; the
 * FIRST pattern is initially active. NULL/empty = no filter. */
typedef struct fdk_file_dialog_options {
    const char *title;     /* copied; NULL = kind-appropriate default */
    const char *start_dir; /* copied; NULL = current working dir      */
    fdk_file_dialog_kind kind;
    bool modal;            /* X11 input grab, like message dialogs    */
    bool show_hidden;      /* initial state of the hidden-files
                              toggle (the dialog can flip it)         */
    fdk_window *parent;    /* borrowed; anchors stacking where the
                              backend supports it                     */
    const char *start_name;/* SAVE: initial contents of the name row,
                              copied; NULL = empty. Ignored by the
                              OPEN kinds.                            */
    const char *filters;   /* copied glob list, see above; NULL = all */
    bool hide_recents;     /* 1.4.4: true = no "Recent" place in the
                              sidebar AND no recording of accepted
                              paths into the XDG recently-used file
                              (the shared ~/.local/share/
                              recently-used.xbel every desktop app
                              converges on — see the section below).
                              Zero (the default) shows the place and
                              records acceptances. Ignored by SAVE. */
} fdk_file_dialog_options;

/* Called once, from inside event dispatch, when the dialog closes.
 * `result` is FDK-owned and only valid during the call — copy what
 * you need (fdk_file_dialog_result_free is for results YOU built or
 * cloned; the dialog's own result needs no free). Destroying the
 * parent window from here is safe (the dialog self-destroys
 * afterwards regardless of path). */
typedef void (*fdk_file_dialog_done_fn)(
    const fdk_file_dialog_result *result, void *user_data);

/* Shows the file-selection dialog on `ctx`. Same lifecycle contract
 * as fdk_dialog_show_message: toolkit-owned window, auto-painted,
 * self-destroying; *out_window may be NULL (fire-and-forget) or used
 * to destroy it early (answers CANCELLED and fires the callback).
 *
 * Failure modes: FDK_ERR_INVALID_ARGUMENT (NULL ctx), FDK_ERR_OUT_OF_
 * MEMORY, FDK_ERR_NOT_INITIALIZED, FDK_ERR_WINDOW_CREATE — no dialog
 * exists and no callback fires. An unreadable start_dir is NOT an
 * error: the dialog falls back to $HOME, then /, showing why in its
 * status line.
 */
fdk_result fdk_dialog_open_file(fdk_context *ctx,
                                const fdk_file_dialog_options *options,
                                fdk_file_dialog_done_fn on_done,
                                void *user_data,
                                fdk_window **out_window);

/* ---- Save dialog (1.2.3) ----
 *
 * fdk_dialog_save_file is the dedicated save-as entry point: it is
 * fdk_dialog_open_file with the kind forced to SAVE_FILE (any kind
 * in `options` is ignored). The window it shows is the same browser
 * — places sidebar, path bar, filters — plus a Name row:
 *
 *   - the row starts as options->start_name; activating a listed
 *     file puts ITS name in the row (so "save over that one" is two
 *     clicks); activating a directory descends, as everywhere else.
 *   - Save validates the name honestly: non-empty, no '/', not
 *     "." or "..", at most 255 bytes, and the current directory
 *     must still exist — every failure is a status-line message,
 *     the dialog stays up (never a silent wrong answer).
 *   - an existing REGULAR target gets an overwrite confirmation
 *     (nested Yes/No message dialog); a directory target is refused
 *     ("a folder with that name exists"); anything else (fifo,
 *     socket, device) is refused as not a regular file.
 *   - the accepted path is <current directory>/<name> with the
 *     directory canonicalized (realpath) but the name kept EXACTLY
 *     as typed — no extension guessing, no symlink resolution on
 *     the leaf: what the user typed is what the app gets.
 */
fdk_result fdk_dialog_save_file(fdk_context *ctx,
                                const fdk_file_dialog_options *options,
                                fdk_file_dialog_done_fn on_done,
                                void *user_data,
                                fdk_window **out_window);

/* ---- Recent files (1.4.4) ----------------------------------------
 *
 * The OPEN dialog's sidebar leads with a "Recent" place: the XDG
 * recently-used list ($XDG_DATA_HOME/recently-used.xbel, falling
 * back to $HOME/.local/share/recently-used.xbel — the same file
 * every desktop's file choosers share). Rows are the recent FILES
 * (newest first, the page glyph); activating one accepts it
 * directly when it still exists, descends when it is a directory,
 * and honestly reports + drops the row when it has vanished.
 * Breadcrumbs read "Recently Used"; Ctrl+L still escapes to a
 * typed location, and any navigation (place, Up, Home, crumb)
 * leaves the recents view for the browsed directory.
 *
 * Recording is the other half of the XDG contract: when a dialog
 * ACCEPTS, every accepted path is moved to the front of the xbel
 * with fresh timestamps (the write is a TEXT SPLICE — other apps'
 * metadata survives byte-for-byte — and atomic: tmp + rename).
 * options->hide_recents opts out of both halves. */

/* ---- About dialog (1.4.3) ----
 *
 * The application's identity card, built from the stock catalog:
 * program name, version line, wrap-text comments, copyright, and
 * (optionally) a logo decoded from an image file. The website row
 * is a LINK-styled button — FDK never launches a browser (nothing
 * in the toolkit ever forks or execs); activating it fires
 * on_website with the URL so the application decides what "open"
 * means. The license text, when given, renders in a scrolling wrap
 * label capped at ~9 lines of dialog height.
 *
 * Same lifecycle as every dialog: toolkit-owned window, auto-
 * painted, self-destroying. The response callback is OPTIONAL
 * (NULL = fire-and-forget; the close button's answer is always
 * CLOSE). */

typedef struct fdk_about_dialog_options {
    const char *program_name;  /* copied; NULL = "About"          */
    const char *version;       /* copied; NULL = omitted line     */
    const char *comments;      /* copied; NULL = omitted block    */
    const char *copyright;     /* copied; NULL = omitted line     */
    const char *website;       /* copied; NULL = no website row   */
    const char *license;       /* copied; NULL = omitted block    */
    const char *logo_path;     /* copied; an image file (PNG/JPEG/
                                  BMP/...) the dialog decodes; NULL
                                  = no logo. Decoding failure logs
                                  and omits the logo (the dialog
                                  still works).                   */
    bool modal;                /* X11 input grab, like messages   */
    fdk_window *parent;        /* borrowed; anchors stacking where
                                  the backend supports it          */
} fdk_about_dialog_options;

/* Fired when the website link button is activated. `url` is the
 * options' website string, valid during the call. May be NULL (the
 * button is display-only then). */
typedef void (*fdk_about_website_fn)(const char *url, void *user_data);

fdk_result fdk_dialog_show_about(fdk_context *ctx,
                                 const fdk_about_dialog_options *options,
                                 fdk_dialog_response_fn on_response,
                                 fdk_about_website_fn on_website,
                                 void *user_data,
                                 fdk_window **out_window);

/* ---- Font chooser dialog (1.4.3) ----
 *
 * The font-scan surface as a picker: a scrolling family/style list
 * (one row per loadable system face, from fdk_font_enumerate), a
 * size spinner (6..96), and a live preview label rendering the
 * sample text in the selected face at the selected size (the
 * preview reloads the face by path+index on every change). OK
 * hands the choice; Cancel/Escape/dismissal hands the negative.
 *
 * The result contract is the file dialog's explicitness: outcome
 * ACCEPTED means the fields are valid; CANCELLED means they are
 * untouched/NULL. The returned strings are FDK-owned and valid only
 * during the callback — copy what you need. The application turns
 * the answer into a font with fdk_font_load_face(result->path,
 * result->face_index, result->size). */

typedef enum fdk_font_dialog_outcome {
    FDK_FONT_DIALOG_CANCELLED = -1,
    FDK_FONT_DIALOG_ACCEPTED  = 0,
} fdk_font_dialog_outcome;

typedef struct fdk_font_dialog_result {
    fdk_font_dialog_outcome outcome;
    char *family;    /* FDK-owned, valid during the callback    */
    char *style;     /* ditto                                   */
    char *path;      /* ditto                                   */
    fdk_i32 face_index;
    fdk_i32 size;    /* pixel size (6..96)                      */
} fdk_font_dialog_result;

typedef struct fdk_font_dialog_options {
    const char *title;      /* copied; NULL = "Select Font"       */
    const char *sample;     /* copied preview text; NULL = the
                               quick-brown-fox default            */
    const char *initial_family; /* copied; selected when it matches
                                   an enumerated face (style
                                   matches first if given)        */
    const char *initial_style;
    fdk_i32 initial_size;   /* clamped 6..96; 0 = 12              */
    bool modal;             /* X11 input grab                      */
    fdk_window *parent;     /* borrowed; anchors stacking         */
} fdk_font_dialog_options;

typedef void (*fdk_font_dialog_fn)(const fdk_font_dialog_result *result,
                                   void *user_data);

fdk_result fdk_dialog_choose_font(fdk_context *ctx,
                                  const fdk_font_dialog_options *options,
                                  fdk_font_dialog_fn on_done,
                                  void *user_data,
                                  fdk_window **out_window);

/* ---- Color chooser dialog (1.4.3) ----
 *
 * The HSV wheel on the canvas: a hue ring with an inscribed
 * saturation/value triangle (the classic Qt geometry), rasterized
 * per-pixel in software — no approximation by primitives. Dragging
 * the ring sets hue; dragging inside the triangle sets S/V; the
 * marker dots track both. A hex entry accepts #rrggbb (Enter
 * applies), the swatch stack shows current and initial, and
 * OK/Cancel close. Everything recomputes live while dragging.
 *
 * The result is explicit like every dialog: ACCEPTED carries the
 * chosen fdk_color; CANCELLED carries the initial color unchanged
 * (fields are always filled — a color dialog's "no answer" is still
 * a color, and the outcome field says which). */

typedef enum fdk_color_dialog_outcome {
    FDK_COLOR_DIALOG_CANCELLED = -1,
    FDK_COLOR_DIALOG_ACCEPTED  = 0,
} fdk_color_dialog_outcome;

typedef struct fdk_color_dialog_result {
    fdk_color_dialog_outcome outcome;
    fdk_color color;    /* ACCEPTED: the choice; CANCELLED: initial */
} fdk_color_dialog_result;

typedef struct fdk_color_dialog_options {
    const char *title;   /* copied; NULL = "Select Color"           */
    fdk_color initial;   /* the starting color (a == 0 also black)  */
    bool show_hex;       /* include the #rrggbb entry row           */
    bool modal;          /* X11 input grab                          */
    fdk_window *parent;  /* borrowed; anchors stacking             */
} fdk_color_dialog_options;

typedef void (*fdk_color_dialog_fn)(
    const fdk_color_dialog_result *result, void *user_data);

fdk_result fdk_dialog_choose_color(fdk_context *ctx,
                                   const fdk_color_dialog_options *options,
                                   fdk_color_dialog_fn on_done,
                                   void *user_data,
                                   fdk_window **out_window);

/* ---- Color button (1.4.4) ----
 *
 * The color-well swatch button (GTK's GtkColorButton / Qt's
 * QColorButton): a small button whose face is the current color,
 * with an alpha checkerboard underneath translucent picks. Pressing
 * it opens the 1.4.3 color chooser dialog (modal where the backend
 * can grab) seeded with the current color; ACCEPTED writes the new
 * color into the button and fires on_color_set — CANCELLED changes
 * nothing. The chooser is anchored to the button's window where the
 * backend supports it.
 *
 * The button needs a live window to open a dialog from: in detached
 * trees a press is a documented no-op (no context to show a window
 * on). The button never launches anything itself — the website-link
 * rule of the About dialog applies: FDK execs nothing.
 *
 * The swatch is honest about alpha: colors with a < 1 paint over
 * the classic two-tone checkerboard; a == 0 shows the board alone
 * ("transparent"). get/set_color move plain fdk_color values. */

typedef void (*fdk_color_button_fn)(fdk_widget *button, fdk_color color,
                                    void *user_data);

/* Creates the button showing `color` (copied into the button; the
 * caller's struct is not retained). */
fdk_result fdk_color_button_create(fdk_widget *parent, fdk_color color,
                                  fdk_widget **out_button);
/* The current color. */
fdk_color fdk_color_button_get_color(fdk_widget *button);
/* Sets the color programmatically (repaints; does NOT fire
 * on_color_set — that callback belongs to user gestures, exactly
 * like fdk_button's checked setter stays silent). */
fdk_result fdk_color_button_set_color(fdk_widget *button, fdk_color color);
/* The chooser's title (copied; NULL restores "Select Color"). */
fdk_result fdk_color_button_set_title(fdk_widget *button, const char *title);
/* Fires after a chooser ACCEPT settles the new color. */
void fdk_color_button_set_on_color_set(fdk_widget *button,
                                       fdk_color_button_fn fn,
                                       void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* FDK_DIALOG_H */
