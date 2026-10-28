/* notifyd.c — the fdk-shell notification daemon: the reference THIN
 * UI over the desktop model (T-B side 2).
 *
 * The protocol half — the org.freedesktop.Notifications server
 * (Notify/CloseNotification/GetCapabilities/GetServerInformation),
 * the etiquette matrix, the flood gate, the live-set cap, expiration
 * timers, and the note record store — lives in the no-drawing MODEL
 * (fdk-shell/src/desktop/notify_model.c, fdk-shell-desktop.h). This
 * example is the rendering half: it creates the model on its bus,
 * renders the card stack from the model's snapshots, and forwards
 * user interactions (action invocations, dismissals) through the
 * model's action entry points. See docs/DECISIONS.md D-006 for the
 * shape decisions.
 *
 * Usage: notifyd [--timeout-ms N] [--max-visible N] [--width N]
 *                [--font-size N] [--margin N] [--gap N]
 *                [--anchor tr|tl|br|bl] [--output N]
 *                [--replace | --wait] [--quit-after S]
 *
 * Diagnostics: FDK_NOTIFYD_TRACE=1 logs every card decision. The
 * rig: make -C fdk-shell test-notifyd.
 */
#include "fdk-shell/fdk-shell.h"
#include "fdk-shell/fdk-shell-desktop.h"

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_ACTION_KEY 64
#define MAX_ACTION_LABEL 128
#define MAX_ACTION_BUTTONS 3  /* buttons actually drawn               */
#define MAX_ICON_DIM 512      /* file/theme icon cap (same posture)  */
#define MAX_BODY_LINES 6

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
    int button_gap;     /* the button block's top gap (8)         */
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
    fflush(stderr);
    va_end(ap);
}

static void sayf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("notifyd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
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

/* ---- the card record (rendering state keyed by the model's note id) ------ */

typedef struct note {
    struct note *next; /* newest first                                */
    struct note *prev;

    uint32_t id; /* the model's stable note id                     */

    /* Rendering content, parsed ONCE from the model's snapshot
     * (the model owns the protocol record; this is the drawing
     * half's derived state). summary is escaped-plain (the spec
     * allows markup in the BODY only), wrapped in <b> for its bold
     * rendering; body is translated notification markup -> FDK
     * markup before parsing. */
    char *sum_plain;
    fdk_span *sum_spans;
    size_t sum_span_count;
    char *body_plain; /* NULL when the body was empty               */
    fdk_span *body_spans;
    size_t body_span_count;

    uint8_t urgency; /* 0 low, 1 normal, 2 critical                 */
    int resident_hint;

    int has_default; /* a "default" action key was supplied         */
    char action_keys[MAX_ACTION_BUTTONS][MAX_ACTION_KEY];
    char action_labels[MAX_ACTION_BUTTONS][MAX_ACTION_LABEL];
    int action_count;

    fdk_surface *icon; /* owned ARGB8888, NULL when none            */

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
    fdk_shell_notify_server *model; /* the protocol half            */
    fdk_font *font;

    options opt;

    note *head; /* doubly linked, newest first                      */

    int serving;     /* we own the bus name                         */
    int stopping;    /* teardown requested                          */
    int exit_code;
    const char *stop_reason;
} notifyd;

static notifyd g_app;

static volatile sig_atomic_t g_signal_stop;

/* ---- forward declarations (the event web is a cycle) ------------------- */

static void relayout(notifyd *app);
static void paint_note(notifyd *app, note *n);
static void note_layer_event(fdk_shell_layer *layer,
                             const fdk_event_data *ev, void *user);

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
 * paint text in a color. The input is the model's already-capped
 * raw string.
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
                /* Malformed numeric: keep the '&' visible — it is
                 * honest text. */
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

/* ---- icon resolution -------------------------------------------------------
 *
 * Priority per the specification (a single displayed image):
 * image-data > image-path > app_icon > icon_data — the MODEL hands
 * the raw sources (the decoded ARGB bytes for both pixmap hints,
 * the two name strings); this half turns them into surfaces. A
 * file:// URI or absolute path decodes through the bounded image
 * loader; anything else is a themed icon NAME through the engine.
 */

/* Raw ARGB32 bytes (the model's decode) -> an owned surface. */
static fdk_surface *icon_from_bytes(const uint8_t *bytes, int w, int h) {
    if (bytes == NULL || w <= 0 || h <= 0) {
        return NULL;
    }
    fdk_surface *surf = NULL;
    if (!fdk_ok(fdk_surface_create_format(w, h,
                                          FDK_SURFACE_FORMAT_ARGB8888,
                                          &surf))) {
        return NULL;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surf, &info))) {
        fdk_surface_destroy(surf);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p;
            memcpy(&p, bytes + (size_t)y * (size_t)w * 4u +
                           (size_t)x * 4u, 4u);
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] = p;
        }
    }
    return surf;
}

