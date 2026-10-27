/* notifyd.c — the fdk-shell notification daemon (S-2 / A-12).
 *
 * A complete org.freedesktop.Notifications SERVER on FDK's native
 * D-Bus client, displaying its cards as fdk-shell OVERLAY layer
 * surfaces: one layer per notification, sized exactly to the card
 * (nothing oversized to eat clicks), stacked from the chosen corner
 * of every output. Written from the freedesktop Desktop
 * Notification Specification v1.3 in public form — no notification
 * daemon's code was read or ported (the license posture,
 * docs/THIRD-PARTY.md).
 *
 * What it serves, honestly:
 *   org.freedesktop.Notifications.Notify            (the full hint set)
 *   org.freedesktop.Notifications.CloseNotification
 *   org.freedesktop.Notifications.GetCapabilities   (a truthful subset)
 *   org.freedesktop.Notifications.GetServerInformation
 *   + NotificationClosed (reasons 1/2/3) and ActionInvoked signals,
 *     and org.freedesktop.DBus.Introspectable for bus tools.
 *
 * The untrusted-input posture: every byte over the bus is hostile.
 * Body markup is sanitized to a render-only subset (hyperlinks are
 * drawn as underlined text and NEVER followed; <img> is stripped,
 * its src never touched); sizes are capped (text, actions, icon
 * geometry, raw icon bytes); a burst limiter drops floods while
 * still answering every call; GetCapabilities lists only what is
 * truly implemented.
 *
 * Bus-name etiquette (the real-desktop rules):
 *   - default: RequestName(ALLOW_REPLACEMENT | DO_NOT_QUEUE). If
 *     another daemon owns the name, ONE clear line names it (its
 *     GetServerInformation, when it answers) and notifyd exits 0.
 *   - --replace: adds REPLACE_EXISTING — takes over from daemons
 *     that allowed replacement (they get their NameLost and go).
 *   - --wait: queues instead of exiting; serves when freed.
 *   - when replaced: one line, clean exit 0. Never crashes, never
 *     spins.
 *
 * Trace: FDK_NOTIFYD_TRACE=1 logs every bus event and placement
 * decision to stderr — one run's log is a complete diagnosis.
 *
 * Build:  make -C fdk-shell examples     (links fdk + fdk-shell)
 * Run:    ./fdk-shell/build/examples/notifyd [--timeout-ms N]
 *               [--max-visible N] [--width N] [--margin N] [--gap N]
 *               [--anchor tr|tl|br|bl] [--output N] [--replace]
 *               [--wait] [--quit-after S] [--help] [--version]
 *
 * Status honesty (the Phase-2 rule): what the rigs below prove is
 * DONE; what only a real desktop can prove is marked in
 * docs/TESTING-ON-DESKTOP.md for the owner.
 */

#include <fdk-shell/fdk-shell.h>

#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_theme.h"

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>

/* ---- constants ------------------------------------------------------- */

#define N_NAME "org.freedesktop.Notifications"
#define N_PATH "/org/freedesktop/Notifications"
#define N_IFACE "org.freedesktop.Notifications"

/* The capabilities we truly implement (the spec's names). */
static const char *const k_caps[] = {
    "actions", "body", "body-markup", "body-hyperlinks", "icon-static",
};
#define CAP_COUNT 5

/* Input caps — everything beyond these is truncated or dropped, and
 * the truncation is honest (an ellipsis, a one-line WARN). */
#define MAX_SUMMARY_BYTES 2048
#define MAX_BODY_BYTES 8192
#define MAX_APP_NAME_BYTES 256
#define MAX_APP_ICON_BYTES 4096
#define MAX_ACTIONS 32        /* pairs accepted from a client        */
#define MAX_ACTION_BUTTONS 3  /* buttons actually drawn              */
#define MAX_ACTION_KEY 64
#define MAX_ACTION_LABEL 128

/* Raw icon (image-data) caps: geometry and derived byte budget. */
#define MAX_ICON_DIM 512
#define MAX_ICON_BYTES (4 * MAX_ICON_DIM * MAX_ICON_DIM)

/* Live-set and flood caps. */
#define MAX_LIVE_NOTES 32
#define RATE_WINDOW_MS 1000
#define RATE_MAX 25 /* Notify calls per window before dropping      */

/* Card metrics (logical px). */
/* The UI metrics (TASK C): every card dimension derives from the
 * process's UI font size (fdk_ui_font_px — theme/settings, the
 * desktop's statement, default 16), scaled off the 14 px baseline
 * the card was designed against, so text, box, paddings, icons and
 * buttons grow together and nothing clips. --font-size pins it. */
typedef struct {
    int font_px;
    double k; /* font_px / 14 — the design baseline               */
    int pad_x, pad_y;   /* card interior padding (14 / 12)        */
    int radius;         /* the card's corner radius (8)           */
    int icon_box;       /* the icon's square (36)                 */
    int icon_gap;       /* icon-to-text gap (10)                  */
    int button_h;       /* action button height (26)              */
    int button_gap;     /* button block's top gap (8)             */
    int button_r;       /* the button's corner radius (5)         */
    int button_pad;     /* the button's side padding (16 / 8)     */
    int strip_w;        /* the urgency strip's width (3)          */
    int text_gap;       /* summary-to-body gap (4)                */
} ui_metrics;

static ui_metrics g_ui;

static int sc(int base) {
    int v = (int)((double)base * g_ui.k + 0.5);
    return v < 1 ? 1 : v;
}

static void ui_init(int font_override_px) {
    int f = font_override_px > 0 ? font_override_px : fdk_ui_font_px();
    if (f < 8) {
        f = 8;
    }
    if (f > 96) {
        f = 96;
    }
    g_ui.font_px = f;
    g_ui.k = (double)f / 14.0;
    g_ui.pad_x = sc(14);
    g_ui.pad_y = sc(12);
    g_ui.radius = sc(8);
    g_ui.icon_box = sc(36);
    g_ui.icon_gap = sc(10);
    g_ui.button_h = sc(26);
    g_ui.button_gap = sc(8);
    g_ui.button_r = sc(5);
    g_ui.button_pad = sc(16);
    g_ui.strip_w = sc(3);
    g_ui.text_gap = sc(4);
}
#define MAX_BODY_LINES 6

/* ---- logging ------------------------------------------------------------ */

static int g_trace; /* FDK_NOTIFYD_TRACE=1 */

