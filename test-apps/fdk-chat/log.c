/* log.c — the chat log view (see log.h).
 *
 * Rendering pipeline per message: stamp → prefix (nick colored) →
 * the mIRC-DECODED body appended ONCE (so decoded offsets ARE
 * document offsets), then per-segment style attrs and the URL scan
 * over the decoded text. Every attr is also recorded in a ledger
 * so chat_log_trim can shift decoration when it erases the head —
 * the streaming surface's documented "attrs are not shifted" is
 * the app's bookkeeping here.
 */
#include "log.h"

#include "irc.h"

#include "fdk/fdk_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TRIM_TO (200 * 1024)

typedef struct log_rec {
    size_t start, end;
    fdk_attr_kind kind;
    fdk_color color; /* FOREGROUND/BACKGROUND/underline color */
    int i;           /* STYLE / UNDERLINE */
    const char *s;   /* LINK (the interned href)                */
} log_rec;

typedef struct chat_log {
    fdk_widget *view;
    fdk_font *font; /* borrowed (the app's mono face)          */
    size_t bytes;
    chat_log_open_link_fn on_open;
    void *user;
    /* href arena: LINK strings must outlive the widget's attr
     * list (the documented contract). Entries live for the log's
     * life — bounded by the session's link count. */
    char **hrefs;
    size_t n_hrefs, cap_hrefs;
    /* the attr ledger (the trim's shift source) */
    log_rec *recs;
    size_t n_recs, cap_recs;
} chat_log;

static const fdk_color NICK_PALETTE[12] = {
    {0.780f, 0.294f, 0.478f, 1.0f}, /* rose    */
    {0.302f, 0.541f, 0.784f, 1.0f}, /* blue    */
    {0.220f, 0.639f, 0.404f, 1.0f}, /* green   */
    {0.855f, 0.569f, 0.220f, 1.0f}, /* amber   */
    {0.573f, 0.420f, 0.796f, 1.0f}, /* violet  */
    {0.180f, 0.667f, 0.690f, 1.0f}, /* teal    */
    {0.878f, 0.404f, 0.290f, 1.0f}, /* orange  */
    {0.439f, 0.639f, 0.220f, 1.0f}, /* lime    */
    {0.796f, 0.420f, 0.596f, 1.0f}, /* magenta */
    {0.286f, 0.553f, 0.839f, 1.0f}, /* azure   */
    {0.690f, 0.580f, 0.290f, 1.0f}, /* olive   */
    {0.447f, 0.529f, 0.796f, 1.0f}, /* slate   */
};

static fdk_color nick_color(const char *nick) {
    unsigned int h = 5381u;
    for (const char *p = nick; *p != '\0'; p++) {
        h = h * 33u + (unsigned char)*p;
    }
    return NICK_PALETTE[h % 12u];
}

/* Ctrl+click on a link opens it: the widget event callback fires
 * for every event the class hook also sees, so the caret placement
 * happens as usual and the modifier press opens the target. */
static bool log_view_event(fdk_widget *w, const fdk_widget_event *ev,
                           void *user) {
    chat_log *l = user;
    (void)w;
    if (ev->type != FDK_WIDGET_POINTER_DOWN ||
        (ev->pointer.modifiers & FDK_MOD_CTRL) == 0 ||
        l->on_open == NULL) {
        return false;
    }
    char href[512];
    if (!chat_log_link_at(l, ev->pointer.position.x,
                          ev->pointer.position.y, href,
                          sizeof href)) {
        return false;
    }
    l->on_open(l->user, href);
    return true;
}

chat_log *chat_log_create(fdk_widget *parent, fdk_font *font,
                          chat_log_open_link_fn on_open, void *user) {
    chat_log *l = calloc(1, sizeof *l);
    if (l == NULL) {
        return NULL;
    }
    if (!fdk_ok(fdk_textview_create(parent, font, &l->view))) {
        free(l);
        return NULL;
    }
    fdk_textview_set_read_only(l->view, true);
    fdk_textview_set_text(l->view, "");
    l->font = font;
    l->on_open = on_open;
    l->user = user;
    if (on_open != NULL) {
        fdk_widget_set_event_callback(l->view, log_view_event, l);
    }
    return l;
}