/* The model's pixmap hint (which: 0 image-data, 1 icon_data). */
static fdk_surface *icon_from_model_hint(notifyd *app, uint32_t id,
                                         int which) {
    int w = 0, h = 0;
    size_t need = 0;
    if (!fdk_ok(fdk_shell_notify_note_icon(app->model, id, which, &w,
                                           &h, NULL, 0, &need))) {
        return NULL;
    }
    if (need == 0 || w <= 0 || h <= 0) {
        return NULL;
    }
    uint8_t *bytes = malloc(need);
    if (bytes == NULL) {
        return NULL;
    }
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_shell_notify_note_icon(app->model, id, which, &w, &h,
                                          bytes, need, NULL))) {
        s = icon_from_bytes(bytes, w, h);
    }
    free(bytes);
    return s;
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

/* Resolves the icon for one notification from the model's snapshot
 * (the spec's priority order). */
static fdk_surface *resolve_icon(notifyd *app, uint32_t id) {
    /* image-data first. */
    fdk_surface *s = icon_from_model_hint(app, id, 0);
    if (s != NULL) {
        return s;
    }
    /* image-path: a file, or (failing that) a themed NAME. */
    fdk_shell_notify_note_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    char image_path[1024], app_icon[1024];
    size_t ip_len = sizeof(image_path), ai_len = sizeof(app_icon);
    info.image_path = image_path;
    info.image_path_len = ip_len;
    info.app_icon_name = app_icon;
    info.app_icon_name_len = ai_len;
    if (!fdk_ok(fdk_shell_notify_note_snapshot(app->model, id, &info))) {
        return NULL;
    }
    if (image_path[0] != '\0') {
        s = icon_from_file(image_path);
        if (s != NULL) {
            return s;
        }
        fdk_surface *t =
            fdk_icon_theme_surface(image_path, g_ui.icon_box);
        if (t != NULL) {
            return t;
        }
    }
    /* app_icon: a path, or a themed name. */
    if (app_icon[0] != '\0') {
        if (app_icon[0] == '/' ||
            strncmp(app_icon, "file://", 7) == 0) {
            return icon_from_file(app_icon);
        }
        return fdk_icon_theme_surface(app_icon, g_ui.icon_box);
    }
    /* The deprecated icon_data hint is the last resort (the spec's
     * compatibility order). */
    return icon_from_model_hint(app, id, 1);
}

/* ---- card painting ------------------------------------------------------- */

static fdk_color tk_color(fdk_theme_token token, fdk_color fallback) {
    fdk_color c = fdk_theme_get_color(NULL, token);
    if (c.a <= 0.0f) {
        return fallback;
    }
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
                     n->action_labels[i]);
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

/* Frees the note's RENDERING content (keeps the record + card). */
static void note_free_content(note *n) {
    fdk_markup_free(n->sum_plain, n->sum_spans);
    n->sum_plain = NULL;
    n->sum_spans = NULL;
    n->sum_span_count = 0;
    fdk_markup_free(n->body_plain, n->body_spans);
    n->body_plain = NULL;
    n->body_spans = NULL;
    n->body_span_count = 0;
    if (n->icon != NULL) {
        fdk_surface_destroy(n->icon);
        n->icon = NULL;
    }
}

static void note_destroy(note *n) {
    note_destroy_layer(n);
    note_free_content(n);
    free(n);
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

    /* The visible-stack cap: the OLDEST card expires (reason 1)
     * through the model — the close flows back as the CLOSED event
     * and this relayout runs again from it. */
    int visible_count = 0;
    for (note *n = app->head; n != NULL; n = n->next) {
        if (n->visible || n->layer != NULL) {
            visible_count++;
        }
    }
    if (visible_count > app->opt.max_visible) {
        note *victim = NULL;
        for (note *n = app->head; n != NULL; n = n->next) {
            if (n->visible || n->layer != NULL) {
                victim = n;
            }
        }
        if (victim != NULL) {
            tracef("stack depth (%d) reached — card id=%u expires",
                   app->opt.max_visible, victim->id);
            fdk_shell_notify_close(app->model, victim->id, 1);
        }
    }
}

/* ---- building the card from the model's snapshot -------------------------- */

