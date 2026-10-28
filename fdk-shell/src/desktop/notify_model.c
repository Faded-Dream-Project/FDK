/* notify_model.c — the notification server model (T-B side 2).
 *
 * The org.freedesktop.Notifications server: the Notify /
 * CloseNotification / GetCapabilities / GetServerInformation
 * protocol, the etiquette matrix against an incumbent daemon, the
 * flood gate, the live-set cap, expiration timers, and the note
 * record store — data and events, NO DRAWING. Extracted
 * verbatim-in-protocol from the reference UI (examples/notifyd.c);
 * the UI keeps card rendering and forwards interactions here. The
 * rig (tests/notifyd_rig.sh) proves the pair end to end and is the
 * behavior-preservation proof of this split.
 *
 * The records hold the SANITIZED RAW STRINGS (size-capped with an
 * honest ellipsis) — the notification markup -> FDK markup
 * translation is a rendering decision and stays with the UI.
 */
#include "fdk-shell/fdk-shell-desktop.h"
#include "fdk-shell/fdk-shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- constants ---------------------------------------------------------- */

#define N_NAME "org.freedesktop.Notifications"
#define N_PATH "/org/freedesktop/Notifications"
#define N_IFACE "org.freedesktop.Notifications"

#define CAP_COUNT 5

/* Input caps — everything beyond these is truncated or dropped, and
 * the truncation is honest (an ellipsis, a one-line WARN). */
#define MAX_SUMMARY_BYTES 2048
#define MAX_BODY_BYTES 8192
#define MAX_APP_NAME_BYTES 256
#define MAX_APP_ICON_BYTES 4096
#define MAX_ACTIONS 32        /* pairs accepted from a client        */
#define MAX_ACTION_KEY 64
#define MAX_ACTION_LABEL 128

/* Raw icon (image-data / icon_data) caps: geometry and derived byte
 * budget (the reference posture). */
#define MAX_ICON_DIM 512
#define MAX_ICON_BYTES (4 * MAX_ICON_DIM * MAX_ICON_DIM)

#define MAX_LIVE_NOTES_DEFAULT 32
#define RATE_WINDOW_MS 1000
#define RATE_MAX 25 /* Notify calls per window before dropping      */

static const char *const k_caps[] = {
    "actions", "body", "body-markup", "body-hyperlinks", "icon-static",
};

/* ---- the model's types --------------------------------------------------- */

typedef struct {
    char key[MAX_ACTION_KEY];
    char label[MAX_ACTION_LABEL];
} note_action;

typedef struct note {
    struct note *next; /* newest first                                */
    struct note *prev;

    uint32_t id;

    fdk_shell_notify_server *timer_owner; /* the expire closure's
                                           * way home                  */

    char *app_name;   /* informational (not drawn by the reference)  */
    char *app_icon;   /* the raw app_icon string arg                 */
    char *image_path; /* the image-path hint string                   */

    /* Sanitized raw content (truncated honestly; markup stays raw —
     * translating it is the renderer's business). */
    char *sum_raw;
    char *body_raw;

    uint8_t urgency; /* 0 low, 1 normal, 2 critical                 */
    int transient_hint;
    int resident_hint;

    note_action actions[MAX_ACTIONS];
    int action_count;
    int has_default; /* a "default" action key was supplied         */

    /* Raw icon pixmaps (ARGB32 host order; decoded once). */
    uint8_t *icon_bytes;      /* image-data (image_data accepted)   */
    int icon_w, icon_h;
    uint8_t *legacy_bytes;    /* the deprecated icon_data hint      */
    int legacy_w, legacy_h;

    uint32_t timeout_ms;  /* resolved expiration (0 = never)        */
    fdk_timer *timer;     /* expire timer, NULL when never          */
} note;

struct fdk_shell_notify_server {
    fdk_dbus_connection *bus;
    fdk_context *ctx;

    fdk_shell_notify_event_fn event_cb;
    void *event_user;
    char *server_name; /* the GetServerInformation identity (owned) */
    uint32_t default_timeout_ms;
    int max_live;
    int opt_replace;
    int opt_wait;

    note *head; /* doubly linked, newest first                      */
    int live_count;

    uint32_t next_id;

    int serving;
    int dead;
    int refused; /* the incumbent refusal: DORMANT forever (never a
                  * queue-lurker that grabs the name when the
                  * incumbent vacates)                          */

    /* Flood window. */
    uint64_t rate_times[RATE_MAX + 1];
    int rate_len;

    /* The name-etiquette plumbing. */
    fdk_dbus_name_watcher *self_watch;
};

/* ---- small utilities ----------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u +
           (uint64_t)ts.tv_nsec / 1000000u;
}

static char *xstrdup_n(const char *s, size_t cap) {
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    if (len > cap) {
        len = cap;
    }
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* strdup clipped to max bytes without breaking UTF-8, with an
 * ellipsis when truncation happened (the honest truncation). */
static char *trunc_utf8(const char *s, size_t max) {
    size_t len = strlen(s);
    if (len <= max) {
        return strdup(s);
    }
    size_t cut = max;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0u) == 0x80u) {
        cut--; /* land on a lead byte */
    }
    /* Leave room for the ellipsis (3 bytes). */
    while (cut > 0 && cut + 3 > max) {
        cut--;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0u) == 0x80u) {
            cut--;
        }
    }
    char *out = malloc(cut + 4);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, cut);
    memcpy(out + cut, "\xE2\x80\xA6", 4); /* U+2026 + NUL */
    return out;
}