static void tracef(const char *fmt, ...) {
    if (!g_trace) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fputs("notifyd[trace]: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

static void sayf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("notifyd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
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
        fputs("notifyd: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        fflush(stderr);
        va_end(ap);
    }
}

/* ---- options ------------------------------------------------------------ */

typedef enum { ANCHOR_TR = 0, ANCHOR_TL, ANCHOR_BR, ANCHOR_BL } anchor_kind;

typedef struct {
    uint32_t default_timeout_ms; /* expire_timeout == -1    */
    int max_visible;             /* cards per stack         */
    int card_width;              /* logical px              */
    int font_size;               /* px; 0 = the UI metrics   */
    int edge_margin;             /* gap from screen edge    */
    int card_gap;                /* gap between cards       */
    anchor_kind anchor;
    int output_index;            /* -1 = every output       */
    int replace;
    int wait;
    double quit_after_s;         /* <=0 = run forever (rigs)*/
} options;

/* ---- the notification record ------------------------------------------- */

typedef struct {
    char key[MAX_ACTION_KEY];
    char label[MAX_ACTION_LABEL];
} note_action;

typedef struct note {
    struct note *next; /* newest first                                */
    struct note *prev;

    uint32_t id;

    /* Sanitized content, parsed ONCE into FDK span form. summary is
     * escaped-plain (the spec allows markup in the BODY only),
     * wrapped in <b> for its bold rendering; body is translated
     * notification markup -> FDK markup before parsing. */
    char *sum_plain;
    fdk_span *sum_spans;
    size_t sum_span_count;
    char *body_plain; /* NULL when the body was empty               */
    fdk_span *body_spans;
    size_t body_span_count;

    uint8_t urgency; /* 0 low, 1 normal, 2 critical                 */
    int transient_hint;
    int resident_hint;

    note_action actions[MAX_ACTION_BUTTONS];
    int action_count;
    int has_default; /* a "default" action key was supplied         */

    fdk_surface *icon; /* owned ARGB8888, NULL when none            */

    fdk_timer *timer; /* expire timer, NULL when never              */

    /* The card. */
    fdk_shell_layer *layer;
    int visible;
    int card_w, card_h;
    int margin_y;   /* current offset from the anchor edge          */
    int dirty;      /* content changed, repaint                     */
    int recreate_count; /* bounded self-healing after output churn  */

    /* Pointer interaction state. */
    int hover_btn;  /* -1 none, 0.. action index                    */
    int press_kind; /* -1 none, -2 body, >=0 action index           */
} note;

/* ---- app state ----------------------------------------------------------- */

typedef struct {
    fdk_context *ctx;
    fdk_shell *shell;
    fdk_dbus_connection *bus;
    fdk_font *font;

    options opt;

    note *head; /* doubly linked, newest first                      */
    int live_count;

    uint32_t next_id;

    int serving;     /* we own the bus name                         */
    int stopping;    /* teardown requested                          */
    int exit_code;
    const char *stop_reason;

    /* Flood window. */
    uint64_t rate_times[RATE_MAX + 1];
    int rate_len;
} notifyd;

static notifyd g_app;

static volatile sig_atomic_t g_signal_stop;

/* ---- forward declarations (the event web is a cycle) ------------------- */

static void relayout(notifyd *app);
static void paint_note(notifyd *app, note *n);
static void close_note(notifyd *app, note *n, uint32_t reason,
                       int emit);
static void note_layer_event(fdk_shell_layer *layer,
                             const fdk_event_data *ev, void *user);
static void request_name_reply(fdk_dbus_connection *bus,
                               fdk_dbus_message *reply, void *user);
static void incumbent_reply(fdk_dbus_connection *bus,
                            fdk_dbus_message *reply, void *user);

/* ---- small helpers ------------------------------------------------------ */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *reason_name(uint32_t reason) {
    switch (reason) {
    case 1: return "expired";
    case 2: return "dismissed by the user";
    case 3: return "CloseNotification";
    default: return "undefined/reserved";
    }
}

/* Truncate a UTF-8 string to at most max bytes, cutting at a
 * codepoint boundary and appending an ellipsis when cut. Returns a
 * freshly allocated string (caller frees). */
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

/* ---- body markup sanitizer ------------------------------------------------
 *
 * The spec's allowed body subset is <b> <i> <u> <a href> <img>, plus
 * the XML entities. FDK's markup vocabulary is <b> <i> <u> <s>
 * <color=#..>; everything unknown is LITERAL TEXT there, so this
 * translator maps notification markup onto it and strips what
 * doesn't map:
 *
 *   <b> <i> <u>            -> identical FDK tags
 *   <a href="..."> </a>    -> <u><color=#LINK> text </color></u>
 *                             (the href is DISCARDED — links are
 *                              drawn, never followed, never fetched)
 *   <img .../>              -> nothing (no body-images capability;
 *                              the src is never opened)
 *   any other tag           -> the markers are stripped, the inner
 *                              text survives ("filter them out")
 *   &amp; &lt; &gt; &quot; &apos; -> passed through (FDK decodes)
 *   &nbsp;                  -> U+00A0
 *   &#NNN; &#xHH;           -> decoded when a valid, non-control
 *                              scalar value; else literal
 *   other entities          -> literal text (visible, harmless)
 *   other ASCII controls    -> dropped (\n \t survive)
 *
 * SECURITY POSTURE: nothing in the output is ever executed,
 * opened, fetched or followed; the worst a hostile body can do is
 * paint text in a color. The output is capped at MAX_BODY_BYTES.
 */

typedef struct {
    char *buf;
    size_t len, cap;
} sbuf;

static int sb_put(sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 256;
        while (want < b->len + n + 1) {
            want *= 2;
        }
        char *nb = realloc(b->buf, want);
        if (nb == NULL) {
            return 0;
        }
        b->buf = nb;
        b->cap = want;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return 1;
}

static int sb_puts(sbuf *b, const char *s) {
    return sb_put(b, s, strlen(s));
}

/* Case-insensitive tag-name match at `p` (which points just past
 * '<'). Returns the length of the tag name, or 0 when `name`
 * doesn't match. */
static size_t tag_is(const char *p, const char *name) {
    size_t n = strlen(name);
    if (strncasecmp(p, name, n) != 0) {
        return 0;
    }
    char after = p[n];
    if (after == '\0' || after == '>' || after == '/' ||
        after == ' ' || after == '\t' || after == '\n' ||
        after == '\r') {
        return n;
    }
    return 0;
}

/* Skips from an opening '<' past the matching '>' — quote-aware, so
 * an attribute containing '>' cannot push the scanner off the rail.
 * Returns the index JUST PAST '>', or end (strlen) when unterminated. */
static size_t skip_tag(const char *s, size_t i) {
    char quote = 0;
    for (; s[i] != '\0'; i++) {
        char c = s[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '>') {
            return i + 1;
        }
    }
    return i;
}

static void sb_put_utf8(sbuf *b, uint32_t cp) {
    char tmp[4];
    if (cp < 0x80u) {
        tmp[0] = (char)cp;
        sb_put(b, tmp, 1);
    } else if (cp < 0x800u) {
        tmp[0] = (char)(0xC0u | (cp >> 6));
        tmp[1] = (char)(0x80u | (cp & 0x3Fu));
        sb_put(b, tmp, 2);
    } else if (cp < 0x10000u) {
        tmp[0] = (char)(0xE0u | (cp >> 12));
        tmp[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[2] = (char)(0x80u | (cp & 0x3Fu));
        sb_put(b, tmp, 3);
    } else {
        tmp[0] = (char)(0xF0u | (cp >> 18));
        tmp[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        tmp[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[3] = (char)(0x80u | (cp & 0x3Fu));
        sb_put(b, tmp, 4);
    }
}

/* Decodes a numeric entity at &s[*io] (already past "&#"). Advances
 * *io past the terminating ';' when well-formed. */
static void put_numeric_entity(sbuf *out, const char *s, size_t *io) {
    size_t i = *io;
    int hex = (s[i] == 'x' || s[i] == 'X');
    if (hex) {
        i++;
    }
    uint64_t v = 0;
    size_t digits = 0;
    for (; s[i] != '\0'; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a') + 10u;
        } else if (hex && c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A') + 10u;
        } else {
            break;
        }
        v = v * (hex ? 16u : 10u) + d;
        digits++;
        if (v > 0x10FFFFu || digits > 8) {
            return; /* absurd — emit nothing (dropped) */
        }
    }
    if (s[i] != ';' || digits == 0) {
        return; /* not an entity after all; the caller's '&' is
                 * handled by the default literal path */
    }
    *io = i + 1;
    /* Reject surrogates, noncharacters-in-practice, and controls
     * (C0, DEL, C1): notification bodies are for reading. */
    if (v >= 0xD800u && v <= 0xDFFFu) {
        return;
    }
    if (v < 0x20u || v == 0x7Fu || (v >= 0x80u && v <= 0x9Fu)) {
        return;
    }
    sb_put_utf8(out, (uint32_t)v);
}

/* The translator itself. `link_color` is "#RRGGBB". Returns a
 * freshly allocated FDK-markup string (caller frees), or NULL on
 * allocation failure. */
static char *sanitize_body_markup(const char *body, const char *link_color) {
    sbuf out = {0};
    size_t i = 0;
    while (body[i] != '\0') {
        char c = body[i];
        if (c == '<') {
            const char *p = body + i + 1;
            int closing = (*p == '/');
            if (closing) {
                p++;
            }
            size_t t;
            if ((t = tag_is(p, "b")) != 0 || (t = tag_is(p, "i")) != 0 ||
                (t = tag_is(p, "u")) != 0) {
                char name = p[t - 1]; /* 'b' | 'i' | 'u' */
                char emit[6];
                size_t e = 0;
                emit[e++] = '<';
                if (closing) {
                    emit[e++] = '/';
                }
                emit[e++] = name;
                emit[e++] = '>';
                emit[e] = '\0';
                if (!sb_put(&out, emit, e)) {
                    goto oom;
                }
                i = skip_tag(body, i);
                continue;
            }
            if ((t = tag_is(p, "a")) != 0) {
                if (!closing) {
                    if (!sb_puts(&out, "<u><color=") ||
                        !sb_puts(&out, link_color) ||
                        !sb_puts(&out, ">")) {
                        goto oom;
                    }
                } else {
                    if (!sb_puts(&out, "</color></u>")) {
                        goto oom;
                    }
                }
                i = skip_tag(body, i);
                continue;
            }
            if ((t = tag_is(p, "img")) != 0) {
                /* Stripped entirely — the src is never opened. */
                i = skip_tag(body, i);
                continue;
            }
            /* Unknown tag: strip the MARKER only (its inner text
             * survives — "filter them out" without losing content). */
            i = skip_tag(body, i);
            continue;
        }
        if (c == '&') {
            const char *rest = body + i + 1;
            if (rest[0] == '#') {
                size_t adv = i + 1;
                put_numeric_entity(&out, body, &adv);
                if (adv != i + 1) {
                    i = adv;
                    continue;
                }
                /* Malformed numeric: drop the '&' literally? Keep it
                 * visible — it is honest text. */
                if (!sb_put(&out, "&", 1)) {
                    goto oom;
                }
                i++;
                continue;
            }
            if (strncmp(rest, "amp;", 4) == 0 ||
                strncmp(rest, "lt;", 3) == 0 ||
                strncmp(rest, "gt;", 3) == 0 ||
                strncmp(rest, "quot;", 5) == 0 ||
                strncmp(rest, "apos;", 5) == 0) {
                /* FDK's markup decodes these itself — pass through. */
                if (!sb_put(&out, "&", 1)) {
                    goto oom;
                }
                i++;
                continue;
            }
            if (strncmp(rest, "nbsp;", 5) == 0) {
                sb_put_utf8(&out, 0x00A0u);
                i += 6;
                continue;
            }
            /* Unknown entity: literal text (FDK's own behavior). */
            if (!sb_put(&out, "&", 1)) {
                goto oom;
            }
            i++;
            continue;
        }
        if ((unsigned char)c < 0x20u && c != '\n' && c != '\t') {
            i++; /* hostile control character: dropped */
            continue;
        }
        if (!sb_put(&out, &c, 1)) {
            goto oom;
        }
        i++;
    }
    if (out.buf == NULL) {
        out.buf = strdup("");
        return out.buf;
    }
    return out.buf;
oom:
    free(out.buf);
    return NULL;
}

/* Escapes a PLAIN string (the summary — markup is body-only per the
 * specification) for FDK's markup scanner and wraps it bold. */
static char *plain_to_bold_markup(const char *s) {
    sbuf out = {0};
    if (!sb_puts(&out, "<b>")) {
        goto oom;
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if ((unsigned char)c < 0x20u && c != '\n' && c != '\t') {
            continue; /* controls dropped, same policy as bodies */
        }
        if (c == '&') {
            if (!sb_puts(&out, "&amp;")) {
                goto oom;
            }
            continue;
        }
        if (c == '<') {
            if (!sb_puts(&out, "&lt;")) {
                goto oom;
            }
            continue;
        }
        if (!sb_put(&out, &c, 1)) {
            goto oom;
        }
    }
    if (!sb_puts(&out, "</b>")) {
        goto oom;
    }
    return out.buf;
oom:
    free(out.buf);
    return NULL;
}

/* ---- icon decoding -------------------------------------------------------
 *
 * Priority per the specification (a single displayed image):
 * image-data > image-path > app_icon > icon_data, with the
 * deprecated underscore spellings accepted as their equals. A
 * file:// URI or absolute path decodes through the bounded image
 * loader; anything else is a themed icon NAME through the engine.
 * Every field of the raw struct is validated BEFORE any allocation
 * sized by it — width*height overflow is checked in 64-bit.
 */


/* A byte from an 'ay' value: the FLAT form first (every array that
 * arrived on the wire is flat since the value model grew the bulk
 * form), the tree form second (hand-built values). */
static uint8_t ay_byte(const fdk_dbus_value *ay, size_t index) {
    size_t n = 0;
    const uint8_t *flat = fdk_dbus_value_array_flat_bytes(ay, &n);
    if (flat != NULL) {
        return index < n ? flat[index] : 0;
    }
    return fdk_dbus_value_byte(fdk_dbus_value_array_item(ay, index));
}

/* Decodes an (iiibiiay) hint value into an owned ARGB surface, or
 * NULL when the struct is malformed / oversized (logged at trace). */
static fdk_surface *icon_from_image_data(const fdk_dbus_value *v) {
    if (v == NULL || fdk_dbus_value_type(v) != FDK_DBUS_STRUCT ||
        fdk_dbus_value_struct_count(v) != 7) {
        tracef("image-data: wrong shape (not a 7-field struct)");
        return NULL;
    }
    const fdk_dbus_value *w = fdk_dbus_value_struct_item(v, 0);
    const fdk_dbus_value *h = fdk_dbus_value_struct_item(v, 1);
    const fdk_dbus_value *rs = fdk_dbus_value_struct_item(v, 2);
    const fdk_dbus_value *alpha = fdk_dbus_value_struct_item(v, 3);
    const fdk_dbus_value *bps = fdk_dbus_value_struct_item(v, 4);
    const fdk_dbus_value *ch = fdk_dbus_value_struct_item(v, 5);
    const fdk_dbus_value *data = fdk_dbus_value_struct_item(v, 6);

    if (fdk_dbus_value_type(w) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(h) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(rs) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(alpha) != FDK_DBUS_BOOLEAN ||
        fdk_dbus_value_type(bps) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(ch) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(data) != FDK_DBUS_ARRAY) {
        tracef("image-data: wrong field types");
        return NULL;
    }
    /* The spec's array is BYTEs. Some tooling's text-form clients
     * (gvariant parsers inferring bare number lists) send int32
     * arrays instead -- accept the struct shape but refuse the
     * wrong element type: reading ints as bytes would silently
     * yield an all-zero (fully transparent) icon. */
    {
        const char *elem = fdk_dbus_value_array_signature(data);
        if (elem == NULL || elem[0] != 'y' || elem[1] != '\0') {
            tracef("image-data: element signature '%s' is not \"y\"",
                   elem ? elem : "?");
            return NULL;
        }
    }
    int32_t width = fdk_dbus_value_int32(w);
    int32_t height = fdk_dbus_value_int32(h);
    int32_t rowstride = fdk_dbus_value_int32(rs);
    int32_t channels = fdk_dbus_value_int32(ch);
    int32_t bits = fdk_dbus_value_int32(bps);

    if (width <= 0 || height <= 0 || width > MAX_ICON_DIM ||
        height > MAX_ICON_DIM) {
        tracef("image-data: geometry %dx%d rejected (cap %d)",
               width, height, MAX_ICON_DIM);
        return NULL;
    }
    if (bits != 8) { /* the spec: must always be 8 */
        tracef("image-data: bits_per_sample %d rejected", bits);
        return NULL;
    }
    if (channels != 3 && channels != 4) {
        tracef("image-data: channels %d rejected", channels);
        return NULL;
    }
    if (rowstride < (int64_t)width * channels) {
        tracef("image-data: rowstride %d < width*channels", rowstride);
        return NULL;
    }
    size_t count = fdk_dbus_value_array_count(data);
    /* The last row needs only width*channels bytes. */
    int64_t needed = (int64_t)rowstride * (height - 1) +
                     (int64_t)width * channels;
    if ((int64_t)count < needed) {
        tracef("image-data: %zu bytes < %lld needed", count,
               (long long)needed);
        return NULL;
    }
    if (needed > MAX_ICON_BYTES) {
        tracef("image-data: %lld bytes over cap", (long long)needed);
        return NULL;
    }

    fdk_surface *surf = NULL;
    if (!fdk_ok(fdk_surface_create_format(width, height,
                                          FDK_SURFACE_FORMAT_ARGB8888,
                                          &surf))) {
        return NULL;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surf, &info))) {
        fdk_surface_destroy(surf);
        return NULL;
    }
    int alpha_present = channels == 4;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t off = (size_t)y * (size_t)rowstride +
                         (size_t)x * (size_t)channels;
            uint32_t r = ay_byte(data, off);
            uint32_t g = ay_byte(data, off + 1);
            uint32_t b = ay_byte(data, off + 2);
            uint32_t a = 0xFFu;
            if (alpha_present) {
                a = ay_byte(data, off + 3);
            }
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    tracef("image-data: decoded %dx%d (%s)", width, height,
           alpha_present ? "RGBA" : "RGB forced-opaque");
    return surf;
}

/* A path or file:// URI, through the bounded image loader. */
static fdk_surface *icon_from_file(const char *spec) {
    if (spec == NULL || spec[0] == '\0') {
        return NULL;
    }
    char path[1024];
    if (strncmp(spec, "file://", 7) == 0) {
        /* Percent-decoding for the file URI is NOT done: the loader
         * would need the decoded path; a plain copy keeps the bound
         * honest (spaces in paths arrive as %20 from some clients —
         * those simply miss, and the daemon draws no icon). */
        snprintf(path, sizeof(path), "%s", spec + 7);
    } else {
        snprintf(path, sizeof(path), "%s", spec);
    }
    if (path[0] != '/') {
        return NULL; /* not a path — a themed NAME (caller's job) */
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_surface_create_from_image(path, &s))) {
        tracef("image-path: '%s' did not decode", path);
        return NULL;
    }
    fdk_surface_info info;
    if (fdk_ok(fdk_surface_get_info(s, &info)) &&
        (info.width > MAX_ICON_DIM || info.height > MAX_ICON_DIM)) {
        tracef("image-path: %dx%d over cap — dropped", info.width,
               info.height);
        fdk_surface_destroy(s);
        return NULL;
    }
    return s;
}

/* Resolves the icon for one notification, applying the spec's
 * priority order. `hints` is the a{sv} array value. */
static fdk_surface *resolve_icon(const char *app_icon,
                                 const fdk_dbus_value *hints) {
    const fdk_dbus_value *img = fdk_dbus_value_dict_lookup(hints,
                                                           "image-data");
    if (img == NULL) {
        img = fdk_dbus_value_dict_lookup(hints, "image_data");
    }
    if (img != NULL) {
        const fdk_dbus_value *inner =
            fdk_dbus_value_type(img) == FDK_DBUS_VARIANT
                ? fdk_dbus_value_variant_value(img)
                : img;
        fdk_surface *s = icon_from_image_data(inner);
        if (s != NULL) {
            return s;
        }
    }
    const char *path_hint = NULL;
    const fdk_dbus_value *pv = fdk_dbus_value_dict_lookup(hints,
                                                          "image-path");
    if (pv == NULL) {
        pv = fdk_dbus_value_dict_lookup(hints, "image_path");
    }
    if (pv != NULL) {
        const fdk_dbus_value *inner =
            fdk_dbus_value_type(pv) == FDK_DBUS_VARIANT
                ? fdk_dbus_value_variant_value(pv)
                : pv;
        if (fdk_dbus_value_type(inner) == FDK_DBUS_STRING) {
            path_hint = fdk_dbus_value_string(inner);
        }
    }
    if (path_hint != NULL && path_hint[0] != '\0') {
        fdk_surface *s = icon_from_file(path_hint);
        if (s != NULL) {
            return s;
        }
        /* A name that came through image-path is still a NAME. */
        fdk_surface *t = fdk_icon_theme_surface(path_hint, g_ui.icon_box);
        if (t != NULL) {
            return t;
        }
    }
    if (app_icon != NULL && app_icon[0] != '\0') {
        if (app_icon[0] == '/' || strncmp(app_icon, "file://", 7) == 0) {
            return icon_from_file(app_icon);
        }
        return fdk_icon_theme_surface(app_icon, g_ui.icon_box);
    }
    /* The deprecated icon_data hint is the last resort (the spec's
     * compatibility order). */
    const fdk_dbus_value *legacy = fdk_dbus_value_dict_lookup(hints,
                                                              "icon_data");
    if (legacy != NULL) {
        const fdk_dbus_value *inner =
            fdk_dbus_value_type(legacy) == FDK_DBUS_VARIANT
                ? fdk_dbus_value_variant_value(legacy)
                : legacy;
        return icon_from_image_data(inner);
    }
    return NULL;
}

/* ---- card painting ------------------------------------------------------- */

static fdk_color tk_color(fdk_theme_token token, fdk_color fallback) {
    fdk_color c = fdk_theme_get_color(NULL, token);
    if (c.a <= 0.0f) { /* unset tokens read black-transparent-safe? no:
                        * get_color returns the theme's value; a fully
                        * transparent result means "no sane value" */
        return fallback;
    }
    (void)fallback;
    return c;
}

static fdk_color with_alpha(fdk_color c, float a) {
    c.a = a;
    return c;
}

/* Paints one notification card onto its layer surface. */
static void paint_note(notifyd *app, note *n) {
    if (n->layer == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_layer_get_surface(n->layer, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    int w = info.width;
    int h = info.height;

    fdk_font_metrics fm;
    fdk_font_get_metrics(app->font, &fm);

    fdk_color bg = tk_color(FDK_TK_TOOLTIP_BACKGROUND,
                            (fdk_color){ .r = 0.10f, .g = 0.11f,
                                         .b = 0.14f, .a = 1.0f });
    fdk_color fg = tk_color(FDK_TK_TOOLTIP_TEXT,
                            (fdk_color){ .r = 0.95f, .g = 0.95f,
                                         .b = 0.95f, .a = 1.0f });
    fdk_color accent = tk_color(FDK_TK_ACCENT,
                                (fdk_color){ .r = 0.56f, .g = 0.47f,
                                             .b = 0.85f, .a = 1.0f });
    fdk_color danger = tk_color(FDK_TK_DANGER,
                                (fdk_color){ .r = 0.85f, .g = 0.33f,
                                             .b = 0.31f, .a = 1.0f });
    fdk_color link = tk_color(FDK_TK_LINK,
                              (fdk_color){ .r = 0.31f, .g = 0.76f,
                                           .b = 0.97f, .a = 1.0f });
    fdk_color btn_bg = tk_color(FDK_TK_CONTROL_BACKGROUND,
                                (fdk_color){ .r = 0.18f, .g = 0.20f,
                                             .b = 0.25f, .a = 1.0f });
    fdk_color btn_hover = tk_color(FDK_TK_CONTROL_BACKGROUND_HOVER,
                                   (fdk_color){ .r = 0.26f, .g = 0.29f,
                                                .b = 0.36f, .a = 1.0f });

    /* The card body: rounded, on transparent surroundings (the
     * layer is sized EXACTLY to the card, so the only transparent
     * pixels are the corners' rounding — the honest click-through
     * limit until S-4's input regions). */
    fdk_surface_fill_rounded_rect(
        s, (fdk_rect){ .x = 0, .y = 0, .width = w, .height = h },
        g_ui.radius, bg);

    /* The urgency strip along the left edge (dunst/mako convention):
     * normal = accent, critical = danger, low = none. */
    if (n->urgency > 0) {
        fdk_color strip = n->urgency >= 2 ? danger : accent;
        fdk_surface_fill_rect(
            s, (fdk_rect){ .x = 0,
                           .y = g_ui.radius,
                           .width = g_ui.strip_w,
                           .height = h - 2 * g_ui.radius },
            strip);
    }

    /* Layout: [pad][icon? ICON_BOX][gap]text column[pad]. */
    int text_x = g_ui.pad_x;
    if (n->icon != NULL) {
        text_x += g_ui.icon_box + g_ui.icon_gap;
    }
    int text_w = w - text_x - g_ui.pad_x;
    if (text_w < 40) {
        text_w = 40;
    }

    /* Summary: single line, ellipsized, bold (its spans carry it). */
    int y = g_ui.pad_y;
    if (n->sum_plain != NULL) {
        size_t sum_len = strlen(n->sum_plain);
        size_t draw_len = sum_len;
        bool fits = true;
        if (sum_len > 0) {
            fdk_font_ellipsize_spans_utf8(app->font, n->sum_plain,
                                          sum_len, n->sum_spans,
                                          n->sum_span_count, text_w,
                                          &draw_len, &fits);
        }
        (void)fits;
        fdk_surface_draw_spans_utf8(s, app->font, n->sum_plain, draw_len,
                                    n->sum_spans, n->sum_span_count,
                                    text_x, y + fm.ascent, fg);
    }

    /* Body: wrapped to MAX_BODY_LINES, ellipsized when truncated. */
    if (n->body_plain != NULL && n->body_plain[0] != '\0') {
        size_t body_len = strlen(n->body_plain);
        size_t line_count = 0;
        bool truncated = false;
        fdk_font_break_lines_spans_utf8(app->font, n->body_plain,
                                        body_len, n->body_spans,
                                        n->body_span_count, text_w,
                                        NULL, 0, &line_count,
                                        &truncated);
        fdk_text_line lines[MAX_BODY_LINES];
        size_t got = 0;
        if (line_count > 0) {
            fdk_font_break_lines_spans_utf8(
                app->font, n->body_plain, body_len, n->body_spans,
                n->body_span_count, text_w, lines, MAX_BODY_LINES,
                &got, NULL);
        } else {
            got = 0;
        }
        int by = y + fm.line_height + g_ui.text_gap;
        for (size_t li = 0; li < got; li++) {
            fdk_surface_draw_spans_utf8(
                s, app->font, n->body_plain + lines[li].byte_offset,
                lines[li].byte_len, n->body_spans,
                n->body_span_count, text_x, by + fm.ascent, fg);
            by += fm.line_height;
        }
        if (truncated || line_count > MAX_BODY_LINES) {
            fdk_surface_draw_spans_utf8(
                s, app->font, "\xE2\x80\xA6", 3, NULL, 0, text_x,
                by + fm.ascent, fg);
        }
    }

    /* The icon, centered in its box (scaled down when oversized). */
    if (n->icon != NULL) {
        fdk_surface_info ii;
        if (fdk_ok(fdk_surface_get_info(n->icon, &ii))) {
            int bx = g_ui.pad_x, by = g_ui.pad_y;
            if (ii.width <= g_ui.icon_box &&
                ii.height <= g_ui.icon_box) {
                fdk_surface_blit_blend(
                    s, bx + (g_ui.icon_box - ii.width) / 2,
                    by + (g_ui.icon_box - ii.height) / 2, n->icon,
                    (fdk_rect){ .x = 0, .y = 0, .width = ii.width,
                                .height = ii.height });
            } else {
                double scale = (double)g_ui.icon_box /
                               (ii.width > ii.height ? ii.width
                                                     : ii.height);
                fdk_i32 dw = (fdk_i32)(ii.width * scale + 0.5);
                fdk_i32 dh = (fdk_i32)(ii.height * scale + 0.5);
                fdk_matrix m = { .m00 = (float)scale, .m01 = 0.0f,
                                 .m10 = 0.0f, .m11 = (float)scale,
                                 .tx = (float)(bx + (g_ui.icon_box - dw) / 2),
                                 .ty = (float)(by + (g_ui.icon_box - dh) / 2) };
                fdk_surface_blit_transformed(s, m, n->icon);
            }
        }
    }

    /* Action buttons along the bottom. */
    if (n->action_count > 0) {
        int bx = text_x;
        int btop = h - g_ui.pad_y - g_ui.button_h;
        for (int i = 0; i < n->action_count; i++) {
            char label[MAX_ACTION_LABEL + 8];
            snprintf(label, sizeof(label), " %s ",
                     n->actions[i].label);
            fdk_text_metrics tm;
            fdk_font_measure_utf8(app->font, label, strlen(label),
                                  &tm);
            int bw = tm.advance_width + g_ui.button_pad;
            fdk_color fill = n->hover_btn == i ? btn_hover : btn_bg;
            if (n->press_kind == i) {
                fill = accent;
            }
            fdk_surface_fill_rounded_rect(
                s, (fdk_rect){ .x = bx, .y = btop, .width = bw,
                               .height = g_ui.button_h },
                g_ui.button_r, fill);
            int tx = bx + g_ui.button_pad / 2;
            int ty = btop + (g_ui.button_h - fm.line_height) / 2 +
                    fm.ascent;
            fdk_surface_draw_utf8(s, app->font, label, strlen(label),
                                  tx, ty,
                                  n->press_kind == i
                                      ? tk_color(FDK_TK_ACCENT_TEXT,
                                                 (fdk_color){ .r = 1.0f,
                                                              .g = 1.0f,
                                                              .b = 1.0f,
                                                              .a = 1.0f })
                                      : fg);
            bx += bw + g_ui.button_gap;
        }
    }

    (void)link; /* links carry their color through their spans' */
                /* <color=...> translation (sanitize_body_markup)  */
    (void)with_alpha;
    (void)btn_hover;
    n->dirty = 0;
    (void)fdk_surface_present(s);
}

/* ---- the stack ---------------------------------------------------------- */

static void note_destroy_layer(note *n) {
    if (n->layer != NULL) {
        fdk_shell_layer_destroy(n->layer);
        n->layer = NULL;
    }
    n->visible = 0;
}

/* Computes the card's pixel size from its content (called once per
 * content change; the font's metrics drive every height). */
static void note_measure(notifyd *app, note *n) {
    fdk_font_metrics fm;
    fdk_font_get_metrics(app->font, &fm);
    int text_w = app->opt.card_width - 2 * g_ui.pad_x -
                 (n->icon != NULL ? g_ui.icon_box + g_ui.icon_gap : 0);
    if (text_w < 40) {
        text_w = 40;
    }

    int text_h = 0;
    if (n->sum_plain != NULL && n->sum_plain[0] != '\0') {
        text_h += fm.line_height;
    }
    if (n->body_plain != NULL && n->body_plain[0] != '\0') {
        size_t line_count = 0;
        fdk_font_break_lines_spans_utf8(app->font, n->body_plain,
                                        strlen(n->body_plain),
                                        n->body_spans,
                                        n->body_span_count, text_w,
                                        NULL, 0, &line_count, NULL);
        if (line_count > MAX_BODY_LINES) {
            line_count = MAX_BODY_LINES + 1; /* + the ellipsis line */
        }
        if (line_count == 0) {
            line_count = 1; /* an explicit newline-only body */
        }
        text_h += g_ui.text_gap + (int)line_count * fm.line_height;
    }
    int content_h = text_h;
    if (n->icon != NULL && content_h < g_ui.icon_box) {
        content_h = g_ui.icon_box;
    }
    int h = g_ui.pad_y + content_h + g_ui.pad_y;
    if (n->action_count > 0) {
        h += g_ui.button_h + g_ui.button_gap;
    }
    if (h < g_ui.pad_y * 2 + fm.line_height) {
        h = g_ui.pad_y * 2 + fm.line_height; /* never a sliver */
    }
    n->card_w = app->opt.card_width;
    n->card_h = h;
    n->dirty = 1;
}

/* Creates (or re-creates) the note's layer at its current margin. */
static int note_create_layer(notifyd *app, note *n, int margin_y) {
    note_destroy_layer(n);
    fdk_shell_layer_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.layer = FDK_SHELL_LAYER_OVERLAY;
    switch (app->opt.anchor) {
    case ANCHOR_TR:
        d.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_RIGHT;
        d.margin_top = app->opt.edge_margin + margin_y;
        d.margin_right = app->opt.edge_margin;
        break;
    case ANCHOR_TL:
        d.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT;
        d.margin_top = app->opt.edge_margin + margin_y;
        d.margin_left = app->opt.edge_margin;
        break;
    case ANCHOR_BR:
        d.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_RIGHT;
        d.margin_bottom = app->opt.edge_margin + margin_y;
        d.margin_right = app->opt.edge_margin;
        break;
    case ANCHOR_BL:
    default:
        d.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_LEFT;
        d.margin_bottom = app->opt.edge_margin + margin_y;
        d.margin_left = app->opt.edge_margin;
        break;
    }
    d.exclusive_zone = 0; /* notifications reserve nothing */
    d.keyboard = FDK_SHELL_KBD_NONE;
    d.width = n->card_w;
    d.height = n->card_h;
    d.output_index = app->opt.output_index;
    snprintf(d.namespace_, sizeof(d.namespace_), "notifyd");

    fdk_result r = fdk_shell_layer_create(app->shell, &d, &n->layer);
    if (!fdk_ok(r)) {
        tracef("place id=%u FAILED layer create (%d)", n->id, r);
        return 0;
    }
    n->margin_y = margin_y;
    n->visible = 1;
    fdk_shell_layer_set_event_callback(n->layer, note_layer_event, n);
    fdk_shell_layer_show(n->layer);
    paint_note(app, n);
    tracef("place id=%u anchor=%s margin=%d size=%dx%d",
           n->id,
           app->opt.anchor == ANCHOR_TR ? "top-right"
           : app->opt.anchor == ANCHOR_TL ? "top-left"
           : app->opt.anchor == ANCHOR_BR ? "bottom-right"
                                          : "bottom-left",
           margin_y, n->card_w, n->card_h);
    return 1;
}

/* Walks the visible stack top-down, assigning each note its layer
 * position; only notes whose (margin, size, dirty) changed are
 * touched. Called after every add/remove/replace. */
static void relayout(notifyd *app) {
    int y = 0;
    for (note *n = app->head; n != NULL; n = n->next) {
        /* EVERY live note gets a card — the guard this replaced
         * (visible || layer) skipped BRAND-NEW notes forever: no
         * layer had ever been created for them, so notifications
         * served headlessly correct D-Bus and rendered nothing.
         * It also defeated the output-removal self-heal (a card
         * whose layer died never came back). */
        int want = y;
        if (n->layer != NULL && n->margin_y == want && !n->dirty) {
            /* unchanged: stays exactly where it was */
        } else if (n->layer != NULL && n->margin_y == want &&
                   n->dirty) {
            paint_note(app, n);
        } else {
            note_create_layer(app, n, want);
        }
        y += n->card_h + app->opt.card_gap;
    }
}

/* ---- signals out -------------------------------------------------------- */

static void emit_notification_closed(notifyd *app, uint32_t id,
                                     uint32_t reason) {
    if (app->bus == NULL) {
        return;
    }
    fdk_dbus_message *m = fdk_dbus_message_new_signal(
        N_PATH, N_IFACE, "NotificationClosed");
    if (m == NULL) {
        return;
    }
    if (fdk_ok(fdk_dbus_message_append_uint32(m, id)) &&
        fdk_ok(fdk_dbus_message_append_uint32(m, reason))) {
        fdk_dbus_connection_send(app->bus, m);
        tracef("signal NotificationClosed id=%u reason=%u (%s)", id,
               reason, reason_name(reason));
    } else {
        fdk_dbus_message_free(m);
    }
}

static void emit_action_invoked(notifyd *app, uint32_t id,
                                const char *action_key) {
    if (app->bus == NULL) {
        return;
    }
    fdk_dbus_message *m = fdk_dbus_message_new_signal(
        N_PATH, N_IFACE, "ActionInvoked");
    if (m == NULL) {
        return;
    }
    if (fdk_ok(fdk_dbus_message_append_uint32(m, id)) &&
        fdk_ok(fdk_dbus_message_append_string(m, action_key))) {
        fdk_dbus_connection_send(app->bus, m);
        tracef("signal ActionInvoked id=%u key='%s'", id, action_key);
    } else {
        fdk_dbus_message_free(m);
    }
}

/* ---- closing ------------------------------------------------------------- */

/* Removes a note from the live set (invalidating its id BEFORE any
 * signal, per the specification) and emits NotificationClosed. */
static void close_note(notifyd *app, note *n, uint32_t reason,
                       int emit) {
    if (n->prev != NULL) {
        n->prev->next = n->next;
    } else {
        app->head = n->next;
    }
    if (n->next != NULL) {
        n->next->prev = n->prev;
    }
    app->live_count--;

    if (n->timer != NULL) {
        fdk_timer_remove(n->timer);
        n->timer = NULL;
    }
    note_destroy_layer(n);
    if (n->icon != NULL) {
        fdk_surface_destroy(n->icon);
        n->icon = NULL;
    }
    tracef("close id=%u reason=%u (%s)", n->id, reason,
           reason_name(reason));
    uint32_t id = n->id;
    fdk_markup_free(n->sum_plain, n->sum_spans);
    fdk_markup_free(n->body_plain, n->body_spans);
    free(n);
    if (emit) {
        emit_notification_closed(app, id, reason);
    }
}

/* Force-expires the OLDEST dismissible note (flood / live-cap
 * relief). Critical notes are protected unless nothing else is. */
static note *pick_overflow_victim(notifyd *app) {
    note *last = NULL;
    for (note *n = app->head; n != NULL; n = n->next) {
        last = n;
    }
    for (note *n = last; n != NULL; n = n->prev) {
        if (n->urgency < 2) {
            return n;
        }
    }
    return last; /* all critical: the oldest goes anyway */
}

/* ---- expire timer -------------------------------------------------------- */

static void expire_timer_fn(fdk_timer *timer, void *user) {
    (void)timer;
    note *n = user;
    notifyd *app = &g_app;
    close_note(app, n, 1, 1);
    relayout(app);
}

/* ---- the layer's event callback ------------------------------------------ */

static int hit_button(notifyd *app, note *n, float x, float y) {
    if (n->action_count == 0) {
        return -1;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(app->font, &fm);
    int btop = n->card_h - g_ui.pad_y - g_ui.button_h;
    if (y < (float)btop ||
        y > (float)(btop + g_ui.button_h)) {
        return -1;
    }
    int bx = g_ui.pad_x +
             (n->icon != NULL ? g_ui.icon_box + g_ui.icon_gap : 0);
    for (int i = 0; i < n->action_count; i++) {
        char label[MAX_ACTION_LABEL + 8];
        snprintf(label, sizeof(label), " %s ", n->actions[i].label);
        fdk_text_metrics tm;
        fdk_font_measure_utf8(app->font, label, strlen(label), &tm);
        int bw = tm.advance_width + g_ui.button_pad;
        if (x >= (float)bx && x < (float)(bx + bw)) {
            return i;
        }
        bx += bw + g_ui.button_gap;
    }
    return -1;
}

static void note_layer_event(fdk_shell_layer *layer,
                             const fdk_event_data *ev, void *user) {
    note *n = user;
    notifyd *app = &g_app;
    (void)layer;
    switch (ev->type) {
    case FDK_EVENT_WINDOW_CONFIGURE:
        paint_note(app, n); /* the compositor sized us: repaint */
        break;
    case FDK_EVENT_WINDOW_EXPOSE:
        paint_note(app, n); /* the ghost-content contract */
        break;
    case FDK_EVENT_POINTER_MOTION:
    case FDK_EVENT_POINTER_ENTER: {
        int hover = hit_button(app, n, ev->pointer.position.x,
                               ev->pointer.position.y);
        if (hover != n->hover_btn) {
            n->hover_btn = hover;
            fdk_shell_layer_set_cursor(
                n->layer,
                hover >= 0 ? FDK_SHELL_CURSOR_POINTER
                           : FDK_SHELL_CURSOR_DEFAULT);
            paint_note(app, n);
        }
        break;
    }
    case FDK_EVENT_POINTER_LEAVE:
        if (n->hover_btn != -1) {
            n->hover_btn = -1;
            fdk_shell_layer_set_cursor(n->layer,
                                       FDK_SHELL_CURSOR_DEFAULT);
            paint_note(app, n);
        }
        n->press_kind = -1;
        break;
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        if (ev->pointer_button.button == FDK_POINTER_BUTTON_LEFT) {
            int b = hit_button(app, n, ev->pointer_button.position.x,
                               ev->pointer_button.position.y);
            n->press_kind = b >= 0 ? b : -2; /* -2 = body */
            if (b >= 0) {
                paint_note(app, n);
            }
        }
        break;
    case FDK_EVENT_POINTER_BUTTON_UP:
        if (ev->pointer_button.button == FDK_POINTER_BUTTON_LEFT &&
            n->press_kind != -1) {
            int b = hit_button(app, n, ev->pointer_button.position.x,
                               ev->pointer_button.position.y);
            int pressed_on = n->press_kind;
            n->press_kind = -1;
            if (b == pressed_on && b >= 0) {
                /* A specific action button. */
                emit_action_invoked(app, n->id, n->actions[b].key);
                if (!n->resident_hint) {
                    close_note(app, n, 2, 1);
                    relayout(app);
                } else {
                    paint_note(app, n);
                }
            } else if (pressed_on == -2) {
                /* Body click: the DEFAULT action when one exists,
                 * always a dismissal otherwise. */
                if (n->has_default) {
                    emit_action_invoked(app, n->id, "default");
                }
                if (!n->resident_hint) {
                    close_note(app, n, 2, 1);
                    relayout(app);
                } else {
                    paint_note(app, n);
                }
            } else {
                paint_note(app, n); /* released elsewhere: repaint */
            }
        }
        break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        /* An output went away (or the environment closed us):
         * destroy the CARD, keep the notification's identity and
         * timers alive, and self-heal onto the surviving outputs —
         * bounded, so a flapping output can never spin us. */
        note_destroy_layer(n);
        if (!app->stopping && n->recreate_count < 5) {
            n->recreate_count++;
            tracef("card id=%u closed by the environment — "
                   "re-anchoring (%d/5)",
                   n->id, n->recreate_count);
            relayout(app);
        }
        break;
    default:
        break;
    }
}

/* ---- D-Bus serving -------------------------------------------------------- */

static const char *k_introspect_xml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
    "Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect."
    ".dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.Notifications\">\n"
    "    <method name=\"GetCapabilities\">\n"
    "      <arg name=\"caps\" type=\"as\" direction=\"out\"/>\n"
    "    </method>\n"
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
    "</node>\n";

/* Replies with an error (unless the caller asked for silence). */
static void reply_error(fdk_dbus_connection *bus,
                        const fdk_dbus_message *call, const char *name,
                        const char *text) {
    if (fdk_dbus_message_flags(call) & FDK_DBUS_FLAG_NO_REPLY_EXPECTED) {
        return;
    }
    fdk_dbus_message *err = fdk_dbus_message_new_error(call, name, text);
    if (err != NULL) {
        fdk_dbus_connection_send(bus, err);
    }
}

static void serve_get_capabilities(notifyd *app,
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
            fdk_dbus_connection_send(app->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    } else {
        fdk_dbus_value_free(arr);
        fdk_dbus_message_free(rep);
    }
    tracef("GetCapabilities -> %zu caps", (size_t)CAP_COUNT);
}

static void serve_get_server_information(notifyd *app,
                                         const fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }
    int ok = fdk_ok(fdk_dbus_message_append_string(rep, "notifyd")) &&
             fdk_ok(fdk_dbus_message_append_string(
                 rep, "Faded Dream Project")) &&
             fdk_ok(fdk_dbus_message_append_string(
                 rep, fdk_shell_version())) &&
             fdk_ok(fdk_dbus_message_append_string(rep, "1.2"));
    if (ok) {
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(app->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    } else {
        fdk_dbus_message_free(rep);
    }
    tracef("GetServerInformation -> notifyd / Faded Dream Project / "
           "%s / spec 1.2",
           fdk_shell_version());
}

static void serve_close_notification(notifyd *app,
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
        reply_error(app->bus, call, "org.freedesktop.DBus.Error."
                                    "InvalidArgs",
                    "CloseNotification requires one uint32 id");
        return;
    }
    for (note *n = app->head; n != NULL; n = n->next) {
        if (n->id == id) {
            close_note(app, n, 3, 1); /* reason 3: by the call */
            relayout(app);
            /* An (optional) empty reply — most servers send one. */
            if (!(fdk_dbus_message_flags(call) &
                  FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                fdk_dbus_message *rep =
                    fdk_dbus_message_new_method_return(call);
                if (rep != NULL) {
                    fdk_dbus_connection_send(app->bus, rep);
                }
            }
            return;
        }
    }
    /* The specification: an unknown id gets an empty error reply. */
    tracef("CloseNotification id=%u: no such notification", id);
    reply_error(app->bus, call,
                "org.freedesktop.DBus.Error.InvalidArgs",
                "no such notification");
}

/* The hint readers (all lookups go through the array-of-dict
 * one-liner; every type is checked before use). */
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

static void stop_serving(notifyd *app, int code, const char *why) {
    app->stopping = 1;
    app->exit_code = code;
    app->stop_reason = why;
}

/* ---- Notify: the whole path ---------------------------------------------- */

static void serve_notify(notifyd *app, const fdk_dbus_message *call) {
    /* Flood gate FIRST: a burst client must never reach layout. */
    uint64_t now = now_ms();
    if (app->rate_len == RATE_MAX + 1) {
        memmove(app->rate_times, app->rate_times + 1,
                (size_t)RATE_MAX * sizeof(uint64_t));
        app->rate_len--;
    }
    app->rate_times[app->rate_len++] = now;
    while (app->rate_len > 0 &&
           now - app->rate_times[0] > (uint64_t)RATE_WINDOW_MS) {
        memmove(app->rate_times, app->rate_times + 1,
                (size_t)(app->rate_len - 1) * sizeof(uint64_t));
        app->rate_len--;
    }
    int flooded = app->rate_len > RATE_MAX;
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
        reply_error(app->bus, call, "org.freedesktop.DBus.Error."
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
        for (note *n = app->head; n != NULL; n = n->next) {
            if (n->id == replaces_id) {
                replaced = n;
                break;
            }
        }
    }
    if (replaced != NULL) {
        id = replaces_id; /* the spec: the same id returns */
    } else {
        id = app->next_id++;
        if (app->next_id == 0) {
            app->next_id = 1; /* never hand out 0 */
        }
        if (replaces_id != 0) {
            tracef("Notify replaces_id=%u unknown — treated as new",
                   replaces_id);
        }
    }

    if (flooded) {
        fdk_dbus_values_free_array(args, argc_);
        tracef("Notify id=%u DROPPED (rate limit)", id);
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL &&
                fdk_ok(fdk_dbus_message_append_uint32(rep, id))) {
                fdk_dbus_connection_send(app->bus, rep);
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
        reply_error(app->bus, call, "org.freedesktop.DBus.Error."
                                    "NoMemory",
                    "allocation failed");
        return;
    }
    n->id = id;
    n->hover_btn = -1;
    n->press_kind = -1;

    /* app_name: informational (traced), not drawn. */
    (void)app_name;

    /* Content: sanitize -> parse once into spans. */
    char *sum_src = trunc_utf8(summary, MAX_SUMMARY_BYTES);
    char *body_src = trunc_utf8(body, MAX_BODY_BYTES);
    fdk_color link = tk_color(FDK_TK_LINK,
                              (fdk_color){ .r = 0.31f, .g = 0.76f,
                                           .b = 0.97f, .a = 1.0f });
    char link_hex[16];
    snprintf(link_hex, sizeof(link_hex), "#%02X%02X%02X",
             (unsigned)(link.r * 255.0f + 0.5f),
             (unsigned)(link.g * 255.0f + 0.5f),
             (unsigned)(link.b * 255.0f + 0.5f));
    if (sum_src != NULL) {
        char *mk = plain_to_bold_markup(sum_src);
        free(sum_src);
        if (mk != NULL) {
            fdk_markup_parse(mk, &n->sum_plain, &n->sum_spans,
                             &n->sum_span_count);
            free(mk);
        }
    }
    if (body_src != NULL && body_src[0] != '\0') {
        char *mk = sanitize_body_markup(body_src, link_hex);
        free(body_src);
        if (mk != NULL) {
            fdk_markup_parse(mk, &n->body_plain, &n->body_spans,
                             &n->body_span_count);
            free(mk);
        }
    } else {
        free(body_src);
    }

    n->urgency = hint_urgency(hints);
    n->transient_hint = hint_bool(hints, "transient");
    n->resident_hint = hint_bool(hints, "resident");

    /* Actions: (key,label) pairs; "default" is captured, up to
     * MAX_ACTION_BUTTONS others are drawn. */
    size_t acount = fdk_dbus_value_array_count(actions);
    for (size_t i = 0; i + 1 < acount &&
                       n->action_count < MAX_ACTION_BUTTONS;) {
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
    /* An odd trailing element is ignored (the spec sends pairs). */
    /* A "default"-only pair at the tail when the loop broke early: */
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

    /* Icon (the spec's priority order). */
    n->icon = resolve_icon(app_icon, hints);

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
        timeout = app->opt.default_timeout_ms;
    }

    tracef("Notify id=%u app='%s' icon='%s' urgency=%u timeout=%u "
           "actions=%d%s hints=%zu replaces=%u summary='%.40s'",
           id, app_name ? app_name : "", app_icon ? app_icon : "",
           n->urgency, timeout, n->action_count,
           n->has_default ? "+default" : "",
           fdk_dbus_value_array_count(hints), replaces_id,
           summary ? summary : "");

    fdk_dbus_values_free_array(args, argc_);

    /* Replacing: the old note's identity CONTINUES (no closed
     * signal); destroy the old record, keep the id. */
    if (replaced != NULL) {
        close_note(app, replaced, 0, 0); /* reason 0: no signal */
    }

    /* Live-set cap: force-expire the oldest dismissible note. */
    while (app->live_count >= MAX_LIVE_NOTES) {
        note *victim = pick_overflow_victim(app);
        if (victim == NULL) {
            break;
        }
        warn_once("livecap",
                  "live-notification cap (%d) reached — the oldest "
                  "notification expires early",
                  MAX_LIVE_NOTES);
        close_note(app, victim, 1, 1);
    }

    /* Insert newest-first. */
    n->next = app->head;
    n->prev = NULL;
    if (app->head != NULL) {
        app->head->prev = n;
    }
    app->head = n;
    app->live_count++;

    note_measure(app, n);
    if (timeout > 0) {
        n->timer = fdk_timer_add(app->ctx, timeout, false,
                                 expire_timer_fn, n);
    }

    /* Visible-stack cap: the OLDEST visible card expires. */
    int visible_count = 0;
    for (note *v = app->head; v != NULL; v = v->next) {
        if (v->visible || v->layer != NULL) {
            visible_count++;
        }
    }
    while (visible_count > app->opt.max_visible) {
        note *victim = NULL;
        for (note *v = app->head; v != NULL; v = v->next) {
            if (v->visible || v->layer != NULL) {
                victim = v;
            }
        }
        if (victim == NULL || victim == n) {
            break; /* never the brand-new card */
        }
        warn_once("viscap",
                  "stack depth (%d) reached — the oldest card expires",
                  app->opt.max_visible);
        close_note(app, victim, 1, 1);
        visible_count--;
    }

    relayout(app);

    if (!(fdk_dbus_message_flags(call) &
          FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            if (fdk_ok(fdk_dbus_message_append_uint32(rep, id))) {
                fdk_dbus_connection_send(app->bus, rep);
            } else {
                fdk_dbus_message_free(rep);
            }
        }
    }
}

/* The dispatcher: everything addressed to us arrives here. */
static void method_handler(fdk_dbus_connection *bus,
                           fdk_dbus_message *call, void *user) {
    notifyd *app = user;
    (void)bus;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);

    if (iface != NULL && strcmp(iface, N_IFACE) == 0) {
        if (strcmp(member, "Notify") == 0) {
            serve_notify(app, call);
            return;
        }
        if (strcmp(member, "CloseNotification") == 0) {
            serve_close_notification(app, call);
            return;
        }
        if (strcmp(member, "GetCapabilities") == 0) {
            serve_get_capabilities(app, call);
            return;
        }
        if (strcmp(member, "GetServerInformation") == 0) {
            serve_get_server_information(app, call);
            return;
        }
    } else if (iface != NULL &&
               strcmp(iface, "org.freedesktop.DBus.Introspectable") ==
                   0) {
        if (strcmp(member, "Introspect") == 0) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL &&
                fdk_ok(fdk_dbus_message_append_string(rep,
                                                      k_introspect_xml))) {
                if (!(fdk_dbus_message_flags(call) &
                      FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                    fdk_dbus_connection_send(app->bus, rep);
                } else {
                    fdk_dbus_message_free(rep);
                }
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
            return;
        }
    }
    /* Everything else: UnknownMethod (the same answer FDK's own
     * no-handler default would give — we just make it explicit). */
    char text[128];
    snprintf(text, sizeof(text), "Unknown method '%s' on interface '%s'",
             member ? member : "?", iface ? iface : "?");
    reply_error(app->bus, call, "org.freedesktop.DBus.Error."
                                "UnknownMethod",
                text);
}

/* ---- bus-name etiquette --------------------------------------------------- */

/* The incumbent's identity, for the one clear "who owns it" line:
 * ask IT for GetServerInformation (a 1.5 s bound — a silent daemon
 * still gets the one-line exit). */
static void report_incumbent(notifyd *app, const char *owner) {
    sayf("%s is already owned by %s", N_NAME, owner ? owner : "?");
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        N_NAME, N_PATH, N_IFACE, "GetServerInformation");
    if (call == NULL) {
        sayf("...; exiting (--replace to take over)");
        stop_serving(app, 0, "name already owned");
        return;
    }
    /* incumbent_reply (below) logs the identity and stops the loop;
     * the g_stop flag is re-checked every pump, so this is a CLEAN
     * async exit, never a spin. */
    if (fdk_dbus_connection_call(app->bus, call, 1500, incumbent_reply,
                                 app) == NULL) {
        sayf("...; exiting (--replace to take over)");
        stop_serving(app, 0, "name already owned");
    }
}

static void incumbent_reply(fdk_dbus_connection *bus,
                            fdk_dbus_message *reply, void *user) {
    (void)bus;
    notifyd *app = user;
    if (reply != NULL && !fdk_dbus_message_is_error(reply)) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK &&
            n >= 3 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING &&
            fdk_dbus_value_type(args[2]) == FDK_DBUS_STRING) {
            sayf("...it is %s %s (vendor %s); exiting "
                 "(--replace to take over)",
                 fdk_dbus_value_string(args[0]),
                 n >= 3 &&
                         fdk_dbus_value_type(args[2]) == FDK_DBUS_STRING
                     ? fdk_dbus_value_string(args[2])
                     : "?",
                 fdk_dbus_value_string(args[1]));
        } else {
            sayf("%s is already owned; exiting (--replace to take "
                 "over)",
                 N_NAME);
        }
        if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
    } else {
        sayf("%s is already owned; exiting (--replace to take over)",
             N_NAME);
    }
    stop_serving(app, 0, "name already owned");
}

