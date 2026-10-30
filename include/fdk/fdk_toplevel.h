/*
 * fdk_toplevel.h — the foreign toplevel list (1.1.0, D-4)
 *
 * The taskbar/dock substrate: the OTHER applications' top-level
 * windows, enumerated and live, with the two actions a panel takes.
 * Wayland speaks wlr-foreign-toplevel-management (sway, labwc,
 * wlroots compositors generally); X11 speaks EWMH (_NET_CLIENT_LIST
 * + the client-message requests). One backend-neutral API.
 *
 * The model is deliberately COARSE (the tray-host pattern, D-014):
 * ONE changed callback — something about the set changed: a window
 * opened, closed, retitled, changed state, focus moved — and the
 * consumer re-snapshots (count + get_info). No per-window event
 * taxonomy, no ordering promises: a taskbar re-renders from the
 * snapshot either way. The indices passed to the read/action calls
 * are valid until the NEXT change callback fires.
 *
 * Degradation is the contract everywhere in FDK's desktop half: a
 * compositor without the protocol (GNOME, KDE before 5.x) or an X11
 * server without EWMH answering gives an EMPTY list (or
 * FDK_ERR_UNSUPPORTED from open) — never an error loop, never a
 * crash. X11's honest limit: title edits on OTHER clients' windows
 * are not notified (clients own their PropertyNotify selection);
 * the list re-snapshots on membership and focus changes — a
 * retitle shows up at the next membership/focus churn (D-031
 * records it; Wayland has no such gap, the protocol pushes titles).
 */

#ifndef FDK_TOPLEVEL_H
#define FDK_TOPLEVEL_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* State bits (fdk_toplevel_info.states). Backends map their own
 * vocabularies onto these; the numbering is stable ABI. */
#define FDK_TOPLEVEL_MAXIMIZED  0x1u
#define FDK_TOPLEVEL_MINIMIZED  0x2u
#define FDK_TOPLEVEL_FULLSCREEN 0x4u
#define FDK_TOPLEVEL_ACTIVATED  0x8u /* the compositor's focus     */

/* One toplevel, snapshot-copied. `struct_size` carries the usual
 * append-only contract: zero it, set struct_size, pass it in; FDK
 * copies exactly the fields it knows. `id` is stable for the
 * toplevel's lifetime (usable to track identity across snapshots);
 * `title`/`app_id` are NUL-terminated copies (empty string when the
 * platform reports none — X11 has no app_id, WM_CLASS's res_class
 * fills it). */
typedef struct fdk_toplevel_info {
    size_t struct_size;
    fdk_u64 id;
    fdk_u32 states;
    char title[160];
    char app_id[160];
} fdk_toplevel_info;

typedef struct fdk_toplevel_list fdk_toplevel_list;

/* Something about the set changed — re-snapshot. Fires from inside
 * FDK's dispatch/pump paths (the threading rule: UI thread only,
 * like every callback here). May fire several times for one user
 * action (open + focus, for instance); the snapshot is idempotent. */
typedef void (*fdk_toplevel_changed_fn)(fdk_toplevel_list *list,
                                        void *user);

/* Opens the foreign toplevel list. NULL when the context has no
 * connection or the backend lacks the mechanism — the documented
 * degradation, callers treat NULL as "no taskbar substrate here".
 * The callback may fire during open itself (the initial set lands
 * as one change). */
fdk_toplevel_list *fdk_toplevel_list_open(fdk_context *ctx,
                                          fdk_toplevel_changed_fn on_change,
                                          void *user);

/* Closes the list (NULL is a no-op). Safe inside the changed
 * callback (the teardown defers, the connection-cleanliness rule). */
void fdk_toplevel_list_close(fdk_toplevel_list *list);

/* The current count (0 for a NULL or protocol-less list). */
fdk_u32 fdk_toplevel_count(fdk_toplevel_list *list);

/* Snapshot-copies toplevel `index` into `out` (0-based; index >=
 * count is FDK_ERR_INVALID_ARGUMENT). FDK_ERR_INVALID_ARGUMENT for
 * NULL/short structs. */
fdk_result fdk_toplevel_get_info(fdk_toplevel_list *list, fdk_u32 index,
                                 fdk_toplevel_info *out);

/* The two panel actions. ACTIVATE asks the compositor/WM to focus
 * the toplevel (the focus-stealing rules of the platform apply —
 * S-6's activation documentation covers the semantics); CLOSE asks
 * it to close the window politely (the app gets its close request;
 * it may still confirm-cancel). Indices follow get_info's validity
 * rule. FDK_ERR_UNSUPPORTED where the platform cannot; INVALID_
 * ARGUMENT for out-of-range indices. */
fdk_result fdk_toplevel_activate(fdk_toplevel_list *list, fdk_u32 index);
fdk_result fdk_toplevel_close(fdk_toplevel_list *list, fdk_u32 index);

#ifdef __cplusplus
}
#endif

#endif /* FDK_TOPLEVEL_H */
