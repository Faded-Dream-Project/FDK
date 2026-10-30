/*
 * fdk_session.h — session participation: save, restore (1.1.0, D-5)
 *
 * The B-19 session half: the protocols by which a desktop remembers
 * an application's windows and restarts it in the same state. Two
 * lanes, two backends, one honest surface — the same degradation
 * contract as every desktop feature here (the taskbar-absent
 * branch, never an error loop).
 *
 * X11 — the ICCCM session conventions, AUTOMATIC on every FDK
 * window, no session object needed:
 *
 *   - WM_COMMAND (fdk_window_set_command, fdk_window.h): the
 *     restart command, argv form — the command a session manager
 *     re-executes at the next login to bring the application back.
 *     Set it once on the application's main window (ICCCM: exactly
 *     one window of a client carries a nonzero WM_COMMAND; FDK
 *     never enforces this, the application knows which window is
 *     "the" window).
 *   - WM_SAVE_YOURSELF: every FDK window lists the atom in
 *     WM_PROTOCOLS. When the session manager (through the WM)
 *     asks the client to save, the window's event callback
 *     receives FDK_EVENT_SESSION_SAVE — save state now
 *     (fdk_prefs_save() and whatever else), and update
 *     fdk_window_set_command() if the restart args changed. FDK
 *     writes the WM_COMMAND ack itself AFTER the callback returns
 *     — the PropertyNotify the session manager waits for (ICCCM
 *     appendix C). Per the same appendix: save without interacting
 *     with the user, and after the event change nothing until the
 *     next input event arrives — the danger may not be over.
 *   - What is deliberately NOT here: full XSMP (SM_CLIENT_ID, the
 *     ICE wire, WM_CLIENT_LEADER). The obsolete lane is what the
 *     light sessions FDK targets actually speak; XSMP is fenced as
 *     a future family (docs/DECISIONS.md D-032).
 *
 * Wayland — xdg-session-management-v1 (staging, wayland-protocols
 * 1.48+): the compositor remembers toplevel state keyed by a
 * session id string. fdk_session_open() binds the manager and
 * mints or restores a session; fdk_window_join_session() attaches
 * a window (the protocol's restore path when the window has not
 * been shown yet, the fresh-add path after — FDK picks
 * automatically). PERSIST the id from the first notify callback
 * (fdk_prefs_set + save) and pass it back on the next launch's
 * open(RESTORE): that round trip is the whole restore loop.
 *
 * fdk_session_open() returns NULL when the backend offers no
 * session mechanism at all — X11 always (its session lane is the
 * object-less WM_SAVE_YOURSELF half above), Wayland when the
 * compositor does not advertise the protocol (sway 1.10/labwc at
 * the time of writing; early adopters only). NULL is the
 * documented degradation: applications treat it as "no session
 * restore on this desktop" and keep running.
 */

#ifndef FDK_SESSION_H
#define FDK_SESSION_H

#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why the session is being opened — the protocol's own reason
 * enum, wire values 1..3 (the compositor may restore more state
 * for a recovered app than a launched one, e.g. workspace and
 * stacking order). LAUNCH for a fresh instance from a launcher;
 * RECOVER when re-spawning after a crash; RESTORE when a desktop
 * session manager restarted the application as part of a restored
 * session. X11 ignores it (no object). */
typedef enum {
    FDK_SESSION_REASON_LAUNCH  = 1,
    FDK_SESSION_REASON_RECOVER = 2,
    FDK_SESSION_REASON_RESTORE = 3
} fdk_session_reason;

/* Where the session stands. PENDING from open() until the
 * compositor answers (one pump later, typically); CREATED means a
 * NEW session was minted — persist fdk_session_id(); RESTORED
 * means prior state was (at least partially) brought back; the
 * same id remains valid for the next round. REPLACED means
 * another client took the session over — this object is inert,
 * destroy it and (if the application still wants tracking) open a
 * fresh one. */
typedef enum {
    FDK_SESSION_STATE_PENDING  = 0,
    FDK_SESSION_STATE_CREATED  = 1,
    FDK_SESSION_STATE_RESTORED = 2,
    FDK_SESSION_STATE_REPLACED = 3
} fdk_session_state;