static void request_the_name(notifyd *app) {
    uint32_t flags = FDK_DBUS_NAME_ALLOW_REPLACEMENT;
    if (app->opt.replace) {
        flags |= FDK_DBUS_NAME_REPLACE_EXISTING;
    }
    if (!app->opt.wait) {
        flags |= FDK_DBUS_NAME_DO_NOT_QUEUE;
    }
    tracef("RequestName(%s, flags=0x%x%s%s%s)", N_NAME, flags,
           app->opt.replace ? " replace" : "",
           app->opt.wait ? " wait" : " no-queue",
           "");
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName");
    if (call == NULL) {
        stop_serving(app, 1, "allocation failure");
        return;
    }
    if (!fdk_ok(fdk_dbus_message_append_string(call, N_NAME)) ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, flags))) {
        fdk_dbus_message_free(call);
        stop_serving(app, 1, "allocation failure");
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        app->bus, call, 5000, request_name_reply, app);
    if (p == NULL) {
        stop_serving(app, 1, "RequestName could not be sent");
    }
}

static void request_name_reply(fdk_dbus_connection *bus,
                               fdk_dbus_message *reply, void *user) {
    (void)bus;
    (void)user;
    notifyd *app = &g_app;
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
    switch (code) {
    case FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER:
    case FDK_DBUS_REQUEST_NAME_ALREADY_OWNER:
        tracef("RequestName -> %u (primary owner)", code);
        if (!app->serving) {
            /* NameAcquired usually beat the reply here; announce once */
            app->serving = 1;
            sayf("serving %s as %s", N_NAME,
                 fdk_dbus_connection_unique_name(app->bus));
        }
        break;
    case FDK_DBUS_REQUEST_NAME_IN_QUEUE:
        tracef("RequestName -> queued (--wait)");
        sayf("%s is owned by another daemon; queued (--wait) — "
             "serving when it releases",
             N_NAME);
        break;
    case FDK_DBUS_REQUEST_NAME_EXISTS:
    default:
        tracef("RequestName -> %u (exists)", code);
        sayf("%s is owned by a daemon that refuses replacement; "
             "exiting",
             N_NAME);
        stop_serving(app, 0, "name exists, replacement refused");
        break;
    }
}

