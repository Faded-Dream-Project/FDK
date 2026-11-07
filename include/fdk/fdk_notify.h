/*
 * fdk_notify.h — Faded Dream ToolKit desktop notifications client (1.1.0)
 *
 * The APP side of the freedesktop notification protocol
 * (org.freedesktop.Notifications): send a notification to whatever
 * daemon the desktop runs (notifyd, dunst, GNOME Shell, ...), get
 * told when it is closed or when the user clicks one of its
 * actions, and withdraw it early — the four verbs every desktop
 * app needs, on FDK's own native D-Bus client (fdk_dbus.h), with
 * no new link-time or run-time dependency of any kind.
 *
 * WHAT IT SPEAKS. Notify (with the full request surface: app name,
 * replaces_id, icon — theme name, file:// path, or raw image bytes
 * — summary, body markup, action pairs, urgency and timeout, the
 * category / desktop-entry / image-data hints), CloseNotification,
 * GetCapabilities, GetServerInformation, and the two events the
 * daemon emits, NotificationClosed and ActionInvoked, matched to
 * the daemon's unique name so a second daemon can't cross-talk.
 *
 * THE LIFECYCLE. fdk_notify_open() owns its OWN session-bus
 * connection (a desktop app's notification traffic and its tray
 * presence each keep a socket; the bus is cheap, and one
 * connection per service object keeps the method-serving surface
 * unambiguous). Everything rides the FDK event loop: connect,
 * daemon discovery, every reply, every event — all from inside
 * fdk_pump_events()/fdk_run(). Nothing blocks, nothing spins.
 *
 * THE GRACEFUL POSTURE. A missing session bus, a dead daemon, a
 * daemon that appears late: all are ordinary states REPORTED
 * THROUGH THE API — fdk_notify_get_status() plus the on_failed /
 * on_ready callbacks — never a crash, never a busy-loop, never a
 * retry storm. A bus that is absent at open is retried on the
 * next send (throttled to one attempt per 30 s); a daemon that
 * appears after the client started is picked up through the name
 * watch. Sends queued before the daemon is ready flush in order
 * once it answers GetCapabilities.
 *
 * THE EVENT MODEL. Every event about one notification carries the
 * void *note_user that send took, so no bookkeeping is forced on
 * the app. The daemon-assigned id (the replace key) is reported
 * once in on_sent; closed and action events follow. The mapping
 * is bounded (256 live notifications per client); beyond that the
 * OLDEST mapping is dropped and its later events arrive with a
 * NULL note_user — the honest limit, stated here rather than
 * discovered.
 *
 * BODY MARKUP. The daemon is expected to render the limited
 * markup the specification allows (<b> <i> <u> <a href="...">);
 * anything an FDK app sends passes through unfiltered — the
 * daemon's sanitizer is the daemon's business (FDK's own notifyd
 * documents exactly what it keeps). The summary is plain text.
 *
 * Documentation: docs/EXTENDING.md (the notification chapter),
 * docs/DECISIONS.md D-008. The wire reference is the freedesktop
 * Desktop Notification Specification (IMPLEMENTED from, not
 * copied from — the license posture is in docs/THIRD-PARTY.md).
 */

#ifndef FDK_NOTIFY_H
#define FDK_NOTIFY_H

#include "fdk_core.h"
#include "fdk_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * The client
 * ========================================================================*/

typedef struct fdk_notify fdk_notify;

/* Where the client stands. NO_BUS and NO_DAEMON are states, not
 * errors: the API keeps working, sends are reported failed (or
 * queued while CONNECTING), and a late daemon is picked up. */
typedef enum {
    FDK_NOTIFY_STATUS_NO_BUS = 0,  /* no reachable session bus yet      */
    FDK_NOTIFY_STATUS_CONNECTING,  /* handshake / daemon probe running  */
    FDK_NOTIFY_STATUS_NO_DAEMON,   /* bus up, no notification daemon     */
    FDK_NOTIFY_STATUS_READY,       /* a daemon answered GetCapabilities  */
} fdk_notify_status;

/* The low/high end of the specification's urgency hint. */
typedef enum {
    FDK_NOTIFY_URGENCY_LOW      = 0,
    FDK_NOTIFY_URGENCY_NORMAL   = 1,
    FDK_NOTIFY_URGENCY_CRITICAL = 2,
} fdk_notify_urgency;

/* NotificationClosed reasons (the specification's numbers). */
enum {
    FDK_NOTIFY_CLOSED_EXPIRED   = 1, /* the timeout ran out               */
    FDK_NOTIFY_CLOSED_DISMISSED = 2, /* the user closed it                 */
    FDK_NOTIFY_CLOSED_REQUEST   = 3, /* someone called CloseNotification  */
    FDK_NOTIFY_CLOSED_UNKNOWN   = 4, /* the daemon's catch-all            */
};