typedef struct fdk_session fdk_session;

/* The coarse notify — the session moved states. Fires from inside
 * FDK's dispatch/pump paths (the UI-thread-only rule, like every
 * callback here): CREATED, RESTORED or REPLACED arrived. Query
 * fdk_session_get_state()/fdk_session_id() inside it. May fire during
 * the pumps that follow fdk_session_open(). NULL is allowed (the
 * application that only ever LAUNCHes and persists the id by
 * polling state after the first pump). */
typedef void (*fdk_session_notify_fn)(fdk_session *session, void *user);

/* Opens a session. `restore_id` is the id a previous run persisted
 * (prefs) — NULL mints a fresh session. Returns NULL when the
 * backend has no session mechanism (X11; Wayland without the
 * protocol) — the documented degradation, not an error. The
 * notify callback may fire from any later fdk_pump_events()/
 * fdk_run() call, never from open() itself. */
fdk_session *fdk_session_open(fdk_context *ctx,
                              fdk_session_reason reason,
                              const char *restore_id,
                              fdk_session_notify_fn on_notify,
                              void *user);

/* Destroys the session object, PRESERVING whatever the compositor
 * stored (the normal end-of-run call — the state is what the next
 * open(RESTORE) brings back). NULL is a no-op. Safe inside the
 * notify callback. */
void fdk_session_destroy(fdk_session *session);

/* Destroys the session AND asks the compositor to forget it — the
 * "do not restore me next time" call (an application's own
 * 'discard session' menu action). NULL is a no-op. */
void fdk_session_remove(fdk_session *session);

/* The session id — the string to persist and hand back to
 * fdk_session_open() on the next launch. NULL until the compositor
 * answers CREATED (or, on a restored session, the id passed in —
 * stable across runs by design). Owned by FDK; valid until the
 * next notify fires or the session is destroyed. */
const char *fdk_session_id(const fdk_session *session);

/* The session's current state (PENDING until the compositor
 * answers). */
fdk_session_state fdk_session_get_state(const fdk_session *session);

/* ---- Attaching windows (the toplevel half) ---------------------------
 *
 * A session only tracks windows explicitly joined to it. The name
 * is the STABLE identity the compositor stores window state under
 * — pick one per window that means the same thing on every run
 * ("main", "editor-1", ...); the pair (app_id, name) is what a
 * future open(RESTORE) re-associates. FDK does not choose names
 * for the application: a wrong-but-stable name still restores;
 * an unstable one never will.
 *
 * JOIN BEFORE fdk_window_show() for the RESTORE path — the
 * protocol requires the restore request before the window's first
 * commit, and FDK issues it automatically while the window is
 * unshown. A window joined after show() is still tracked (the
 * fresh-add path: the compositor records its state from now on)
 * but cannot restore its own prior state this run — one INFO log
 * line says so, the call succeeds. Popups, subsurfaces and
 * extension surfaces have no xdg_toplevel: joining them answers
 * FDK_ERR_INVALID_ARGUMENT. Joining the same window twice answers
 * FDK_ERR_INVALID_STATE.
 *
 * When a restored window's state comes back, its event callback
 * receives FDK_EVENT_SESSION_RESTORED immediately before the first
 * FDK_EVENT_CONFIGURE — the "the compositor remembered this
 * window" signal (an application may skip its own first-show
 * centering, for instance). Windows joined after show, or added
 * fresh, never receive it.
 *
 * X11: join/leave answer FDK_ERR_UNSUPPORTED — the X11 session
 * lane is WM_COMMAND + WM_SAVE_YOURSELF (see above), which need
 * no per-window attachment. */

/* Attaches `window` to `session` under `name`. NULL/empty `name`
 * is FDK_ERR_INVALID_ARGUMENT; NULL window/session likewise. */
fdk_result fdk_window_join_session(fdk_window *window,
                                   fdk_session *session,
                                   const char *name);

/* Detaches the window from its session — the compositor forgets
 * this window's stored state (the others keep theirs). No-op when
 * the window is not joined. Safe before or after show. */
void fdk_window_leave_session(fdk_window *window);

#ifdef __cplusplus
}
#endif

#endif /* FDK_SESSION_H */