static void name_owner_event(fdk_dbus_connection *bus, const char *name,
                             const char *owner, void *user) {
    (void)name;
    notifyd *app = user;
    tracef("watch: %s owner -> %s", N_NAME, owner ? owner : "(none)");
    /* Our OWN acquisition echoes through the watcher: RequestName
     * makes the bus broadcast NameOwnerChanged(our name, "", our
     * unique name), and the watcher reports the new owner -- us.
     * That is not an incumbent (the rig caught the daemon reporting
     * ITSELF and exiting politely at its own reflection). The
     * RequestName reply / NameAcquired carry the truth. */
    const char *me = fdk_dbus_connection_unique_name(bus);
    if (owner != NULL && me != NULL && strcmp(owner, me) == 0) {
        return;
    }
    if (owner != NULL && !app->opt.replace && !app->opt.wait &&
        !app->serving) {
        report_incumbent(app, owner);
        return;
    }
    if (!app->serving &&
        (owner == NULL || app->opt.replace || app->opt.wait)) {
        /* No owner: take it. An owner with --replace: attempt the
         * takeover (REPLACE_EXISTING — granted only if they allowed
         * replacement, else EXISTS and the one-line exit). An owner
         * with --wait: queue for it. (The rig caught the fourth
         * combination -- owner + replace, request never sent --
         * as a daemon that sat silent forever.) */
        request_the_name(app);
    }
}