/* One notification request. Every field has a benign default:
 * zero/NULL. `summary` is the only required field. Strings are
 * borrowed for the duration of fdk_notify_send() only (copied
 * before it returns). The actions array is a NULL-terminated flat
 * list of PAIRS — {"key1", "Label 1", "key2", "Label 2", NULL} —
 * and the special keys "default" (body click) and "inline-reply"
 * (where the daemon supports it) travel like any other. */
typedef struct {
    const char *app_name;      /* NULL -> "FDK application"               */
    uint32_t replaces_id;      /* 0 -> new; else an id from on_sent       */
    const char *icon;          /* theme name or "file:///..." or path     */
    const char *summary;       /* REQUIRED, plain text                    */
    const char *body;          /* NULL ok; limited markup                 */
    const char *const *actions;/* NULL-terminated flat key/label pairs    */
    fdk_notify_urgency urgency;
    int32_t timeout_ms;        /* -1 daemon default, 0 never, >0 ms       */

    /* Hints the desktop actually uses. */
    const char *category;      /* e.g. "email.arrived"; NULL ok           */
    const char *desktop_entry; /* the .desktop basename; NULL ok          */

    /* Raw image bytes (the image-data hint). RGB (3 channels) or
     * RGBA (4), row-major, `image_rowstride` bytes per row (0 ->
     * width * channels). This is the specification's native image
     * struct payload — network byte order never applies to a byte
     * array. Borrowed for the duration of the send call. */
    const void *image_data;
    int32_t image_width;
    int32_t image_height;
    int32_t image_rowstride;
    int image_has_alpha;
} fdk_notify_request;

/* All callbacks are optional (a NULL member or a NULL callbacks
 * pointer simply mutes that event). Every callback fires from the
 * event loop. `user` is the pointer given at open; `note_user` is
 * the one given at the send that produced the notification. */
typedef struct {
    /* The daemon is up and answered GetCapabilities —
     * fdk_notify_capabilities() now answers. Fires once per daemon
     * appearance (a daemon restart fires it again). */
    void (*on_ready)(fdk_notify *notify, void *user);
    /* The daemon accepted the notification and assigned `id` —
     * the key to pass as replaces_id later. */
    void (*on_sent)(fdk_notify *notify, uint32_t id, void *note_user,
                    void *user);
    /* The send failed: no daemon, no bus, a timeout, or an error
     * reply (error names the reason — an error NAME or "no bus" /
     * "timeout" / "overflow"; borrowed). */
    void (*on_failed)(fdk_notify *notify, void *note_user,
                      const char *error, void *user);
    /* The daemon closed one of ours (the FDK_NOTIFY_CLOSED_*
     * reasons). note_user is NULL when the mapping aged out. */
    void (*on_closed)(fdk_notify *notify, uint32_t id, uint32_t reason,
                      void *note_user, void *user);
    /* The user activated an action (action is its KEY, borrowed). */
    void (*on_action)(fdk_notify *notify, uint32_t id, const char *action,
                      void *note_user, void *user);
    void *user;
} fdk_notify_callbacks;

/* Opens the client. Never NULL except on allocation failure: a
 * missing bus is a STATUS (retried per send, 30 s throttle), not
 * an open failure. `callbacks` may be NULL. */
fdk_notify *fdk_notify_open(fdk_context *ctx,
                            const fdk_notify_callbacks *callbacks);

/* Tears the client down: cancels in-flight sends, drops the
 * subscriptions and the connection it owns. Safe from inside any
 * of its own callbacks (deferred, like every FDK teardown). NULL
 * is a no-op; not idempotent across double calls on the same
 * pointer. */
void fdk_notify_close(fdk_notify *notify);

fdk_notify_status fdk_notify_get_status(const fdk_notify *notify);

/* The daemon's capability strings ("actions", "body-markup",
 * "body-hyperlinks", "icon-static", ...) — NULL until the daemon
 * is READY, NULL-terminated, borrowed until the client is closed
 * or the daemon is replaced. */
const char *const *fdk_notify_capabilities(const fdk_notify *notify);

/* The daemon's identity from GetServerInformation (name, vendor,
 * version) — fetched with open; NULL until answered, borrowed
 * like the capabilities. Diagnostic surface. */
const char *fdk_notify_server_name(const fdk_notify *notify);
const char *fdk_notify_server_vendor(const fdk_notify *notify);

/* Queues a notification. Returns 1 when the request was accepted
 * for delivery (the verdict arrives in on_sent / on_failed —
 * sends before the daemon is ready are queued, in order), 0 when
 * the request itself is malformed (no summary) — no event fires
 * for a 0 return. note_user travels with every event about this
 * notification. */
int fdk_notify_send(fdk_notify *notify, const fdk_notify_request *req,
                    void *note_user);

/* Asks the daemon to withdraw a notification this client sent
 * (the daemon answers NotificationClosed reason 3). A no-op for
 * an unknown id. */
void fdk_notify_withdraw(fdk_notify *notify, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* FDK_NOTIFY_H */