/* Fills a UI note's rendering content from the model's record. */
static int note_fill_from_model(notifyd *app, note *n) {
    fdk_shell_notify_note_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (!fdk_ok(fdk_shell_notify_note_snapshot(app->model, n->id,
                                               &info))) {
        return 0;
    }
    /* Read the raw strings (bounded buffers). */
    char summary[4096], body[16384];
    size_t sum_len = sizeof(summary), body_len = sizeof(body);
    info.summary = summary;
    info.summary_len = sum_len;
    info.body = body;
    info.body_len = body_len;
    fdk_shell_notify_note_snapshot(app->model, n->id, &info);

    n->urgency = (uint8_t)info.urgency;
    n->resident_hint = info.resident_hint;
    n->has_default = info.has_default;

    /* Content: sanitize -> parse once into spans. */
    fdk_color link = tk_color(FDK_TK_LINK,
                              (fdk_color){ .r = 0.31f, .g = 0.76f,
                                           .b = 0.97f, .a = 1.0f });
    char link_hex[16];
    snprintf(link_hex, sizeof(link_hex), "#%02X%02X%02X",
             (unsigned)(link.r * 255.0f + 0.5f),
             (unsigned)(link.g * 255.0f + 0.5f),
             (unsigned)(link.b * 255.0f + 0.5f));
    if (summary[0] != '\0') {
        char *mk = plain_to_bold_markup(summary);
        if (mk != NULL) {
            fdk_markup_parse(mk, &n->sum_plain, &n->sum_spans,
                             &n->sum_span_count);
            free(mk);
        }
    }
    if (body[0] != '\0') {
        char *mk = sanitize_body_markup(body, link_hex);
        if (mk != NULL) {
            fdk_markup_parse(mk, &n->body_plain, &n->body_spans,
                             &n->body_span_count);
            free(mk);
        }
    }

    /* Actions: up to MAX_ACTION_BUTTONS drawn. */
    n->action_count = 0;
    for (uint32_t i = 0;
         i < info.action_count && n->action_count < MAX_ACTION_BUTTONS;
         i++) {
        char key[MAX_ACTION_KEY], label[MAX_ACTION_LABEL];
        if (fdk_ok(fdk_shell_notify_note_action(
                app->model, n->id, i, key, sizeof(key), label,
                sizeof(label)))) {
            snprintf(n->action_keys[n->action_count], MAX_ACTION_KEY,
                     "%s", key);
            snprintf(n->action_labels[n->action_count],
                     MAX_ACTION_LABEL, "%s", label);
            n->action_count++;
        }
    }

    /* The icon (the spec's priority order). */
    n->icon = resolve_icon(app, n->id);
    return 1;
}

/* Finds a UI note by the model's id. */
static note *note_find(notifyd *app, uint32_t id) {
    for (note *n = app->head; n != NULL; n = n->next) {
        if (n->id == id) {
            return n;
        }
    }
    return NULL;
}

/* ---- the model's events -> the card stack --------------------------------- */