static void name_event(fdk_dbus_connection *bus, const char *name,
                       int owned, void *user) {
    (void)bus;
    notifyd *app = user;
    /* The bus ALSO delivers NameAcquired for the connection's own
     * UNIQUE name right after Hello (the daemon announces :1.x the
     * way it announces every acquisition) — that is NOT ours to
     * serve on. Filter hard: only the well-known name counts. (The
     * rig caught this: the daemon used to print "serving" on its
     * unique-name NameAcquired and never send the real RequestName,
     * so every client call bounced with ServiceUnknown.) */
    if (name == NULL || strcmp(name, N_NAME) != 0) {
        return;
    }
    if (owned) {
        if (!app->serving) {
            app->serving = 1;
            sayf("serving %s as %s", N_NAME,
                 fdk_dbus_connection_unique_name(bus));
            tracef("NameAcquired: the well-known name is ours%s",
                   " (queued path complete)");
        }
    } else if (app->serving) {
        sayf("another daemon took %s — handing over", N_NAME);
        tracef("NameLost — clean shutdown");
        stop_serving(app, 0, "replaced by another daemon");
    }
}

static void bus_ready(fdk_dbus_connection *bus, int ok, const char *error,
                      void *user) {
    notifyd *app = user;
    if (!ok) {
        sayf("session bus connection failed (%s)", error ? error : "?");
        stop_serving(app, 1, "bus connect failed");
        return;
    }
    tracef("bus ready as %s", fdk_dbus_connection_unique_name(bus));
    fdk_dbus_connection_set_name_events(bus, name_event, app);
    if (fdk_dbus_connection_watch_name(bus, N_NAME, name_owner_event,
                                       app) == NULL) {
        stop_serving(app, 1, "name watcher failed");
    }
}

