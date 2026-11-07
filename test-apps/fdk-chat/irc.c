/* irc.c — the IRC protocol engine (see irc.h). */
#include "irc.h"

#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parse --------------------------------------------------------- */

bool irc_parse(char *line, irc_msg *out) {
    memset(out, 0, sizeof *out);
    if (line == NULL) {
        return false;
    }
    char *p = line;
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    if (*p == '@') { /* IRCv3 message tags: skip to the space */
        out->raw_tags = p + 1;
        char *sp = strchr(p, ' ');
        if (sp == NULL) {
            return false; /* only tags: nothing to dispatch */
        }
        *sp = '\0';
        if (strchr(out->raw_tags, ';') != NULL ||
            strchr(out->raw_tags, '=') != NULL) {
            /* tags are consumed but not interpreted: the client
             * speaks plain RFC 2812 and tolerates servers that
             * add them */
        }
        p = sp + 1;
        while (*p == ' ') {
            p++;
        }
    }
    if (*p == ':') { /* prefix: nick[!user@host] */
        p++;
        char *sp = strchr(p, ' ');
        if (sp == NULL) {
            return false;
        }
        *sp = '\0';
        char *bang = strchr(p, '!');
        if (bang != NULL) {
            *bang = '\0';
            char *at = strchr(bang + 1, '@');
            if (at != NULL) {
                *at = '\0';
                out->host = at + 1;
            }
            out->user = bang + 1;
        }
        out->nick = p;
        p = sp + 1;
        while (*p == ' ') {
            p++;
        }
    }
    if (*p == '\0') {
        return false;
    }
    /* command: up to the next space or the trailing */
    char *cmd = p;
    while (*p != '\0' && *p != ' ') {
        p++;
    }
    if (*p == ' ') {
        *p = '\0';
        p++;
        while (*p == ' ') {
            p++;
        }
    }
    out->command = cmd;
    if (cmd[0] >= '0' && cmd[0] <= '9' && isdigit((unsigned char)cmd[1]) &&
        isdigit((unsigned char)cmd[2]) && cmd[3] == '\0') {
        out->numeric = (cmd[0] - '0') * 100 + (cmd[1] - '0') * 10 +
                       (cmd[2] - '0');
    }
    /* params */
    while (*p != '\0') {
        if (*p == ':') { /* trailing: the rest, colons included */
            p++;
            if (out->n_params < IRC_PARAMS_MAX) {
                out->params[out->n_params++] = p;
            }
            out->trailing = p;
            break;
        }
        if (out->n_params < IRC_PARAMS_MAX) {
            out->params[out->n_params++] = p;
        }
        char *sp = strchr(p, ' ');
        if (sp == NULL) {
            break;
        }
        *sp = '\0';
        p = sp + 1;
        while (*p == ' ') {
            p++;
        }
    }
    return out->command[0] != '\0';
}

/* ---- mIRC decode ---------------------------------------------------- */

/* Parses <fg>[,<bg>] just past 0x03; returns the position after the
 * color spec. A bare 0x03 resets both colors to default. Numbers
 * beyond 99 clamp to 99 (the "default" slot). */
static const char *irc_color_parse(const char *p, const char *end,
                                   uint8_t *out_fg, bool *out_fg_set,
                                   uint8_t *out_bg, bool *out_bg_set) {
    uint32_t vals[2] = {0, 0};
    bool have[2] = {false, false};
    for (int part = 0; part < 2; part++) {
        int digits = 0;
        while (p < end && isdigit((unsigned char)*p) && digits < 2) {
            vals[part] = vals[part] * 10u + (uint32_t)(*p - '0');
            p++;
            digits++;
        }
        if (digits > 0) {
            have[part] = true;
        }
        if (part == 0 && p + 1 < end && *p == ',' &&
            isdigit((unsigned char)p[1])) {
            p++;
            continue;
        }
        break;
    }
    if (!have[0]) {
        *out_fg_set = false; /* bare 0x03: colors off */
        *out_bg_set = false;
        return p;
    }
    *out_fg = (uint8_t)(vals[0] > 98u ? 99u : vals[0]);
    *out_fg_set = true;
    if (have[1]) {
        *out_bg = (uint8_t)(vals[1] > 98u ? 99u : vals[1]);
        *out_bg_set = true;
    }
    return p;
}