void chat_log_destroy(chat_log *l) {
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < l->n_hrefs; i++) {
        free(l->hrefs[i]);
    }
    free(l->hrefs);
    free(l->recs);
    free(l);
}

static const char *log_intern_href(chat_log *l, const char *href) {
    if (l->n_hrefs == l->cap_hrefs) {
        size_t cap = l->cap_hrefs > 0 ? l->cap_hrefs * 2 : 16;
        char **grown = realloc(l->hrefs, cap * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        l->hrefs = grown;
        l->cap_hrefs = cap;
    }
    char *copy = malloc(strlen(href) + 1);
    if (copy == NULL) {
        return NULL;
    }
    strcpy(copy, href);
    l->hrefs[l->n_hrefs++] = copy;
    return copy;
}

/* Adds one attr to the view AND the ledger (the trim's source). */
static void log_style(chat_log *l, size_t start, size_t end,
                      fdk_attr_kind kind, fdk_color color, int i,
                      const char *s) {
    if (start >= end || start > l->bytes) {
        return;
    }
    if (end > l->bytes) {
        end = l->bytes;
    }
    fdk_attr a;
    memset(&a, 0, sizeof a);
    a.byte_start = start;
    a.byte_end = end;
    a.kind = kind;
    if (kind == FDK_ATTR_LINK) {
        a.v.s = s;
    } else if (kind == FDK_ATTR_STYLE ||
               kind == FDK_ATTR_UNDERLINE) {
        a.v.i = i;
    } else {
        a.v.color = color;
    }
    if (!fdk_ok(fdk_textview_add_attr(l->view, &a))) {
        return;
    }
    if (l->n_recs == l->cap_recs) {
        size_t cap = l->cap_recs > 0 ? l->cap_recs * 2 : 64;
        log_rec *grown = realloc(l->recs, cap * sizeof *grown);
        if (grown == NULL) {
            return; /* rendered, just not trimmable */
        }
        l->recs = grown;
        l->cap_recs = cap;
    }
    log_rec *r = &l->recs[l->n_recs++];
    r->start = start;
    r->end = end;
    r->kind = kind;
    r->color = color;
    r->i = i;
    r->s = s;
}

/* Appends text and advances the byte counter; NULL appends nothing. */
static size_t log_put(chat_log *l, const char *text) {
    if (text == NULL || *text == '\0') {
        return 0;
    }
    size_t len = strlen(text);
    if (!fdk_ok(fdk_textview_append(l->view, text))) {
        return 0;
    }
    l->bytes += len;
    return len;
}


/* The 16 classic mIRC hues, tuned to stay readable on both the
 * light and the dark faces. */
static const fdk_color MIRC16[16] = {
    {0.75f, 0.75f, 0.75f, 1}, {0.71f, 0.24f, 0.20f, 1},
    {0.28f, 0.62f, 0.29f, 1}, {0.83f, 0.55f, 0.16f, 1},
    {0.24f, 0.39f, 0.85f, 1}, {0.76f, 0.25f, 0.51f, 1},
    {0.22f, 0.64f, 0.66f, 1}, {0.85f, 0.85f, 0.85f, 1},
    {0.50f, 0.50f, 0.50f, 1}, {0.60f, 0.35f, 0.85f, 1},
    {0.45f, 0.80f, 0.45f, 1}, {0.95f, 0.80f, 0.35f, 1},
    {0.42f, 0.58f, 0.95f, 1}, {0.90f, 0.45f, 0.65f, 1},
    {0.45f, 0.85f, 0.85f, 1}, {0.15f, 0.15f, 0.15f, 1},
};

void chat_log_append(chat_log *l, chat_log_kind kind,
                     const char *nick, const char *body) {
    if (l == NULL || body == NULL) {
        return;
    }
    fdk_color ink = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    fdk_color link = fdk_theme_get_color(NULL, FDK_TK_LINK);
    fdk_color ok = fdk_theme_get_color(NULL, FDK_TK_SUCCESS);
    fdk_color bad = fdk_theme_get_color(NULL, FDK_TK_DANGER);

    /* 1. the stamp */
    char stamp[16];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(stamp, sizeof stamp, "%02d:%02d ", tmv.tm_hour,
             tmv.tm_min);
    size_t base = l->bytes;
    size_t stamp_len = log_put(l, stamp);
    if (stamp_len > 0) {
        fdk_color dim = ink;
        dim.a *= 0.5f;
        log_style(l, base, base + stamp_len, FDK_ATTR_FOREGROUND,
                  dim, 0, NULL);
    }

    /* 2. the prefix (nick colored) */
    size_t nick_len = (nick != NULL) ? strlen(nick) : 0;
    switch (kind) {
    case CHAT_MSG_PRIVMSG:
    case CHAT_MSG_HIGHLIGHT: {
        size_t p = log_put(l, "<");
        (void)p;
        size_t ns = l->bytes;
        log_put(l, nick != NULL ? nick : "");
        if (nick_len > 0) {
            log_style(l, ns, ns + nick_len, FDK_ATTR_FOREGROUND,
                      nick_color(nick), 0, NULL);
            log_style(l, ns, ns + nick_len, FDK_ATTR_STYLE, ink,
                      (int)FDK_FONT_STYLE_BOLD, NULL);
        }
        log_put(l, "> ");
        break;
    }
    case CHAT_MSG_ACTION: {
        log_put(l, "* ");
        size_t ns = l->bytes;
        log_put(l, nick != NULL ? nick : "");
        if (nick_len > 0) {
            log_style(l, ns, ns + nick_len, FDK_ATTR_FOREGROUND,
                      nick_color(nick), 0, NULL);
            log_style(l, ns, ns + nick_len, FDK_ATTR_STYLE, ink,
                      (int)FDK_FONT_STYLE_ITALIC, NULL);
        }
        log_put(l, " ");
        break;
    }
    case CHAT_MSG_JOIN:
        log_put(l, "→ ");
        break;
    case CHAT_MSG_PART:
    case CHAT_MSG_QUIT:
        log_put(l, "← ");
        break;
    case CHAT_MSG_NICK:
        log_put(l, "→ ");
        break;
    case CHAT_MSG_NOTICE:
        log_put(l, "[");
        break;
    case CHAT_MSG_STATUS:
    default:
        log_put(l, "· ");
        break;
    }
    /* decorate the prefix's nick slot for the arrow/bracket forms */
    if ((kind == CHAT_MSG_JOIN || kind == CHAT_MSG_PART ||
         kind == CHAT_MSG_QUIT) &&
        nick_len > 0) {
        size_t ns = l->bytes;
        log_put(l, nick);
        log_style(l, ns, ns + nick_len, FDK_ATTR_FOREGROUND,
                  nick_color(nick), 0, NULL);
        log_put(l, kind == CHAT_MSG_JOIN ? " has joined"
                                         : (kind == CHAT_MSG_PART
                                                ? " has left"
                                                : " has quit"));
    } else if (kind == CHAT_MSG_NICK && nick_len > 0) {
        size_t ns = l->bytes;
        log_put(l, nick);
        log_style(l, ns, ns + nick_len, FDK_ATTR_FOREGROUND,
                  nick_color(nick), 0, NULL);
        log_put(l, " is now known as ");
        size_t ns2 = l->bytes;
        size_t new_len = log_put(l, body);
        log_style(l, ns2, ns2 + new_len, FDK_ATTR_FOREGROUND,
                  nick_color(body), 0, NULL);
    } else if (kind == CHAT_MSG_NOTICE && nick_len > 0) {
        size_t ns = l->bytes;
        log_put(l, nick);
        log_style(l, ns, ns + nick_len, FDK_ATTR_FOREGROUND,
                  nick_color(nick), 0, NULL);
        log_put(l, "] ");
    }

    /* 3. the body (for the forms that still owe one) */
    const char *raw = NULL;
    size_t raw_len = 0;
    bool styled_body = false;
    switch (kind) {
    case CHAT_MSG_PRIVMSG:
    case CHAT_MSG_HIGHLIGHT:
    case CHAT_MSG_ACTION:
    case CHAT_MSG_NOTICE:
        raw = body;
        styled_body = true;
        break;
    case CHAT_MSG_STATUS:
    default:
        raw = body;
        break;
    case CHAT_MSG_JOIN:
    case CHAT_MSG_PART:
    case CHAT_MSG_QUIT:
    case CHAT_MSG_NICK:
        raw = NULL; /* already rendered in the prefix */
        break;
    }
    if (raw != NULL) {
        raw_len = strlen(raw);
        if (raw_len > IRC_LINE_MAX - 1) {
            raw_len = IRC_LINE_MAX - 1;
        }
        if (styled_body) {
            char decoded[IRC_LINE_MAX];
            irc_seg segs[IRC_SEG_MAX];
            size_t n_segs = 0;
            if (irc_decode_mirc(raw, raw_len, decoded,
                                sizeof decoded, segs, IRC_SEG_MAX,
                                &n_segs) && n_segs <= IRC_SEG_MAX) {
                size_t bs = l->bytes;
                log_put(l, decoded);
                size_t consumed = 0;
                for (size_t i = 0; i < n_segs; i++) {
                    const irc_seg *sg = &segs[i];
                    size_t seg_base = bs + consumed;
                    consumed += sg->len; /* segments tile the
                                         * decoded buffer */
                    if (sg->fg_set && sg->fg < 16) {
                        log_style(l, seg_base,
                                  seg_base + sg->len,
                                  FDK_ATTR_FOREGROUND,
                                  MIRC16[sg->fg & 0xF], 0, NULL);
                    }
                    if (sg->bg_set && sg->bg < 16) {
                        fdk_color bg = MIRC16[sg->bg & 0xF];
                        log_style(l, seg_base,
                                  seg_base + sg->len,
                                  FDK_ATTR_BACKGROUND, bg, 0, NULL);
                    }
                    if (sg->bold) {
                        log_style(l, seg_base,
                                  seg_base + sg->len,
                                  FDK_ATTR_STYLE, ink,
                                  (int)FDK_FONT_STYLE_BOLD, NULL);
                    }
                    if (sg->italic) {
                        log_style(l, seg_base,
                                  seg_base + sg->len,
                                  FDK_ATTR_STYLE, ink,
                                  (int)FDK_FONT_STYLE_ITALIC,
                                  NULL);
                    }
                    if (sg->underline) {
                        log_style(l, seg_base,
                                  seg_base + sg->len,
                                  FDK_ATTR_UNDERLINE, ink,
                                  (int)FDK_UNDERLINE_SINGLE, NULL);
                    }
                }
                /* URLs over the decoded text: the offsets ARE
                 * document bytes (the decode never expands). */
                irc_url urls[IRC_URLS_MAX];
                size_t n_urls = irc_find_urls(
                    decoded, strlen(decoded), urls, IRC_URLS_MAX);
                for (size_t i = 0; i < n_urls; i++) {
                    char href[512];
                    size_t ulen = urls[i].end - urls[i].start;
                    if (ulen >= sizeof href) {
                        continue;
                    }
                    memcpy(href, decoded + urls[i].start, ulen);
                    href[ulen] = '\0';
                    const char *target = href;
                    char full[520];
                    if (strncmp(href, "www.", 4) == 0) {
                        snprintf(full, sizeof full, "http://%s",
                                 href);
                        target = full;
                    }
                    const char *kept = log_intern_href(l, target);
                    if (kept != NULL) {
                        log_style(l, bs + urls[i].start,
                                  bs + urls[i].end,
                                  FDK_ATTR_LINK, link, 0, kept);
                        log_style(l, bs + urls[i].start,
                                  bs + urls[i].end,
                                  FDK_ATTR_FOREGROUND, link, 0,
                                  NULL);
                    }
                }
            } else {
                log_put(l, raw); /* undecodable: honest plain */
            }
        } else {
            log_put(l, raw);
        }
    }

    /* 4. the line's end + the per-kind whole-line washes */
    size_t line_start = base;
    size_t line_end = l->bytes;
    log_put(l, "\n");
    if (kind == CHAT_MSG_HIGHLIGHT) {
        fdk_color wash = fdk_theme_get_color(NULL, FDK_TK_WARNING);
        wash.a = 0.16f;
        log_style(l, line_start, line_end, FDK_ATTR_BACKGROUND,
                  wash, 0, NULL);
    } else if (kind == CHAT_MSG_JOIN || kind == CHAT_MSG_NICK) {
        log_style(l, line_start, line_end, FDK_ATTR_FOREGROUND, ok,
                  0, NULL);
    } else if (kind == CHAT_MSG_PART || kind == CHAT_MSG_QUIT) {
        log_style(l, line_start, line_end, FDK_ATTR_FOREGROUND, bad,
                  0, NULL);
    }
    (void)ink;
}

void chat_log_sysline(chat_log *l, const char *text) {
    chat_log_append(l, CHAT_MSG_STATUS, NULL, text);
}

void chat_log_trim(chat_log *l, size_t max_bytes) {
    if (l == NULL || l->bytes <= max_bytes) {
        return;
    }
    size_t target = max_bytes - max_bytes / 10; /* trim to ~90% */
    size_t need = l->bytes - target;
    const char *doc = fdk_textview_get_text(l->view);
    size_t cut = 0;
    for (size_t i = 0; i < l->bytes && cut < need; i++) {
        if (doc[i] == '\n') {
            cut = i + 1;
        }
    }
    if (cut == 0 || !fdk_ok(fdk_textview_erase(l->view, 0, cut))) {
        return;
    }
    l->bytes -= cut;
    /* Rebuild the ledger shifted; the widget's list resets to it
     * (hrefs stay interned for the log's life — the LINK strings'
     * documented outlive contract). */
    size_t w = 0;
    for (size_t i = 0; i < l->n_recs; i++) {
        log_rec *r = &l->recs[i];
        if (r->end <= cut) {
            continue; /* trimmed history */
        }
        size_t start = (r->start > cut) ? r->start - cut : 0;
        size_t end = r->end - cut;
        r->start = start;
        r->end = end;
        l->recs[w++] = *r;
    }
    l->n_recs = w;
    fdk_attr_list list;
    if (!fdk_ok(fdk_attr_list_init(&list))) {
        return;
    }
    for (size_t i = 0; i < l->n_recs; i++) {
        log_rec *r = &l->recs[i];
        if (r->start >= r->end || r->start > l->bytes) {
            continue;
        }
        if (r->end > l->bytes) {
            r->end = l->bytes;
        }
        fdk_attr a;
        memset(&a, 0, sizeof a);
        a.byte_start = r->start;
        a.byte_end = r->end;
        a.kind = r->kind;
        if (r->kind == FDK_ATTR_LINK) {
            a.v.s = r->s;
        } else if (r->kind == FDK_ATTR_STYLE ||
                   r->kind == FDK_ATTR_UNDERLINE) {
            a.v.i = r->i;
        } else {
            a.v.color = r->color;
        }
        if (!fdk_ok(fdk_attr_list_add(&list, &a))) {
            break;
        }
    }
    (void)fdk_textview_set_attrs(l->view, &list);
    fdk_attr_list_destroy(&list);
}

bool chat_log_link_at(chat_log *l, fdk_f32 x, fdk_f32 y, char *out,
                      size_t cap) {
    if (l == NULL) {
        return false;
    }
    return fdk_ok(fdk_textview_link_at(l->view, x, y, out, cap));
}

size_t chat_log_bytes(const chat_log *l) {
    return (l != NULL) ? l->bytes : 0;
}

fdk_widget *chat_log_widget(chat_log *l) {
    return (l != NULL) ? l->view : NULL;
}

const char *chat_log_text(const chat_log *l) {
    if (l == NULL || l->view == NULL) {
        return "";
    }
    return fdk_textview_get_text(l->view);
}

void chat_log_clear(chat_log *l) {
    if (l == NULL || l->bytes == 0) {
        return;
    }
    (void)fdk_textview_erase(l->view, 0, l->bytes);
    l->bytes = 0;
    l->n_recs = 0;
    (void)fdk_textview_set_attrs(l->view, NULL);
}