/* One-line-per-burst WARN helper (floods must not amplify floods). */
static void warn_once(const char *key, const char *fmt, ...) {
    static char last_key[128];
    static time_t last_when;
    time_t now = time(NULL);
    if (strcmp(last_key, key) != 0 || now - last_when >= 5) {
        snprintf(last_key, sizeof(last_key), "%s", key);
        last_when = now;
        va_list ap;
        va_start(ap, fmt);
        fputs("fdk-shell: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        fflush(stderr);
        va_end(ap);
    }
}

static void notify_emit(fdk_shell_notify_server *s,
                        fdk_shell_notify_event_type ev, uint32_t id) {
    if (s != NULL && !s->dead && s->event_cb != NULL) {
        s->event_cb(s, ev, id, s->event_user);
    }
}

/* ---- hint readers ---------------------------------------------------------- */

/* A byte from an 'ay' value: the FLAT form first (every array that
 * arrived on the wire is flat since the value model grew the bulk
 * form), the tree form second (hand-built values). */
static uint8_t ay_byte(const fdk_dbus_value *ay, size_t index) {
    size_t n = 0;
    const uint8_t *flat = fdk_dbus_value_array_flat_bytes(ay, &n);
    if (flat != NULL) {
        return index < n ? flat[index] : 0;
    }
    const fdk_dbus_value *v = fdk_dbus_value_array_item(ay, index);
    if (v == NULL || fdk_dbus_value_type(v) != FDK_DBUS_BYTE) {
        return 0;
    }
    return (uint8_t)fdk_dbus_value_byte(v);
}

static uint8_t hint_urgency(const fdk_dbus_value *hints) {
    const fdk_dbus_value *v = fdk_dbus_value_dict_lookup(hints, "urgency");
    if (v == NULL) {
        return 1; /* normal is the default */
    }
    if (fdk_dbus_value_type(v) == FDK_DBUS_VARIANT) {
        v = fdk_dbus_value_variant_value(v);
    }
    if (fdk_dbus_value_type(v) != FDK_DBUS_BYTE) {
        return 1;
    }
    uint8_t u = fdk_dbus_value_byte(v);
    return u > 2 ? 1 : u;
}

static int hint_bool(const fdk_dbus_value *hints, const char *key) {
    const fdk_dbus_value *v = fdk_dbus_value_dict_lookup(hints, key);
    if (v == NULL) {
        return 0;
    }
    if (fdk_dbus_value_type(v) == FDK_DBUS_VARIANT) {
        v = fdk_dbus_value_variant_value(v);
    }
    return fdk_dbus_value_type(v) == FDK_DBUS_BOOLEAN &&
           fdk_dbus_value_boolean(v);
}

static char *hint_string(const fdk_dbus_value *hints, const char *key,
                         size_t cap) {
    const fdk_dbus_value *v = fdk_dbus_value_dict_lookup(hints, key);
    if (v == NULL) {
        return NULL;
    }
    if (fdk_dbus_value_type(v) == FDK_DBUS_VARIANT) {
        v = fdk_dbus_value_variant_value(v);
    }
    if (fdk_dbus_value_type(v) != FDK_DBUS_STRING) {
        return NULL;
    }
    return xstrdup_n(fdk_dbus_value_string(v), cap);
}

/* The image-data (iiibiiay) decode: width, height, rowstride,
 * has_alpha, bits_per_sample, channels, bytes — the SEVEN-field
 * shape (the field order is the spec's; the data array is item 6).
 * Network byte order; rowstride may exceed the dense row (the tail
 * is padding); channels 3 decodes forced-opaque (the reference
 * posture — RGB icons are common). Returns 0 on refusal. */
static int pixmap_from_image_data(const fdk_dbus_value *v,
                                  uint8_t **out_bytes, int *out_w,
                                  int *out_h) {
    if (v == NULL || fdk_dbus_value_type(v) != FDK_DBUS_STRUCT ||
        fdk_dbus_value_struct_count(v) != 7) {
        return 0;
    }
    const fdk_dbus_value *wv = fdk_dbus_value_struct_item(v, 0);
    const fdk_dbus_value *hv = fdk_dbus_value_struct_item(v, 1);
    const fdk_dbus_value *rv = fdk_dbus_value_struct_item(v, 2);
    const fdk_dbus_value *av = fdk_dbus_value_struct_item(v, 3);
    const fdk_dbus_value *bv = fdk_dbus_value_struct_item(v, 4);
    const fdk_dbus_value *cv = fdk_dbus_value_struct_item(v, 5);
    const fdk_dbus_value *dv = fdk_dbus_value_struct_item(v, 6);
    if (fdk_dbus_value_type(wv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(hv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(rv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(av) != FDK_DBUS_BOOLEAN ||
        fdk_dbus_value_type(bv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(cv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(dv) != FDK_DBUS_ARRAY) {
        return 0;
    }
    int32_t w = fdk_dbus_value_int32(wv);
    int32_t h = fdk_dbus_value_int32(hv);
    int32_t rowstride = fdk_dbus_value_int32(rv);
    int32_t bits = fdk_dbus_value_int32(bv);
    int32_t channels = fdk_dbus_value_int32(cv);
    if (w <= 0 || h <= 0 || w > MAX_ICON_DIM || h > MAX_ICON_DIM) {
        return 0;
    }
    if (bits != 8) { /* the spec: must always be 8 */
        return 0;
    }
    if (channels != 3 && channels != 4) {
        return 0;
    }
    if (rowstride < (int64_t)w * channels) {
        return 0;
    }
    /* The last row needs only width*channels bytes. */
    int64_t needed = (int64_t)rowstride * (h - 1) +
                     (int64_t)w * channels;
    if ((int64_t)fdk_dbus_value_array_count(dv) < needed) {
        return 0;
    }
    if (needed > MAX_ICON_BYTES) {
        return 0;
    }
    /* The spec's array is BYTEs; int32 arrays (gdbus's text-form
     * inference) are refused — reading ints as bytes would silently
     * yield an all-zero (fully transparent) icon. */
    {
        const char *elem = fdk_dbus_value_array_signature(dv);
        if (elem == NULL || elem[0] != 'y' || elem[1] != '\0') {
            return 0;
        }
    }
    size_t need = (size_t)w * (size_t)h * 4u;
    uint8_t *px = malloc(need);
    if (px == NULL) {
        return 0;
    }
    /* The bytes are R,G,B(,A) per pixel (the original's reader,
     * the probe's writer, and every real client agree); the output
     * is host-order ARGB32. Channels 3 is forced opaque. */
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t src = (size_t)y * (size_t)rowstride +
                         (size_t)x * (size_t)channels;
            size_t dst = (size_t)y * (size_t)w * 4u + (size_t)x * 4u;
            uint32_t r = ay_byte(dv, src);
            uint32_t g = ay_byte(dv, src + 1);
            uint32_t b = ay_byte(dv, src + 2);
            uint32_t a = channels == 4 ? ay_byte(dv, src + 3) : 0xFFu;
            uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;
            memcpy(px + dst, &argb, 4u);
        }
    }
    *out_bytes = px;
    *out_w = (int)w;
    *out_h = (int)h;
    return 1;
}

/* Picks the image-data hint (either spelling) or icon_data. */
static const fdk_dbus_value *hint_pixmap(const fdk_dbus_value *hints,
                                         int legacy) {
    if (legacy) {
        return fdk_dbus_value_dict_lookup(hints, "icon_data");
    }
    const fdk_dbus_value *v =
        fdk_dbus_value_dict_lookup(hints, "image-data");
    if (v == NULL) {
        v = fdk_dbus_value_dict_lookup(hints, "image_data");
    }
    return v;
}

/* ---- note lifecycle -------------------------------------------------------- */

static void note_free(note *n) {
    if (n->timer != NULL) {
        fdk_timer_remove(n->timer);
        n->timer = NULL;
    }
    free(n->app_name);
    free(n->app_icon);
    free(n->image_path);
    free(n->sum_raw);
    free(n->body_raw);
    free(n->icon_bytes);
    free(n->legacy_bytes);
    free(n);
}

/* Emits NotificationClosed(id, reason). */
static void emit_notification_closed(fdk_shell_notify_server *s,
                                     uint32_t id, uint32_t reason) {
    if (s->bus == NULL) {
        return;
    }
    fdk_dbus_message *m = fdk_dbus_message_new_signal(
        N_PATH, N_IFACE, "NotificationClosed");
    if (m == NULL) {
        return;
    }
    if (fdk_ok(fdk_dbus_message_append_uint32(m, id)) &&
        fdk_ok(fdk_dbus_message_append_uint32(m, reason))) {
        fdk_dbus_connection_send(s->bus, m);
    } else {
        fdk_dbus_message_free(m);
    }
}

/* Emits ActionInvoked(id, action_key). */
static void emit_action_invoked(fdk_shell_notify_server *s, uint32_t id,
                                const char *action_key) {
    if (s->bus == NULL) {
        return;
    }
    fdk_dbus_message *m = fdk_dbus_message_new_signal(
        N_PATH, N_IFACE, "ActionInvoked");
    if (m == NULL) {
        return;
    }
    if (fdk_ok(fdk_dbus_message_append_uint32(m, id)) &&
        fdk_ok(fdk_dbus_message_append_string(m, action_key))) {
        fdk_dbus_connection_send(s->bus, m);
    } else {
        fdk_dbus_message_free(m);
    }
}

static void close_note_internal(fdk_shell_notify_server *s, note *n,
                                uint32_t reason, int emit) {
    if (n->prev != NULL) {
        n->prev->next = n->next;
    } else {
        s->head = n->next;
    }
    if (n->next != NULL) {
        n->next->prev = n->prev;
    }
    s->live_count--;
    uint32_t id = n->id;
    note_free(n);
    if (emit) {
        emit_notification_closed(s, id, reason);
        notify_emit(s, FDK_SHELL_NOTIFY_EV_CLOSED, id);
    }
}

/* Force-expires the OLDEST dismissible note (flood / live-cap). */
static note *pick_overflow_victim(fdk_shell_notify_server *s) {
    note *last = NULL;
    for (note *n = s->head; n != NULL; n = n->next) {
        last = n;
    }
    for (note *n = last; n != NULL; n = n->prev) {
        if (n->urgency < 2) {
            return n;
        }
    }
    return last; /* all critical: the oldest goes anyway */
}

static void expire_timer_fn(fdk_timer *timer, void *user) {
    (void)timer;
    note *n = user;
    /* The note's server is reachable through the closure's owner:
     * the timer was created by the server, which stays alive longer
     * than every note (destroy() removes all timers first). */
    fdk_shell_notify_server *s = n->timer_owner;
    n->timer = NULL;
    close_note_internal(s, n, 1, 1);
}

/* ---- Notify ---------------------------------------------------------------- */

static void reply_error(fdk_dbus_connection *bus,
                        const fdk_dbus_message *call, const char *name,
                        const char *text) {
    fdk_dbus_message *err = fdk_dbus_message_new_error(call, name, text);
    if (err != NULL) {
        fdk_dbus_connection_send(bus, err);
    }
}

static void serve_notify(fdk_shell_notify_server *s,
                         const fdk_dbus_message *call) {
    /* Flood gate FIRST: a burst client must never reach layout. */
    uint64_t now = now_ms();
    if (s->rate_len == RATE_MAX + 1) {
        memmove(s->rate_times, s->rate_times + 1,
                (size_t)RATE_MAX * sizeof(uint64_t));
        s->rate_len--;
    }
    s->rate_times[s->rate_len++] = now;
    while (s->rate_len > 0 &&
           now - s->rate_times[0] > (uint64_t)RATE_WINDOW_MS) {
        memmove(s->rate_times, s->rate_times + 1,
                (size_t)(s->rate_len - 1) * sizeof(uint64_t));
        s->rate_len--;
    }
    int flooded = s->rate_len > RATE_MAX;
    if (flooded) {
        warn_once("rate",
                  "notify flood: more than %d Notify calls in %d ms — "
                  "excess notifications are acknowledged and dropped",
                  RATE_MAX, RATE_WINDOW_MS);
    }

    fdk_dbus_value **args = NULL;
    size_t argc_ = 0;
    fdk_result rr = fdk_dbus_message_read_args(call, &args, &argc_);
    if (!fdk_ok(rr) || argc_ != 8 || args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_STRING ||
        fdk_dbus_value_type(args[1]) != FDK_DBUS_UINT32 ||
        fdk_dbus_value_type(args[2]) != FDK_DBUS_STRING ||
        fdk_dbus_value_type(args[3]) != FDK_DBUS_STRING ||
        fdk_dbus_value_type(args[4]) != FDK_DBUS_STRING ||
        fdk_dbus_value_type(args[5]) != FDK_DBUS_ARRAY ||
        fdk_dbus_value_type(args[6]) != FDK_DBUS_ARRAY ||
        fdk_dbus_value_type(args[7]) != FDK_DBUS_INT32) {
        if (args != NULL) {
            fdk_dbus_values_free_array(args, argc_);
        }
        reply_error(s->bus, call, "org.freedesktop.DBus.Error."
                                  "InvalidArgs",
                    "Notify expects (susssasa{sv}i)");
        return;
    }

    const char *app_name = fdk_dbus_value_string(args[0]);
    uint32_t replaces_id = fdk_dbus_value_uint32(args[1]);
    const char *app_icon = fdk_dbus_value_string(args[2]);
    const char *summary = fdk_dbus_value_string(args[3]);
    const char *body = fdk_dbus_value_string(args[4]);
    const fdk_dbus_value *actions = args[5];
    const fdk_dbus_value *hints = args[6];
    int32_t expire_timeout = fdk_dbus_value_int32(args[7]);

    /* A visible id is assigned even under the flood gate: the
     * caller must never block or spin on a missing reply. */
    uint32_t id = 0;
    note *replaced = NULL;
    if (replaces_id != 0) {
        for (note *n = s->head; n != NULL; n = n->next) {
            if (n->id == replaces_id) {
                replaced = n;
                break;
            }
        }
    }
    if (replaced != NULL) {
        id = replaces_id; /* the spec: the same id returns */
    } else {
        id = s->next_id++;
        if (s->next_id == 0) {
            s->next_id = 1; /* never hand out 0 */
        }
    }

    if (flooded) {
        fdk_dbus_values_free_array(args, argc_);
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL &&
                fdk_ok(fdk_dbus_message_append_uint32(rep, id))) {
                fdk_dbus_connection_send(s->bus, rep);
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
        }
        return;
    }

    /* Build the note. */
    note *n = calloc(1, sizeof(note));
    if (n == NULL) {
        fdk_dbus_values_free_array(args, argc_);
        reply_error(s->bus, call, "org.freedesktop.DBus.Error."
                                  "NoMemory",
                    "allocation failed");
        return;
    }
    n->id = id;
    n->timer_owner = s;

    n->app_name = app_name != NULL
                      ? xstrdup_n(app_name, MAX_APP_NAME_BYTES)
                      : NULL;
    n->app_icon = app_icon != NULL && app_icon[0] != '\0'
                      ? xstrdup_n(app_icon, MAX_APP_ICON_BYTES)
                      : NULL;
    n->image_path = hint_string(hints, "image-path", MAX_APP_ICON_BYTES);
    if (n->image_path == NULL) {
        n->image_path =
            hint_string(hints, "image_path", MAX_APP_ICON_BYTES);
    }
    n->sum_raw = summary != NULL ? trunc_utf8(summary, MAX_SUMMARY_BYTES)
                                 : NULL;
    n->body_raw = body != NULL && body[0] != '\0'
                      ? trunc_utf8(body, MAX_BODY_BYTES)
                      : NULL;

    n->urgency = hint_urgency(hints);
    n->transient_hint = hint_bool(hints, "transient");
    n->resident_hint = hint_bool(hints, "resident");

    /* Actions: (key,label) pairs; "default" is captured, up to
     * MAX_ACTIONS others are kept (the UI draws what it can). */
    size_t acount = fdk_dbus_value_array_count(actions);
    for (size_t i = 0; i + 1 < acount &&
                       n->action_count < MAX_ACTIONS;) {
        const fdk_dbus_value *k = fdk_dbus_value_array_item(actions, i);
        const fdk_dbus_value *l =
            fdk_dbus_value_array_item(actions, i + 1);
        if (fdk_dbus_value_type(k) != FDK_DBUS_STRING ||
            fdk_dbus_value_type(l) != FDK_DBUS_STRING) {
            break; /* malformed pairs: stop, keep what parsed */
        }
        const char *key = fdk_dbus_value_string(k);
        const char *label = fdk_dbus_value_string(l);
        if (strcmp(key, "default") == 0) {
            n->has_default = 1;
        } else if (key[0] != '\0' && label[0] != '\0') {
            snprintf(n->actions[n->action_count].key, MAX_ACTION_KEY,
                     "%s", key);
            snprintf(n->actions[n->action_count].label,
                     MAX_ACTION_LABEL, "%s", label);
            n->action_count++;
        }
        i += 2;
    }
    /* An odd trailing element is ignored (the spec sends pairs).
     * A "default"-only pair at the tail when the loop broke early: */
    for (size_t i = 0; i + 1 < acount; i += 2) {
        const fdk_dbus_value *k = fdk_dbus_value_array_item(actions, i);
        const fdk_dbus_value *l =
            fdk_dbus_value_array_item(actions, i + 1);
        if (fdk_dbus_value_type(k) == FDK_DBUS_STRING &&
            fdk_dbus_value_type(l) == FDK_DBUS_STRING &&
            strcmp(fdk_dbus_value_string(k), "default") == 0) {
            n->has_default = 1;
        }
    }

    /* Raw icons (the spec's data channels; the priority order's
     * file/theme lookups are the renderer's). */
    {
        const fdk_dbus_value *img = hint_pixmap(hints, 0);
        if (img != NULL) {
            const fdk_dbus_value *inner =
                fdk_dbus_value_type(img) == FDK_DBUS_VARIANT
                    ? fdk_dbus_value_variant_value(img)
                    : img;
            pixmap_from_image_data(inner, &n->icon_bytes, &n->icon_w,
                                   &n->icon_h);
        }
        const fdk_dbus_value *legacy = hint_pixmap(hints, 1);
        if (legacy != NULL) {
            const fdk_dbus_value *inner =
                fdk_dbus_value_type(legacy) == FDK_DBUS_VARIANT
                    ? fdk_dbus_value_variant_value(legacy)
                    : legacy;
            pixmap_from_image_data(inner, &n->legacy_bytes,
                                   &n->legacy_w, &n->legacy_h);
        }
    }

    /* Expiration: -1 = the server default (critical: never, per the
     * spec's "should not automatically expire"); 0 = never; >0 = ms. */
    uint32_t timeout;
    if (expire_timeout > 0) {
        timeout = (uint32_t)expire_timeout;
    } else if (expire_timeout == 0) {
        timeout = 0;
    } else if (n->urgency >= 2) {
        timeout = 0;
    } else {
        timeout = s->default_timeout_ms;
    }
    n->timeout_ms = timeout;

    fdk_dbus_values_free_array(args, argc_);

    /* Replacing: the old note's identity CONTINUES (no closed
     * signal); destroy the old record, keep the id. */
    if (replaced != NULL) {
        close_note_internal(s, replaced, 0, 0); /* reason 0: no signal */
    }

    /* Live-set cap: force-expire the oldest dismissible note. */
    while (s->live_count >= s->max_live) {
        note *victim = pick_overflow_victim(s);
        if (victim == NULL) {
            break;
        }
        warn_once("livecap",
                  "live-notification cap (%d) reached — the oldest "
                  "notification expires early",
                  s->max_live);
        close_note_internal(s, victim, 1, 1);
    }

    /* Insert newest-first. */
    n->next = s->head;
    n->prev = NULL;
    if (s->head != NULL) {
        s->head->prev = n;
    }
    s->head = n;
    s->live_count++;

    if (timeout > 0) {
        n->timer = fdk_timer_add(s->ctx, timeout, false,
                                 expire_timer_fn, n);
    }

    notify_emit(s, replaced != NULL ? FDK_SHELL_NOTIFY_EV_REPLACED
                                    : FDK_SHELL_NOTIFY_EV_ADDED,
                id);

    if (!(fdk_dbus_message_flags(call) &
          FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            if (fdk_ok(fdk_dbus_message_append_uint32(rep, id))) {
                fdk_dbus_connection_send(s->bus, rep);
            } else {
                fdk_dbus_message_free(rep);
            }
        }
    }
}

/* ---- CloseNotification / capabilities / info ----------------------------- */

static void serve_close_notification(fdk_shell_notify_server *s,
                                     const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t argc_ = 0;
    uint32_t id = 0;
    if (fdk_dbus_message_read_args(call, &args, &argc_) == FDK_OK &&
        argc_ == 1 && args != NULL &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32) {
        id = fdk_dbus_value_uint32(args[0]);
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, argc_);
    }
    if (id == 0) {
        reply_error(s->bus, call, "org.freedesktop.DBus.Error."
                                  "InvalidArgs",
                    "CloseNotification requires one uint32 id");
        return;
    }
    for (note *n = s->head; n != NULL; n = n->next) {
        if (n->id == id) {
            close_note_internal(s, n, 3, 1); /* reason 3: by the call */
            /* An (optional) empty reply — most servers send one. */
            if (!(fdk_dbus_message_flags(call) &
                  FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                fdk_dbus_message *rep =
                    fdk_dbus_message_new_method_return(call);
                if (rep != NULL) {
                    fdk_dbus_connection_send(s->bus, rep);
                }
            }
            return;
        }
    }
    /* The specification: an unknown id gets an empty error reply. */
    reply_error(s->bus, call,
                "org.freedesktop.DBus.Error.InvalidArgs",
                "no such notification");
}

static void serve_get_capabilities(fdk_shell_notify_server *s,
                                   const fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }
    fdk_dbus_value *items[CAP_COUNT];
    for (size_t i = 0; i < CAP_COUNT; i++) {
        items[i] = fdk_dbus_value_new_string(k_caps[i]);
    }
    /* new_array OWNS the values from here (freeing arr frees them);
     * message_append then deep-copies into the reply. */
    fdk_dbus_value *arr = fdk_dbus_value_new_array("s", items, CAP_COUNT);
    if (arr != NULL && fdk_ok(fdk_dbus_message_append(rep, arr))) {
        fdk_dbus_value_free(arr);
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(s->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    } else {
        fdk_dbus_value_free(arr);
        fdk_dbus_message_free(rep);
    }
}

static void serve_get_server_information(fdk_shell_notify_server *s,
                                         const fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }
    int ok = fdk_ok(fdk_dbus_message_append_string(
                        rep, s->server_name)) &&
             fdk_ok(fdk_dbus_message_append_string(
                 rep, "Faded Dream Project")) &&
             fdk_ok(fdk_dbus_message_append_string(
                 rep, fdk_shell_version())) &&
             fdk_ok(fdk_dbus_message_append_string(rep, "1.2"));
    if (ok) {
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(s->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    } else {
        fdk_dbus_message_free(rep);
    }
}

/* ---- the method dispatcher ------------------------------------------------- */

static const char *k_introspect_xml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
    "Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect."
    "dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.Notifications\">\n"
    "    <method name=\"Notify\">\n"
    "      <arg name=\"app_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"replaces_id\" type=\"u\" direction=\"in\"/>\n"
    "      <arg name=\"app_icon\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"summary\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"body\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"actions\" type=\"as\" direction=\"in\"/>\n"
    "      <arg name=\"hints\" type=\"a{sv}\" direction=\"in\"/>\n"
    "      <arg name=\"expire_timeout\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"id\" type=\"u\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"CloseNotification\">\n"
    "      <arg name=\"id\" type=\"u\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"GetCapabilities\">\n"
    "      <arg name=\"capabilities\" type=\"as\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetServerInformation\">\n"
    "      <arg name=\"name\" type=\"s\" direction=\"out\"/>\n"
    "      <arg name=\"vendor\" type=\"s\" direction=\"out\"/>\n"
    "      <arg name=\"version\" type=\"s\" direction=\"out\"/>\n"
    "      <arg name=\"spec_version\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"NotificationClosed\">\n"
    "      <arg name=\"id\" type=\"u\"/>\n"
    "      <arg name=\"reason\" type=\"u\"/>\n"
    "    </signal>\n"
    "    <signal name=\"ActionInvoked\">\n"
    "      <arg name=\"id\" type=\"u\"/>\n"
    "      <arg name=\"action_key\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" "
    "direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "    <method name=\"Ping\"/>\n"
    "    <method name=\"GetMachineId\">\n"
    "      <arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n";

static void serve_ping(fdk_shell_notify_server *s,
                       const fdk_dbus_message *call) {
    fdk_dbus_message *rep =
        fdk_dbus_message_new_method_return(call);
    if (rep != NULL) {
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(s->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    }
}

static void serve_get_machine_id(fdk_shell_notify_server *s,
                                 const fdk_dbus_message *call) {
    char id[64] = "";
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f != NULL) {
        if (fgets(id, sizeof(id), f) == NULL) {
            id[0] = '\0';
        }
        fclose(f);
        size_t n = strlen(id);
        while (n > 0 && (id[n - 1] == '\n' || id[n - 1] == '\r')) {
            id[--n] = '\0';
        }
    }
    fdk_dbus_message *rep =
        fdk_dbus_message_new_method_return(call);
    if (rep != NULL) {
        if (fdk_ok(fdk_dbus_message_append_string(rep, id))) {
            if (!(fdk_dbus_message_flags(call) &
                  FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                fdk_dbus_connection_send(s->bus, rep);
            } else {
                fdk_dbus_message_free(rep);
            }
        } else {
            fdk_dbus_message_free(rep);
        }
    }
}

static void serve_properties(fdk_shell_notify_server *s,
                             const fdk_dbus_message *call) {
    const char *member = fdk_dbus_message_member(call);
    if (member != NULL && strcmp(member, "GetAll") == 0) {
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            fdk_dbus_value *empty =
                fdk_dbus_value_new_array("{sv}", NULL, 0);
            if (empty != NULL &&
                fdk_ok(fdk_dbus_message_append(rep, empty))) {
                fdk_dbus_value_free(empty);
                if (!(fdk_dbus_message_flags(call) &
                      FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                    fdk_dbus_connection_send(s->bus, rep);
                } else {
                    fdk_dbus_message_free(rep);
                }
            } else {
                fdk_dbus_value_free(empty);
                fdk_dbus_message_free(rep);
            }
        }
        return;
    }
    if (member != NULL && strcmp(member, "Get") == 0) {
        reply_error(s->bus, call,
                    "org.freedesktop.DBus.Error.InvalidArgs",
                    "the notification server publishes no properties");
        return;
    }
    if (member != NULL && strcmp(member, "Set") == 0) {
        reply_error(s->bus, call,
                    "org.freedesktop.DBus.Error.PropertyReadOnly",
                    "the notification server's properties are "
                    "read-only");
        return;
    }
    reply_error(s->bus, call,
                "org.freedesktop.DBus.Error.UnknownMethod",
                "Unknown method for org.freedesktop.DBus.Properties");
}

static void method_handler(fdk_dbus_connection *bus,
                           fdk_dbus_message *call, void *user) {
    fdk_shell_notify_server *s = user;
    (void)bus;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);

    if (iface != NULL && strcmp(iface, N_IFACE) == 0) {
        if (strcmp(member, "Notify") == 0) {
            serve_notify(s, call);
            return;
        }
        if (strcmp(member, "CloseNotification") == 0) {
            serve_close_notification(s, call);
            return;
        }
        if (strcmp(member, "GetCapabilities") == 0) {
            serve_get_capabilities(s, call);
            return;
        }
        if (strcmp(member, "GetServerInformation") == 0) {
            serve_get_server_information(s, call);
            return;
        }
    } else if (iface != NULL &&
               strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
        serve_properties(s, call);
        return;
    } else if (iface != NULL &&
               strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0) {
        if (strcmp(member, "Introspect") == 0) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL &&
                fdk_ok(fdk_dbus_message_append_string(rep,
                                                      k_introspect_xml))) {
                if (!(fdk_dbus_message_flags(call) &
                      FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                    fdk_dbus_connection_send(s->bus, rep);
                } else {
                    fdk_dbus_message_free(rep);
                }
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
            return;
        }
    } else if (iface != NULL &&
               strcmp(iface, "org.freedesktop.DBus.Peer") == 0) {
        if (strcmp(member, "Ping") == 0) {
            serve_ping(s, call);
            return;
        }
        if (strcmp(member, "GetMachineId") == 0) {
            serve_get_machine_id(s, call);
            return;
        }
    }
    fdk_dbus_message *err = fdk_dbus_message_new_error(
        call, "org.freedesktop.DBus.Error.UnknownMethod",
        "The notification server does not implement this method");
    if (err != NULL) {
        fdk_dbus_connection_send(s->bus, err);
    }
}

/* ---- the name etiquette (the reference matrix, verbatim) ----------------- */

/* The RequestName reply: EXISTS is the refusal that must reach the
 * host app (a --replace against a stubborn incumbent) — the event
 * is SERVER_STATE(0); the app decides to exit. IN_QUEUE is the
 * --wait posture (serving when the incumbent releases; the
 * NameAcquired path completes it). */
static void request_name_reply(fdk_dbus_connection *bus,
                               fdk_dbus_message *reply, void *user) {
    (void)bus;
    fdk_shell_notify_server *s = user;
    uint32_t code = 0;
    if (reply != NULL && !fdk_dbus_message_is_error(reply)) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK &&
            n == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32) {
            code = fdk_dbus_value_uint32(args[0]);
        }
        if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
    }
    if (code == FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER ||
        code == FDK_DBUS_REQUEST_NAME_ALREADY_OWNER) {
        /* NameAcquired usually beat the reply; the event fires from
         * there (once — the serving guard holds). */
        return;
    }
    if (code == FDK_DBUS_REQUEST_NAME_IN_QUEUE) {
        /* --wait: serving when the incumbent releases. */
        return;
    }
    /* EXISTS (or an error reply): the refusal. */
    if (!s->serving) {
        s->refused = 1;
        notify_emit(s, FDK_SHELL_NOTIFY_EV_SERVER_STATE, 0);
    }
}

static void request_the_name(fdk_shell_notify_server *s) {
    uint32_t flags = FDK_DBUS_NAME_ALLOW_REPLACEMENT;
    if (s->opt_replace) {
        flags |= FDK_DBUS_NAME_REPLACE_EXISTING;
    }
    if (!s->opt_wait) {
        flags |= FDK_DBUS_NAME_DO_NOT_QUEUE;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, N_NAME)) ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, flags))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        s->bus, call, 5000, request_name_reply, s);
    (void)p;
}