bool irc_decode_mirc(const char *src, size_t len, char *out_text,
                     size_t out_cap, irc_seg *segs, size_t seg_cap,
                     size_t *out_n) {
    if (out_n == NULL || out_text == NULL || out_cap == 0) {
        return false;
    }
    *out_n = 0;
    size_t w = 0;
    size_t out_len = (len < out_cap - 1) ? len : out_cap - 1;
    const char *end = src + len;

    irc_seg cur;
    memset(&cur, 0, sizeof cur);
    cur.fg = 99;
    cur.bg = 99;
    const char *seg_start = out_text;
    bool overflow = false;

#define SEG_FLUSH()                                                   \
    do {                                                              \
        if (w > (size_t)(seg_start - out_text)) {                     \
            out_text[w] = '\0';                                       \
            size_t slot = (*out_n < seg_cap) ? *out_n : seg_cap - 1;  \
            if (*out_n >= seg_cap) {                                  \
                overflow = seg_cap > 0;                               \
            }                                                         \
            if (seg_cap > 0) {                                        \
                segs[slot] = cur;                                     \
                segs[slot].text = seg_start;                          \
                segs[slot].len = w - (size_t)(seg_start - out_text);  \
            }                                                         \
            if (*out_n < seg_cap) {                                   \
                (*out_n)++;                                           \
            }                                                         \
        }                                                             \
        seg_start = out_text + w;                                     \
    } while (0)

    for (const char *p = src; p < end && w < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case 0x02:
            SEG_FLUSH();
            cur.bold = true;
            break;
        case 0x1D:
            SEG_FLUSH();
            cur.italic = true;
            break;
        case 0x1F:
            SEG_FLUSH();
            cur.underline = true;
            break;
        case 0x1E:
            SEG_FLUSH();
            cur.strike = true;
            break;
        case 0x11:
            SEG_FLUSH();
            cur.mono = true;
            break;
        case 0x16:
            SEG_FLUSH();
            cur.reverse = true;
            break;
        case 0x0F:
            SEG_FLUSH();
            memset(&cur, 0, sizeof cur);
            cur.fg = 99;
            cur.bg = 99;
            break;
        case 0x03: {
            uint8_t fg = 99, bg = 99;
            bool fg_set = false, bg_set = false;
            const char *next = irc_color_parse(p + 1, end, &fg,
                                               &fg_set, &bg, &bg_set);
            SEG_FLUSH();
            cur.fg = fg_set ? fg : 99;
            cur.bg = bg_set ? bg : 99;
            cur.fg_set = fg_set;
            cur.bg_set = bg_set;
            p = next - 1;
            break;
        }
        case 0x29: /* IRCv3: reset colors only */
            SEG_FLUSH();
            cur.fg = 99;
            cur.bg = 99;
            cur.fg_set = false;
            cur.bg_set = false;
            break;
        default:
            out_text[w++] = (char)c;
            break;
        }
    }
    SEG_FLUSH();
    out_text[w] = '\0';
    return !overflow;
#undef SEG_FLUSH
}

/* ---- urls ------------------------------------------------------------ */

static bool irc_url_char(char c) {
    return isalnum((unsigned char)c) || strchr("-._~:/?#[]@!$&'()*+,;=%",
                                               c) != NULL;
}

size_t irc_find_urls(const char *text, size_t len, irc_url *out,
                     size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        bool scheme = false;
        size_t lead = 0;
        if (len - i >= 8 && strncasecmp(text + i, "https://", 8) == 0) {
            scheme = true;
            lead = 8;
        } else if (len - i >= 7 &&
                   strncasecmp(text + i, "http://", 7) == 0) {
            scheme = true;
            lead = 7;
        } else if (len - i >= 4 && strncasecmp(text + i, "www.", 4) == 0 &&
                   (i == 0 || !irc_url_char(text[i - 1]))) {
            scheme = true;
            lead = 4;
        }
        if (!scheme) {
            continue;
        }
        if (i > 0 && irc_url_char(text[i - 1])) {
            continue; /* mid-word */
        }
        size_t j = i + lead;
        while (j < len && irc_url_char(text[j])) {
            j++;
        }
        while (j > i + lead &&
               (text[j - 1] == '.' || text[j - 1] == ',' ||
                text[j - 1] == ')' || text[j - 1] == ';' ||
                text[j - 1] == '!' || text[j - 1] == '?' ||
                text[j - 1] == ':')) {
            j--; /* sentence punctuation is not the URL's */
        }
        if (j > i + lead && n < cap) {
            out[n].start = i;
            out[n].end = j;
            n++;
        }
        if (j > i) {
            i = j - 1;
        }
    }
    return n;
}

/* ---- mentions --------------------------------------------------------- */

char irc_nick_ci_dbg(char c);
/* the word-boundary test for mentions: letters/digits/underscore
 * (NOT the URL punctuation set — "Dream," ends a word) */
static bool irc_word_char(char c) {
    return isalnum((unsigned char)c) != 0 || c == '_';
}

static char irc_nick_ci(char c) {
    switch (c) {
    case '{':
        return '[';
    case '}':
        return ']';
    case '|':
        return '\\';
    case '^':
        return '~';
    default:
        return (char)tolower((unsigned char)c);
    }
}

bool irc_mentions(const char *nick, const char *text) {
    if (nick == NULL || *nick == '\0' || text == NULL) {
        return false;
    }
    size_t nl = strlen(nick);
    for (const char *p = text; *p != '\0'; p++) {
        if (irc_nick_ci(*p) != irc_nick_ci(nick[0])) {
            continue;
        }
        size_t k = 1;
        while (k < nl && p[k] != '\0' &&
               irc_nick_ci(p[k]) == irc_nick_ci(nick[k])) {
            k++;
        }
        if (k < nl) {
            continue;
        }
        bool left_ok = (p == text || !irc_word_char(p[-1]));
        bool right_ok = (p[k] == '\0' || !irc_word_char(p[k]));
        if (left_ok && right_ok) {
            return true;
        }
    }
    return false;
}