static void bus_died(fdk_dbus_connection *bus, void *user) {
    (void)bus;
    notifyd *app = user;
    sayf("the session bus died — exiting (a supervisor should "
         "restart notifyd)");
    app->bus = NULL;
    stop_serving(app, 1, "bus died");
}

/* ---- output events -------------------------------------------------------- */

static void output_event(fdk_shell *shell,
                         const fdk_shell_output_event *event, void *user) {
    (void)shell;
    notifyd *app = user;
    const char *kind =
        event->type == FDK_SHELL_OUTPUT_ADDED
            ? "ADDED"
            : event->type == FDK_SHELL_OUTPUT_REMOVED ? "REMOVED"
                                                       : "CHANGED";
    tracef("output %s index=%u", kind, event->index);
    if (event->type != FDK_SHELL_OUTPUT_ADDED) {
        /* Cards on the changed/removed output get their CLOSE_REQUEST
         * (handled in note_layer_event: destroy + self-heal). A
         * CHANGED geometry re-anchors anchored surfaces itself. */
        return;
    }
    /* A new output: any headless notes (no layer after a failed
     * self-heal) get their card back. */
    relayout(app);
}

/* ---- CLI ------------------------------------------------------------------- */

static void usage(FILE *out) {
    fputs(
        "notifyd — the fdk-shell notification daemon "
        "(org.freedesktop.Notifications)\n\n"
        "usage: notifyd [options]\n\n"
        "  --timeout-ms N   default expiry for notifications that ask\n"
        "                   for the server default (-1); critical ones\n"
        "                   never expire (default 5000)\n"
        "  --max-visible N  cards kept in the stack (default 5)\n"
        "  --width N        card width in logical px (default: scaled\n"
        "                  from the UI font size; 360 at 14 px)\n"
        "  --font-size N    pin the UI font size in px (default: theme/\n"
        "                  settings / the desktop statement / 16)\n"
        "  --margin N       gap from the screen edges (default 8)\n"
        "  --gap N          gap between cards (default 8)\n"
        "  --anchor W       tr (default) | tl | br | bl\n"
        "  --output N       show on output N only (default: all)\n"
        "  --replace        take the bus name from a daemon that\n"
        "                   allows replacement\n"
        "  --wait           queue for the name instead of exiting\n"
        "  --quit-after S   exit after S seconds (rigs/testing)\n"
        "  --help           this text\n"
        "  --version        the daemon and spec versions\n\n"
        "Diagnostics: FDK_NOTIFYD_TRACE=1 logs every bus call,\n"
        "signal, and placement decision to stderr.\n",
        out);
}