static void name_owner_event(fdk_dbus_connection *bus, const char *name,
                             const char *owner, void *user) {
    (void)name;
    fdk_shell_notify_server *s = user;
    const char *me = fdk_dbus_connection_unique_name(bus);
    if (owner != NULL && me != NULL && strcmp(owner, me) == 0) {
        return; /* our own acquisition echoing through the watcher   */
    }
    if (owner != NULL && !s->opt_replace && !s->opt_wait &&
        !s->serving) {
        /* The incumbent refusal: SERVER_STATE(0), stay dormant. */
        if (!s->refused) {
            s->refused = 1;
            notify_emit(s, FDK_SHELL_NOTIFY_EV_SERVER_STATE, 0);
        }
        return;
    }
    if (!s->refused && !s->serving &&
        (owner == NULL || s->opt_replace || s->opt_wait)) {
        request_the_name(s);
    }
}

static void name_event(fdk_dbus_connection *bus, const char *name,
                       int owned, void *user) {
    (void)bus;
    fdk_shell_notify_server *s = user;
    if (name == NULL || strcmp(name, N_NAME) != 0) {
        return;
    }
    if (owned) {
        if (!s->serving) {
            s->serving = 1;
            notify_emit(s, FDK_SHELL_NOTIFY_EV_SERVER_STATE, 1);
        }
    } else if (s->serving) {
        s->serving = 0;
        notify_emit(s, FDK_SHELL_NOTIFY_EV_SERVER_STATE, 0);
    }
}

