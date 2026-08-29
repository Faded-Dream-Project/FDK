/*
 * fdk_dnd.h — drag and drop (1.2.0)
 *
 * Two halves, both text and files, both backends:
 *
 *   RECEIVING: a window registers the formats it can accept with
 *   fdk_window_set_drop_formats(). FDK then negotiates with the
 *   dragging client on its own — XDND on X11 (Enter/Position/
 *   Status/Leave/Drop/Finished, version min(peer, 5)), wl_data_device
 *   drag offers on Wayland — and delivers FDK_EVENT_DRAG_ENTER /
 *   MOTION / LEAVE to the window's event callback while the pointer
 *   is over it, and FDK_EVENT_DRAG_DROP with the transferred bytes
 *   once the user releases. The application never touches the
 *   protocol: position is window-local, formats are a mask, data is
 *   decoded (file:// URIs become POSIX paths, percent-decoding
 *   applied; plain-text drops arrive as UTF-8).
 *
 *   SENDING: fdk_drag_begin() starts a drag from an FDK window —
 *   from inside a pointer-press/motion handler, the way every
 *   toolkit does it (the call must be able to cite the press's
 *   input serial; Wayland compositors reject drags started from a
 *   cold serial). The drag then runs to completion on its own: FDK
 *   grabs the pointer (X11) or hands the drag to the compositor
 *   (Wayland start_drag, no icon surface), tracks the target window
 *   under the pointer, and reports the outcome exactly once through
 *   the on_done callback (SUCCEEDED = a target accepted the drop;
 *   CANCELLED = released over nothing that accepts, or the source
 *   action was cancelled; FAILED = the backend could not start).
 *
 * Scope notes (documented rather than faked):
 *   - Formats are TEXT and URI_LIST; one drag may offer both.
 *   - Drop targets are WINDOW-level in v1: the callback hit-tests
 *     its own widgets (the capabilities example's drop panel does
 *     exactly this). Widget-level targets are future work.
 *   - Actions are COPY only — no move/link negotiation (XDND
 *     advertises XdndActionCopy; Wayland set_actions(copy)).
 *   - No intra-application drag-local "direct save" or fancy MIME
 *     types (images, rich text): if a peer offers only formats FDK
 *     does not speak, the drag simply is not accepted.
 */

#ifndef FDK_DND_H
#define FDK_DND_H

#include "fdk_core.h"
#include "fdk_types.h"
#include "fdk_window.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a drag carries. Bitmask; a source may offer several, a target
 * registers the ones it can read. */
typedef enum fdk_drag_format {
    FDK_DRAG_FORMAT_TEXT     = 1 << 0, /* UTF-8 text                */
    FDK_DRAG_FORMAT_URI_LIST = 1 << 1, /* files/folders (URI list)  */
} fdk_drag_format;

/* ---- Receiving ---- */

/* Registers which drop formats this window accepts (0 = accept
 * nothing; this is also the default). Must be called before a drag
 * can be dropped here; can be changed at any time (a drag in flight
 * re-evaluates on its next position event — X11 — or is simply not
 * accepted at release time — Wayland, where the compositor asks once
 * at enter via set_actions).
 *
 * On X11 this also decides the XDND accept/reject status FDK replies
 * with while the drag hovers — the dragging client's cursor feedback
 * follows it. On Wayland the same information drives the drag
 * offer's accepted action.
 *
 * Can fail with FDK_ERR_INVALID_ARGUMENT (NULL window) or
 * FDK_ERR_NOT_INITIALIZED (window's backend connection is gone). */
fdk_result fdk_window_set_drop_formats(fdk_window *window, int formats);

/* The registered format mask (0 for NULL). */
int fdk_window_get_drop_formats(const fdk_window *window);

/* ---- Sending ---- */

/* How an fdk_drag_begin-ended drag concluded. */
typedef enum fdk_drag_status {
    FDK_DRAG_FAILED    = -1, /* backend could not start the drag   */
    FDK_DRAG_CANCELLED = 0,  /* no accepting target / user aborted */
    FDK_DRAG_SUCCEEDED = 1,  /* a target accepted the drop         */
} fdk_drag_status;

/* Fired exactly once when a drag started by fdk_drag_begin ends. */
typedef void (*fdk_drag_done_fn)(fdk_drag_status status, void *user_data);

/* Starts a drag from `origin` offering `formats`.
 *
 *   formats & FDK_DRAG_FORMAT_TEXT     -> `text` is copied (may be
 *                                         NULL for an empty text)
 *   formats & FDK_DRAG_FORMAT_URI_LIST -> the `uri_count` entries of
 *                                         `uris` are copied; entries
 *                                         are POSIX paths OR URIs —
 *                                         anything without a scheme
 *                                         is treated as a path and
 *                                         file://-encoded on the
 *                                         wire.
 *
 * Call this from a pointer-press (or press-then-motion) handler on
 * `origin`'s callback; the backend cites the press's serial. Calling
 * it outside an input grab context still works on X11 (the grab is
 * taken here) but may be refused by Wayland compositors (stale
 * serial) — FDK_DRAG_FAILED, honestly reported.
 *
 * The drag runs inside FDK's ordinary event dispatch: pointer events
 * while the drag is active are consumed by it (no nested loop, no
 * blocking — docs/threading.md). Exactly one of on_done's statuses
 * arrives later, from inside fdk_pump_events/fdk_run.
 *
 * Can fail with FDK_ERR_INVALID_ARGUMENT (NULL origin, or formats
 * naming no bits, or URI_LIST with zero URIs), FDK_ERR_UNSUPPORTED
 * (backend without DnD — e.g. Wayland with no data_device_manager),
 * FDK_ERR_PLATFORM (drag could not start: X11 pointer grab lost to
 * another client, serial rejected), FDK_ERR_OUT_OF_MEMORY. */
fdk_result fdk_drag_begin(fdk_window *origin, int formats,
                          const char *text,
                          const char *const *uris, size_t uri_count,
                          fdk_drag_done_fn on_done, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* FDK_DND_H */