static void parse_args(notifyd *app, int argc, char **argv) {
    app->opt.default_timeout_ms = 5000;
    app->opt.max_visible = 5;
    /* -1 = derive from the UI metrics after ui_init(); --width
     * pins it. */
    app->opt.card_width = -1;
    app->opt.font_size = 0;
    app->opt.edge_margin = 8;
    app->opt.card_gap = 8;
    app->opt.anchor = ANCHOR_TR;
    app->opt.output_index = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "--version") == 0) {
            printf("notifyd %s (fdk-shell %s, spec 1.2)\n",
                   fdk_shell_version(), fdk_shell_version());
            exit(0);
        } else if (strcmp(a, "--replace") == 0) {
            app->opt.replace = 1;
        } else if (strcmp(a, "--wait") == 0) {
            app->opt.wait = 1;
        } else if (strcmp(a, "--timeout-ms") == 0 && i + 1 < argc) {
            app->opt.default_timeout_ms =
                (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--max-visible") == 0 && i + 1 < argc) {
            app->opt.max_visible = atoi(argv[++i]);
            if (app->opt.max_visible < 1) {
                app->opt.max_visible = 1;
            }
            if (app->opt.max_visible > 16) {
                app->opt.max_visible = 16;
            }
        } else if (strcmp(a, "--font-size") == 0 && i + 1 < argc) {
            app->opt.font_size = atoi(argv[++i]);
            if (app->opt.font_size < 8 || app->opt.font_size > 96) {
                fprintf(stderr, "--font-size: %d out of [8, 96]\n",
                        app->opt.font_size);
                exit(2);
            }
        } else if (strcmp(a, "--width") == 0 && i + 1 < argc) {
            app->opt.card_width = atoi(argv[++i]);
            if (app->opt.card_width < 160) {
                app->opt.card_width = 160;
            }
            if (app->opt.card_width > 1000) {
                app->opt.card_width = 1000;
            }
        } else if (strcmp(a, "--margin") == 0 && i + 1 < argc) {
            app->opt.edge_margin = atoi(argv[++i]);
        } else if (strcmp(a, "--gap") == 0 && i + 1 < argc) {
            app->opt.card_gap = atoi(argv[++i]);
        } else if (strcmp(a, "--anchor") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "tr") == 0) {
                app->opt.anchor = ANCHOR_TR;
            } else if (strcmp(v, "tl") == 0) {
                app->opt.anchor = ANCHOR_TL;
            } else if (strcmp(v, "br") == 0) {
                app->opt.anchor = ANCHOR_BR;
            } else if (strcmp(v, "bl") == 0) {
                app->opt.anchor = ANCHOR_BL;
            } else {
                fprintf(stderr,
                        "notifyd: unknown --anchor '%s' (tr|tl|br|"
                        "bl)\n",
                        v);
                exit(2);
            }
        } else if (strcmp(a, "--output") == 0 && i + 1 < argc) {
            app->opt.output_index = atoi(argv[++i]);
        } else if (strcmp(a, "--quit-after") == 0 && i + 1 < argc) {
            app->opt.quit_after_s = strtod(argv[++i], NULL);
        } else {
            fprintf(stderr, "notifyd: unknown option '%s' (--help)\n",
                    a);
            exit(2);
        }
    }
    if (app->opt.edge_margin < 0) {
        app->opt.edge_margin = 0;
    }
    if (app->opt.edge_margin > 200) {
        app->opt.edge_margin = 200;
    }
    if (app->opt.card_gap < 0) {
        app->opt.card_gap = 0;
    }
    if (app->opt.default_timeout_ms == 0) {
        app->opt.default_timeout_ms = 5000;
    }
}

