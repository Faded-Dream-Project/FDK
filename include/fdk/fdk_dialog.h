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

#ifdef __cplusplus
}
#endif

#endif /* FDK_DIALOG_H */