static void model_event(fdk_shell_notify_server *server,
                        fdk_shell_notify_event_type ev, uint32_t id,
                        void *user) {
    (void)server;
    notifyd *app = user;
    switch (ev) {
    case FDK_SHELL_NOTIFY_EV_ADDED: {
        note *n = calloc(1, sizeof(note));
        if (n == NULL) {
            return;
        }
        n->id = id;
        n->hover_btn = -1;
        n->press_kind = -1;
        note_fill_from_model(app, n);
        note_measure(app, n);
        /* Insert newest-first (the model's order). */
        n->next = app->head;
        n->prev = NULL;
        if (app->head != NULL) {
            app->head->prev = n;
        }
        app->head = n;
        relayout(app);
        break;
    }
    case FDK_SHELL_NOTIFY_EV_REPLACED: {
        /* The model replaced the record under the same id: rebuild
         * this card's content in place (position preserved by the
         * stack walk). */
        note *n = note_find(app, id);
        if (n == NULL) {
            /* No card yet (built while headless): treat as ADD. */
            note *fresh = calloc(1, sizeof(note));
            if (fresh == NULL) {
                return;
            }
            fresh->id = id;
            fresh->hover_btn = -1;
            fresh->press_kind = -1;
            note_fill_from_model(app, fresh);
            note_measure(app, fresh);
            fresh->next = app->head;
            fresh->prev = NULL;
            if (app->head != NULL) {
                app->head->prev = fresh;
            }
            app->head = fresh;
            relayout(app);
            break;
        }
        note_free_content(n);
        n->hover_btn = -1;
        n->press_kind = -1;
        note_fill_from_model(app, n);
        note_measure(app, n);
        relayout(app);
        break;
    }
    case FDK_SHELL_NOTIFY_EV_CLOSED: {
        note *n = note_find(app, id);
        if (n == NULL) {
            return;
        }
        if (n->prev != NULL) {
            n->prev->next = n->next;
        } else {
            app->head = n->next;
        }
        if (n->next != NULL) {
            n->next->prev = n->prev;
        }
        note_destroy(n);
        relayout(app);
        break;
    }
    case FDK_SHELL_NOTIFY_EV_SERVER_STATE:
        if (fdk_shell_notify_server_serving(app->model)) {
            if (!app->serving) {
                app->serving = 1;
                sayf("serving org.freedesktop.Notifications as %s",
                     fdk_dbus_connection_unique_name(app->bus));
            }
        } else {
            /* Lost, or refused before ever serving (the incumbent
             * watcher path or the RequestName-EXISTS path — a
             * stubborn incumbent beats even --replace). Both are
             * terminal for the reference daemon. */
            int was_serving = app->serving;
            if (was_serving) {
                sayf("another daemon took the name — handing over");
            } else if (app->opt.replace || app->opt.wait) {
                /* We ASKED (replace/wait) and RequestName said
                 * EXISTS: the incumbent refuses replacement. */
                sayf("org.freedesktop.Notifications is owned by a "
                     "daemon that refuses replacement; exiting");
            } else {
                sayf("org.freedesktop.Notifications is already owned; "
                     "exiting (--replace to take over, --wait to "
                     "queue)");
            }
            app->serving = 0;
            app->stopping = 1;
            app->exit_code = 0;
            app->stop_reason = was_serving
                                   ? "replaced by another daemon"
                                   : "name exists, replacement refused";
        }
        break;
    default:
        break;
    }
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
        snprintf(label, sizeof(label), " %s ",
                 n->action_labels[i]);
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
                /* A specific action button: the model emits
                 * ActionInvoked and closes unless resident (the
                 * CLOSED event drives our teardown). */
                fdk_shell_notify_action_invoke(app->model, n->id,
                                               n->action_keys[b]);
                if (!n->resident_hint) {
                    /* the model's close is on its way */
                } else {
                    paint_note(app, n);
                }
            } else if (pressed_on == -2) {
                /* Body click: the DEFAULT action when one exists,
                 * always a dismissal otherwise. */
                if (n->has_default) {
                    fdk_shell_notify_action_invoke(app->model, n->id,
                                                   "default");
                } else if (!n->resident_hint) {
                    fdk_shell_notify_close(app->model, n->id, 2);
                }
                if (n->resident_hint) {
                    paint_note(app, n);
                }
            } else {
                paint_note(app, n); /* released elsewhere: repaint */
            }
        }
        break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        /* An output went away (or the environment closed us):
         * destroy the CARD, keep the notification's identity (the
         * model's record is untouched), and self-heal onto the
         * surviving outputs — bounded, so a flapping output can
         * never spin us. */
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

/* ---- bus lifecycle ----------------------------------------------------------- */

static void bus_ready(fdk_dbus_connection *bus, int ok, const char *error,
                      void *user) {
    notifyd *app = user;
    if (!ok) {
        sayf("session bus connection failed (%s)", error ? error : "?");
        app->stopping = 1;
        app->exit_code = 1;
        app->stop_reason = "bus connect failed";
        return;
    }
    tracef("bus ready as %s", fdk_dbus_connection_unique_name(bus));
    /* The MODEL owns the protocol from here (the Notifications
     * method surface, the etiquette matrix, the flood gate, the
     * live-set cap, expiration). */
    fdk_shell_notify_server_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.event_cb = model_event;
    desc.user = app;
    desc.server_name = "notifyd";
    desc.default_timeout_ms = app->opt.default_timeout_ms;
    desc.replace = app->opt.replace;
    desc.wait = app->opt.wait;
    fdk_result r =
        fdk_shell_notify_server_create(bus, app->ctx, &desc,
                                        &app->model);
    if (!fdk_ok(r)) {
        sayf("the notification model could not start (%d)", r);
        app->stopping = 1;
        app->exit_code = 1;
        app->stop_reason = "notify model create failed";
    }
}

static void bus_died(fdk_dbus_connection *bus, void *user) {
    (void)bus;
    notifyd *app = user;
    sayf("the session bus died — exiting (a supervisor should "
         "restart notifyd)");
    app->bus = NULL;
    app->model = NULL; /* the connection's teardown took the model's
                        * protocol state with it                       */
    app->stopping = 1;
    app->exit_code = 1;
    app->stop_reason = "bus died";
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
        "Diagnostics: FDK_NOTIFYD_TRACE=1 logs every card decision,\n"
        "and placement change to stderr.\n",
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

    /* Teardown: cards down (no signals — the bus may be gone), then
     * the model, then the bus, then the shell, then FDK. ASan-clean
     * by construction (the rig asserts it). */
    while (g_app.head != NULL) {
        note *n = g_app.head;
        g_app.head = n->next;
        note_destroy(n);
    }
    if (g_app.model != NULL) {
        fdk_shell_notify_server_destroy(g_app.model);
        g_app.model = NULL;
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
