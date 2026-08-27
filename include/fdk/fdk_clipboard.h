/*
 * fdk_clipboard.h — clipboard text exchange (Phase 9)
 *
 * One clipboard per fdk_context, shared by every window on it. The
 * API is deliberately tiny and text-only for v1: set the clipboard's
 * text, read it back. Ownership semantics follow the platform's
 * native model:
 *
 *   X11 (ICCCM): setting the text makes FDK's connection the owner
 *   of the CLIPBOARD selection. FDK keeps its copy and SERVES it to
 *   other clients on request (SelectionRequest) for as long as it
 *   owns the selection; when another client takes ownership
 *   (SelectionClear), FDK drops its copy. Reading the clipboard is
 *   synchronous from the caller's perspective — the backend performs
 *   the ICCCM convert-and-notify dance on a hidden helper window with
 *   a bounded internal wait (250 ms), so a hung clipboard owner can
 *   never hang the application.
 *
 *   Wayland: setting the text creates a wl_data_source and offers it
 *   via wl_data_device.set_selection, using the serial of the most
 *   recent input event (the protocol requires a valid input serial;
 *   before any input has arrived, compositors are permitted to ignore
 *   the request — an honest, documented limitation). Reading returns
 *   FDK's own text when FDK owns the selection (compositors do not
 *   hand a client its own selection back), and otherwise receives the
 *   current wl_data_offer's UTF-8 text over the offer's pipe.
 *
 * Scope notes (documented rather than faked):
 *   - UTF-8 text only. No image/URI formats in v1.
 *   - X11: the PRIMARY selection and INCR (incremental, >~1 MiB)
 *     transfers are not supported; oversized reads fail with a
 *     warning instead of hanging.
 *   - Wayland: no wl_data_source "ask" actions, no drag-and-drop.
 *   - The clipboard is not versioned; "set" replaces wholesale.
 */

#ifndef FDK_CLIPBOARD_H
#define FDK_CLIPBOARD_H

#include "fdk_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Makes `text` the context's clipboard content. The string is copied
 * (the caller keeps ownership of its buffer), embedded NUL bytes end
 * the text (clipboard text is NUL-terminated by contract), and an
 * empty string is a legal clipboard. Setting NULL is treated as "".
 *
 * The call takes effect immediately on X11 (selection ownership is
 * synchronous); on Wayland it becomes visible to other clients when
 * the compositor processes the request.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT  - ctx is NULL
 *   FDK_ERR_NOT_INITIALIZED   - ctx has no platform connection
 *   FDK_ERR_UNSUPPORTED       - this backend has no clipboard
 *                               (headless / Wayland without
 *                               wl_data_device_manager)
 *   FDK_ERR_PLATFORM          - the platform refused (X11: selection
 *                               ownership did not take effect)
 *   FDK_ERR_OUT_OF_MEMORY
 */
fdk_result fdk_clipboard_set_text(fdk_context *ctx, const char *text);

/* Reads the current clipboard text into a freshly allocated,
 * NUL-terminated UTF-8 string. Returns NULL when the clipboard is
 * empty, unreadable (owner refused / timed out / oversized INCR
 * transfer), or the backend has no clipboard support — a warning is
 * logged in those cases, never an error code the caller must branch
 * on. The caller frees the string with fdk_free() (free() is also
 * correct on every supported platform).
 *
 * On X11 this performs the bounded synchronous convert described in
 * the file header. Events that arrive while FDK waits for the owning
 * client's SelectionNotify stay queued — FDK never dispatches them
 * re-entrantly from inside this call. */
char *fdk_clipboard_get_text(fdk_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FDK_CLIPBOARD_H */
