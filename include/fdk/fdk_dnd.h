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
 *   DRAG ICON (1.1.0, S-9): fdk_drag_begin_ex attaches an image to
 *   the drag itself — the picture under the cursor while it flies
 *   (a file's thumbnail, a text snippet's preview). Wayland: the
 *   protocol's own icon surface in wl_data_device.start_drag. X11,
 *   where XDND has no icon protocol: a small override-redirect
 *   window that tracks the pointer for the drag's life (the
 *   classic GTK approach), shaped per the icon's alpha so
 *   transparent icons are not black boxes.
 *
 *   WIDGET-LEVEL DROP TARGETS (1.1.0, S-9): fdk_widget_set_drop_target
 *   registers a widget (or a container standing in for its subtree)
 *   as the thing a drop lands on, and fdk_widget_set_drag_callback
 *   receives the drag events scoped to it — ENTER when the drag
 *   enters its bounds, MOTION inside, LEAVE when it exits, DROP on
 *   release. Acceptance becomes PER-POSITION: over a registered
 *   target the offered formats intersect THAT widget's mask; over
 *   the rest of the window the window's own mask applies (the v1
 *   behavior, unchanged). The window-level callback keeps seeing
 *   every drag event regardless — widget routing is additive, not a
 *   replacement.
 *
 * Scope notes (documented rather than faked):
 *   - Formats are TEXT and URI_LIST; one drag may offer both.
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
#include "fdk_event.h" /* the drag event payloads the widget routing delivers */

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

/* ---- Widget-level drop targets (1.1.0, S-9 / B-13) ----------------------
 *
 * Registration: a widget with a nonzero mask is a drop target. The
 * window's drag routing hit-tests the tree at the drag's position
 * and walks UP from the deepest hit widget — the first target on
 * that chain owns the position (a registered CONTAINER receives
 * drops anywhere in its subtree, exactly as an app would expect a
 * drop zone to behave). Over a position with no target on the
 * chain, the window's own mask governs — the pre-1.1.0 behavior
 * where nothing registered.
 *
 * Acceptance is therefore per-position on BOTH backends: the XDND
 * Status reply and the wl_data_offer accept are re-evaluated as the
 * pointer moves, so the source's cursor feedback reflects the real
 * answer under it (GTK's behavior). The negotiation never accepts
 * MORE than the window did before — a widget target can only
 * refine, not widen, what the position accepts (the window's mask
 * still applies where no widget target exists; a drag whose
 * formats match NO target at the drop position is refused at the
 * protocol level, no event delivered). */

/* Marks `widget` as a drop target accepting `formats` (0 clears the
 * registration — the default). Fails with FDK_ERR_INVALID_ARGUMENT
 * on NULL widget or unknown format bits. Changing the mask while a
 * drag hovers is legal: the next motion re-evaluates. */
fdk_result fdk_widget_set_drop_target(fdk_widget *widget, int formats);

/* The registered mask (0 for NULL or unregistered). */
int fdk_widget_get_drop_target(const fdk_widget *widget);

/* Scoped drag delivery: fired for FDK_EVENT_DRAG_ENTER (the drag
 * crossed into this target's scope), FDK_EVENT_DRAG_MOTION (inside
 * it), FDK_EVENT_DRAG_LEAVE (crossed out — including the drag's
 * end), and FDK_EVENT_DRAG_DROP (released inside it). The event
 * pointer is the SAME payload the window callback receives (same
 * lifetime rules: data valid for the callback only). Setting NULL
 * clears the callback. A widget may be a target without a callback
 * (it still participates in acceptance — useful for containers
 * that only gate the drop); and a callback without a registration
 * never fires. */
typedef void (*fdk_widget_drag_fn)(fdk_widget *widget, fdk_event_type type,
                                   const fdk_drag_event *event, void *user);

/* Sets the scoped drag callback (and user data) on `widget`. The
 * widget must ALSO be registered via fdk_widget_set_drop_target for
 * the callback to fire. */
fdk_result fdk_widget_set_drag_callback(fdk_widget *widget,
                                        fdk_widget_drag_fn on_drag,
                                        void *user);

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

/* fdk_drag_begin with a DRAG ICON (1.1.0, S-9): `icon` is width x
 * height ARGB8888 pixels (fdk_u32 packing A<<24|R<<16|G<<8|B — the
 * same layout fdk_window_set_icon and _NET_WM_ICON carry), row-major,
 * no padding, COPIED at call time. NULL icon or non-positive dims =
 * no icon (identical to fdk_drag_begin). Dimensions are clamped to
 * 256 — an icon is drag feedback, not a second framebuffer; larger
 * images scale down (nearest-neighbor, alpha-preserving) by FDK
 * itself.
 *
 * Per backend: Wayland commits the icon as the protocol's own drag
 * surface (compositor-positioned, alpha-blended); X11 shows it in a
 * small override-redirect window tracking the pointer, offset up-
 * right of the cursor (the GTK convention), shaped by the alpha
 * channel (>= 0x80 = opaque pixels own the shape) so rounded and
 * transparent icons are not black boxes. In both cases the icon
 * lives exactly as long as the drag. */
fdk_result fdk_drag_begin_ex(fdk_window *origin, int formats,
                             const char *text,
                             const char *const *uris, size_t uri_count,
                             const fdk_u32 *icon, fdk_i32 icon_w,
                             fdk_i32 icon_h,
                             fdk_drag_done_fn on_done, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* FDK_DND_H */