static void on_signal(int sig) {
    (void)sig;
    g_signal_stop = 1;
}

/* ---- main ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app));
    g_trace = getenv("FDK_NOTIFYD_TRACE") != NULL;
    parse_args(&g_app, argc, argv);

    /* The add-on's version gate (the documented pattern). */
    if (fdk_get_version() < FDK_VERSION_ENCODE(1, 1, 0) ||
        fdk_ext_api_version() < 1) {
        fprintf(stderr,
                "notifyd: FDK 1.1.0 (extension API v1) required\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr,
                "notifyd: no display — a notification daemon renders "
                "cards (WAYLAND_DISPLAY or DISPLAY required)\n");
        return 1;
    }
    g_app.ctx = ctx;

    fdk_result r = fdk_shell_init(ctx, &g_app.shell);
    if (r == FDK_ERR_UNSUPPORTED) {
        fprintf(stderr,
                "notifyd: this backend offers no layer surfaces "
                "(Wayland compositor without wlr-layer-shell?)\n");
        fdk_shutdown(ctx);
        return 1;
    }
    if (!fdk_ok(r)) {
        fprintf(stderr, "notifyd: fdk_shell_init failed (%d)\n", r);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_shell_set_output_event_callback(g_app.shell, output_event,
                                        &g_app);

    ui_init(g_app.opt.font_size);
    if (g_app.opt.card_width < 0) {
        g_app.opt.card_width = (int)(360.0 * g_ui.k + 0.5);
    }
    tracef("ui: font %dpx (k=%.2f) card_w=%d pad=%d/%d icon=%d "
           "button=%d",
           g_ui.font_px, g_ui.k, g_app.opt.card_width, g_ui.pad_x,
           g_ui.pad_y, g_ui.icon_box, g_ui.button_h);
    g_app.font = fdk_font_load_system_default(g_ui.font_px);
    if (g_app.font == NULL) {
        sayf("no system font found — cards render without text "
             "(install a font, e.g. fonts-dejavu-core)");
    }
    g_app.next_id = 1;

    /* The bus. No session bus = one line, exit 1 (never a spin). */
    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, bus_ready, &g_app);
    if (bus == NULL) {
        sayf("no session bus reachable (DBUS_SESSION_BUS_ADDRESS?) — "
             "exiting");
        fdk_font_destroy(g_app.font);
        fdk_shell_shutdown(g_app.shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_app.bus = bus;
    fdk_dbus_connection_set_death_handler(bus, bus_died, &g_app);
    fdk_dbus_connection_set_method_handler(bus, method_handler, &g_app);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* --quit-after measures SERVING time: the clock starts at the
     * first pump after the bus name is ours (a cold fontconfig cache
     * can consume seconds of process lifetime before that — the rig's
     * window must cover actual service, and a daemon that exits before
     * ever serving is a rig failure, not a pass). */
    struct timespec start;
    int serving_clock = 0;

    while (!g_app.stopping && !g_signal_stop) {
        fdk_result pr = fdk_pump_events(ctx, 100);
        if (pr < 0) {
            /* Negative = fatal (the display connection died — the
             * documented application-owned-loop contract). */
            sayf("the display connection died (%d) — exiting",
                 (int)pr);
            g_app.exit_code = 1;
            break;
        }
        if (g_app.opt.quit_after_s > 0.0) {
            if (g_app.serving && !serving_clock) {
                serving_clock = 1;
                clock_gettime(CLOCK_MONOTONIC, &start);
                tracef("quit-after clock starts (serving)");
            } else if (!serving_clock) {
                clock_gettime(CLOCK_MONOTONIC, &start);
            }
            if (serving_clock) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed =
                    (double)(now.tv_sec - start.tv_sec) +
                    (double)(now.tv_nsec - start.tv_nsec) / 1e9;
                if (elapsed >= g_app.opt.quit_after_s) {
                    break;
                }
            }
        }
    }
    if (g_signal_stop) {
        g_app.exit_code = 0;
    }

    /* Teardown: cards down (no signals — the bus may be gone),
     * then the bus, then the shell, then FDK. ASan-clean by
     * construction (the rig asserts it). */
    while (g_app.head != NULL) {
        close_note(&g_app, g_app.head, 0, 0);
    }
    if (g_app.bus != NULL) {
        fdk_dbus_connection_close(g_app.bus);
        g_app.bus = NULL;
    }
    if (g_app.font != NULL) {
        fdk_font_destroy(g_app.font);
    }
    fdk_shell_shutdown(g_app.shell);
    fdk_shutdown(ctx);
    if (g_app.stop_reason != NULL && g_app.exit_code != 0) {
        sayf("%s", g_app.stop_reason);
    }
    return g_app.exit_code;
}