/* ---- public snapshot APIs ------------------------------------------------- */

static void copy_field(char *ptr, size_t *len, const char *src) {
    if (ptr == NULL || len == NULL) {
        return;
    }
    size_t cap = *len;
    size_t n = 0;
    if (src != NULL) {
        n = strlen(src);
        if (n > cap) {
            n = cap;
        }
        memcpy(ptr, src, n);
    }
    if (cap > 0) {
        ptr[n] = '\0';
    }
    *len = n;
}

uint32_t fdk_shell_notify_note_count(
    const fdk_shell_notify_server *server) {
    if (server == NULL) {
        return 0;
    }
    uint32_t n = 0;
    for (const note *p = server->head; p != NULL; p = p->next) {
        n++;
    }
    return n;
}

static const note *note_find(const fdk_shell_notify_server *s,
                             uint32_t id) {
    for (const note *p = s->head; p != NULL; p = p->next) {
        if (p->id == id) {
            return p;
        }
    }
    return NULL;
}

fdk_result fdk_shell_notify_note_snapshot(
    const fdk_shell_notify_server *server, uint32_t id,
    fdk_shell_notify_note_info *out) {
    if (server == NULL || out == NULL ||
        out->struct_size != sizeof(*out)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const note *n = note_find(server, id);
    if (n == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    out->id = n->id;
    out->urgency = n->urgency;
    out->transient_hint = n->transient_hint;
    out->resident_hint = n->resident_hint;
    out->has_default = n->has_default;
    out->action_count = (uint32_t)n->action_count;
    copy_field(out->summary, &out->summary_len, n->sum_raw);
    copy_field(out->body, &out->body_len, n->body_raw);
    copy_field(out->app_icon_name, &out->app_icon_name_len, n->app_icon);
    copy_field(out->image_path, &out->image_path_len, n->image_path);
    out->timeout_ms = n->timeout_ms;
    return FDK_OK;
}

fdk_result fdk_shell_notify_note_action(
    const fdk_shell_notify_server *server, uint32_t id,
    uint32_t index, char *key_buf, size_t key_cap, char *label_buf,
    size_t label_cap) {
    if (server == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const note *n = note_find(server, id);
    if (n == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    if (index >= (uint32_t)n->action_count) {
        return FDK_ERR_NOT_FOUND;
    }
    size_t kl = key_cap, ll = label_cap;
    copy_field(key_buf, &kl, n->actions[index].key);
    copy_field(label_buf, &ll, n->actions[index].label);
    return FDK_OK;
}

fdk_result fdk_shell_notify_note_icon(
    const fdk_shell_notify_server *server, uint32_t id, int which,
    int *out_w, int *out_h, uint8_t *buf, size_t cap,
    size_t *out_needed) {
    if (server == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const note *n = note_find(server, id);
    if (n == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    const uint8_t *bytes = which ? n->legacy_bytes : n->icon_bytes;
    int w = which ? n->legacy_w : n->icon_w;
    int h = which ? n->legacy_h : n->icon_h;
    if (bytes == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    size_t need = (size_t)w * (size_t)h * 4u;
    if (out_w != NULL) {
        *out_w = w;
    }
    if (out_h != NULL) {
        *out_h = h;
    }
    if (out_needed != NULL) {
        *out_needed = need;
    }
    if (buf == NULL) {
        return FDK_OK; /* the documented probe form (dimensions +
                        * byte count only)                        */
    }
    if (cap < need) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memcpy(buf, bytes, need);
    return FDK_OK;
}

/* ---- public interaction APIs ---------------------------------------------- */

fdk_result fdk_shell_notify_action_invoke(
    fdk_shell_notify_server *server, uint32_t id, const char *action_key) {
    if (server == NULL || action_key == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    note *n = NULL;
    for (note *p = server->head; p != NULL; p = p->next) {
        if (p->id == id) {
            n = p;
            break;
        }
    }
    if (n == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    emit_action_invoked(server, id, action_key);
    if (!n->resident_hint) {
        close_note_internal(server, n, 2, 1); /* reason 2: dismissed */
    }
    return FDK_OK;
}

fdk_result fdk_shell_notify_close(fdk_shell_notify_server *server,
                                  uint32_t id, uint32_t reason) {
    if (server == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    note *n = NULL;
    for (note *p = server->head; p != NULL; p = p->next) {
        if (p->id == id) {
            n = p;
            break;
        }
    }
    if (n == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    if (reason == 0) {
        reason = 4; /* the spec's unspecified bucket                */
    }
    close_note_internal(server, n, reason, 1);
    return FDK_OK;
}

/* ---- lifecycle --------------------------------------------------------------- */

fdk_result fdk_shell_notify_server_create(
    fdk_dbus_connection *bus, fdk_context *ctx,
    const fdk_shell_notify_server_desc *desc,
    fdk_shell_notify_server **out) {
    if (bus == NULL || ctx == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (desc != NULL && desc->struct_size != 0 &&
        desc->struct_size != sizeof(*desc)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    int replace = desc != NULL ? desc->replace : 0;
    int wait = desc != NULL ? desc->wait : 0;
    if (replace && wait) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!fdk_dbus_connection_ready(bus)) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    fdk_shell_notify_server *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    s->bus = bus;
    s->ctx = ctx;
    s->event_cb = desc != NULL ? desc->event_cb : NULL;
    s->event_user = desc != NULL ? desc->user : NULL;
    s->server_name = xstrdup_n(
        desc != NULL && desc->server_name != NULL ? desc->server_name
                                                  : "fdk-shell",
        255);
    if (s->server_name == NULL) {
        free(s);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    s->default_timeout_ms = desc != NULL && desc->default_timeout_ms > 0
                                ? desc->default_timeout_ms
                                : 5000;
    s->max_live = desc != NULL && desc->max_live > 0 ? desc->max_live
                                                     : MAX_LIVE_NOTES_DEFAULT;
    s->opt_replace = replace;
    s->opt_wait = wait;
    s->next_id = 1;

    fdk_dbus_connection_set_method_handler(bus, method_handler, s);
    fdk_dbus_connection_set_name_events(bus, name_event, s);

    s->self_watch = fdk_dbus_connection_watch_name(
        bus, N_NAME, name_owner_event, s);
    if (s->self_watch == NULL) {
        fdk_dbus_connection_set_method_handler(bus, NULL, NULL);
        fdk_dbus_connection_set_name_events(bus, NULL, NULL);
        free(s);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    *out = s;
    return FDK_OK;
}

void fdk_shell_notify_server_destroy(fdk_shell_notify_server *server) {
    if (server == NULL || server->dead) {
        return;
    }
    server->dead = 1; /* events must not fire into a dying server */

    /* Close every note WITHOUT the signals — teardown, not
     * delivery (the fdk_shell_shutdown discipline). */
    while (server->head != NULL) {
        note *n = server->head;
        server->head = n->next;
        server->live_count--;
        note_free(n);
    }

    fdk_dbus_name_watcher_free(server->self_watch);
    if (server->serving && server->bus != NULL) {
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ReleaseName");
        if (call != NULL &&
            fdk_ok(fdk_dbus_message_append_string(call, N_NAME))) {
            fdk_dbus_pending *p =
                fdk_dbus_connection_call(server->bus, call, 2000,
                                         NULL, NULL);
            (void)p;
        } else if (call != NULL) {
            fdk_dbus_message_free(call);
        }
    }

    if (server->bus != NULL) {
        fdk_dbus_connection_set_method_handler(server->bus, NULL, NULL);
        fdk_dbus_connection_set_name_events(server->bus, NULL, NULL);
    }

    free(server->server_name);
    free(server);
}

int fdk_shell_notify_server_serving(
    const fdk_shell_notify_server *server) {
    return server != NULL ? server->serving : 0;
}