/* ---- channel state ------------------------------------------------------ */

void irc_state_init(irc_state *st) {
    memset(st, 0, sizeof *st);
}

void irc_state_destroy(irc_state *st) {
    for (size_t i = 0; i < st->n_chans; i++) {
        free(st->chans[i].users);
    }
    free(st->chans);
    irc_state_init(st);
}

static char irc_chan_ci(char c) {
    switch (c) {
    case '{':
        return '[';
    case '}':
        return ']';
    case '|':
        return '\\';
    default:
        return (char)tolower((unsigned char)c);
    }
}

static bool irc_name_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (irc_chan_ci(*a) != irc_chan_ci(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

irc_chan *irc_chan_find(irc_state *st, const char *name) {
    for (size_t i = 0; i < st->n_chans; i++) {
        if (irc_name_eq(st->chans[i].name, name)) {
            return &st->chans[i];
        }
    }
    return NULL;
}

irc_chan *irc_chan_join(irc_state *st, const char *name) {
    if (st == NULL || name == NULL || *name == '\0') {
        return NULL;
    }
    irc_chan *ch = irc_chan_find(st, name);
    if (ch != NULL) {
        return ch;
    }
    if (st->n_chans == st->cap_chans) {
        size_t cap = st->cap_chans > 0 ? st->cap_chans * 2 : 4;
        irc_chan *grown =
            realloc(st->chans, cap * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        st->chans = grown;
        st->cap_chans = cap;
    }
    ch = &st->chans[st->n_chans++];
    memset(ch, 0, sizeof *ch);
    snprintf(ch->name, sizeof ch->name, "%s", name);
    return ch;
}

bool irc_chan_part(irc_state *st, const char *name) {
    for (size_t i = 0; i < st->n_chans; i++) {
        if (irc_name_eq(st->chans[i].name, name)) {
            free(st->chans[i].users);
            memmove(&st->chans[i], &st->chans[i + 1],
                    (st->n_chans - i - 1) * sizeof *st->chans);
            st->n_chans--;
            return true;
        }
    }
    return false;
}

bool irc_chan_user_add(irc_chan *ch, const char *nick) {
    if (ch == NULL || nick == NULL || *nick == '\0') {
        return false;
    }
    char mode = 0;
    while (*nick != '\0' &&
           (*nick == '~' || *nick == '&' || *nick == '@' ||
            *nick == '%' || *nick == '+')) {
        mode = (mode == 0) ? *nick : mode; /* first = highest */
        nick++;
    }
    if (irc_chan_user_find(ch, nick) != NULL) {
        return true;
    }
    if (ch->n_users == ch->cap_users) {
        size_t cap = ch->cap_users > 0 ? ch->cap_users * 2 : 16;
        irc_user *grown = realloc(ch->users, cap * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        ch->users = grown;
        ch->cap_users = cap;
    }
    irc_user *u = &ch->users[ch->n_users++];
    snprintf(u->nick, sizeof u->nick, "%s", nick);
    u->mode = mode;
    return true;
}

irc_user *irc_chan_user_find(irc_chan *ch, const char *nick) {
    for (size_t i = 0; i < ch->n_users; i++) {
        if (irc_name_eq(ch->users[i].nick, nick)) {
            return &ch->users[i];
        }
    }
    return NULL;
}

bool irc_chan_user_remove(irc_chan *ch, const char *nick) {
    for (size_t i = 0; i < ch->n_users; i++) {
        if (irc_name_eq(ch->users[i].nick, nick)) {
            memmove(&ch->users[i], &ch->users[i + 1],
                    (ch->n_users - i - 1) * sizeof *ch->users);
            ch->n_users--;
            return true;
        }
    }
    return false;
}

bool irc_chan_user_rename(irc_chan *ch, const char *from,
                          const char *to) {
    irc_user *u = irc_chan_user_find(ch, from);
    if (u == NULL) {
        return false;
    }
    snprintf(u->nick, sizeof u->nick, "%s", to);
    return true;
}

void irc_chan_names_add(irc_chan *ch, const char *names_line) {
    if (ch == NULL || names_line == NULL) {
        return;
    }
    char body[IRC_LINE_MAX];
    snprintf(body, sizeof body, "%s", names_line);
    char *save = NULL;
    for (char *tok = strtok_r(body, " ", &save); tok != NULL;
         tok = strtok_r(NULL, " ", &save)) {
        if (!irc_chan_user_add(ch, tok)) {
            return; /* allocation failed; keep what we have */
        }
    }
}

bool irc_is_channel(const char *name) {
    return name != NULL && (name[0] == '#' || name[0] == '&');
}
