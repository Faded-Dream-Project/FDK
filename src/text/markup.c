/*
 * markup.c — the tiny tag scanner (1.4.11, the markup milestone).
 *
 * NOT an XML/HTML parser and deliberately never becoming one: a
 * fixed-vocabulary scanner over the markup string that emits PLAIN
 * UTF-8 plus an fdk_span array for the span entry points (measure /
 * draw / break / ellipsize — see fdk_text.h's markup block).
 *
 * Vocabulary: <b> <i> <u> <s> <color=#rrggbb[aa]> (+ the matching
 * closing tags), and the five classic entities (&amp; &lt; &gt;
 * &quot; &apos;) plus &nbsp;. Everything else is literal text:
 * unknown tags print as themselves, an unmatched '<' prints as
 * itself, an unclosed tag at end-of-string closes implicitly.
 * Lenient on purpose — markup strings come from translators and app
 * data, and "half the UI vanished because a tag was mistyped" is
 * the failure mode leniency exists to prevent.
 *
 * Nesting: the scanner tracks an attribute STACK, merging tags into
 * effective attributes, then emits maximal non-overlapping runs —
 * callers of fdk_markup_parse never see overlapping spans (the
 * flatten pass would still resolve them, later-wins; the parser
 * just makes its own output canonical).
 *
 * Style bits are cumulative up the stack: <b>a<i>b</i></b> is a
 * bold run then a bold+italic run. A <color> REPLACES the effective
 * color at its level (the innermost color wins, CSS-style) —
 * closing it restores the enclosing one.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"

#include <string.h>

/* ---- shared growable output ---- */

typedef struct out_text {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} out_text;

static void out_push(out_text *o, char c) {
    if (o->oom) {
        return;
    }
    if (o->len + 1 >= o->cap) {
        size_t cap = o->cap > 0 ? o->cap * 2 : 64;
        char *grown = fdk_realloc(o->buf, cap);
        if (grown == NULL) {
            o->oom = true;
            return;
        }
        o->buf = grown;
        o->cap = cap;
    }
    o->buf[o->len++] = c;
    o->buf[o->len] = '\0';
}

/* ---- the attribute stack ----

 * Depth is unbounded in principle; markup from sane sources nests a
 * handful of levels. 16 covers real UIs with room to spare; deeper
 * nesting merges into the top slot (attributes accumulate, so
 * nothing is lost — only the ability to close an inner level
 * independently, which at depth 16 nobody can see anyway). */
#define MARKUP_STACK_MAX 16

typedef struct attr_level {
    unsigned style;      /* FDK_FONT_STYLE_* bits ORed so far      */
    bool color_set;      /* this level introduced a color          */
    fdk_color color;     /* valid when color_set                   */
    bool underline;
    bool strikethrough;
} attr_level;

typedef struct attr_stack {
    attr_level levels[MARKUP_STACK_MAX];
    size_t depth;
} attr_stack;

/* Merges every live level: styles OR, decorations OR, color = the
 * innermost set one (closing a color pops its level, so "last
 * pushed that set one" is the CSS-style winner). */
static attr_level stack_effective(const attr_stack *st) {
    attr_level e = {0, false, {0, 0, 0, 0}, false, false};
    for (size_t i = 0; i < st->depth; i++) {
        e.style |= st->levels[i].style;
        e.underline |= st->levels[i].underline;
        e.strikethrough |= st->levels[i].strikethrough;
        if (st->levels[i].color_set) {
            e.color_set = true;
            e.color = st->levels[i].color;
        }
    }
    return e;
}

static bool attrs_equal(const attr_level *a, const attr_level *b) {
    return a->style == b->style && a->color_set == b->color_set &&
           (a->color_set == false ||
            (a->color.r == b->color.r && a->color.g == b->color.g &&
             a->color.b == b->color.b && a->color.a == b->color.a)) &&
           a->underline == b->underline &&
           a->strikethrough == b->strikethrough;
}

/* ---- span emission ---- */

typedef struct span_out {
    fdk_span *items;
    size_t count;
    size_t cap;
    bool oom;
    attr_level last;   /* attributes of the current open run       */
    size_t run_start;  /* plain-text byte where the run opened     */
    bool have_run;
} span_out;

/* Grows the span array by doubling; returns false on OOM. */
static bool span_grow(span_out *so) {
    if (so->count < so->cap) {
        return true;
    }
    size_t cap = so->cap > 0 ? so->cap * 2 : 8;
    fdk_span *grown = fdk_realloc(so->items, cap * sizeof *grown);
    if (grown == NULL) {
        so->oom = true;
        return false;
    }
    so->items = grown;
    so->cap = cap;
    return true;
}

static void span_emit(span_out *so, size_t end) {
    /* A fully-default run (no style, no color, no decorations) is
     * NOT emitted: tag-free text parses to zero spans — the public
     * contract "no markup, no spans" — and default runs around
     * marked-up ones stay invisible to the caller. */
    if (so->last.style == 0 && !so->last.color_set &&
        !so->last.underline && !so->last.strikethrough) {
        return;
    }
    if (!span_grow(so)) {
        return;
    }
    fdk_span *s = &so->items[so->count++];
    s->byte_start = so->run_start;
    s->byte_end = end;
    s->style = so->last.style;
    s->color_set = so->last.color_set;
    s->color = so->last.color_set ? so->last.color
                                  : (fdk_color){0, 0, 0, 0};
    s->underline = so->last.underline;
    s->strikethrough = so->last.strikethrough;
}

/* Opens (or extends) the current attribute run at plain byte `at`:
 * same attributes continue the run; changed attributes close the
 * old one and open fresh. */
static void span_open(span_out *so, const attr_level *a, size_t at) {
    if (so->have_run && attrs_equal(a, &so->last)) {
        return; /* same attributes: the run just continues */
    }
    if (so->have_run && so->run_start < at) {
        span_emit(so, at); /* attributes changed mid-text */
    }
    so->last = *a;
    so->run_start = at;
    so->have_run = true;
}

/* Closes the final run at end of plain text. */
static void span_close(span_out *so, size_t at) {
    if (so->have_run && so->run_start < at) {
        span_emit(so, at);
    }
    so->have_run = false;
}

/* ---- tag / entity scanning ---- */

static bool ci_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
    }
    return a == b;
}

/* Parses a #rrggbb / #rrggbbaa hex color starting at s[*p] (the '#'
 * already consumed by the caller). On failure restores *p and
 * returns false — the caller treats the whole tag as literal text. */
static bool parse_hex_color(const char *s, size_t len, size_t *p,
                            fdk_color *out) {
    size_t start = *p;
    fdk_u32 v = 0;
    size_t digits = 0;
    while (*p < len && digits < 8) {
        char c = s[*p];
        fdk_u32 d;
        if (c >= '0' && c <= '9') {
            d = (fdk_u32)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (fdk_u32)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (fdk_u32)(c - 'A' + 10);
        } else {
            break;
        }
        v = (v << 4) | d;
        digits++;
        (*p)++;
    }
    if (digits != 6 && digits != 8) {
        *p = start;
        return false;
    }
    fdk_u32 rgb = (digits == 8) ? (v >> 8) : v;
    fdk_u32 alpha = (digits == 8) ? (v & 0xFFu) : 0xFFu;
    out->r = (fdk_f32)((rgb >> 16) & 0xFFu) / 255.0f;
    out->g = (fdk_f32)((rgb >> 8) & 0xFFu) / 255.0f;
    out->b = (fdk_f32)(rgb & 0xFFu) / 255.0f;
    out->a = (fdk_f32)alpha / 255.0f;
    return true;
}

typedef struct tag_scan {
    bool recognized;   /* false: not our vocabulary — literal text  */
    bool closing;
    bool is_color;     /* a <color> / </color> tag                  */
    unsigned style;    /* FDK_FONT_STYLE_* bits (single-letter)     */
    bool underline;
    bool strikethrough;
    fdk_color color;   /* valid when recognized && !closing &&
                          is_color                                  */
    size_t consumed;   /* bytes of markup eaten when recognized     */
} tag_scan;

/* Scans one tag starting at s[i] (s[i] == '<'). Fills *out;
 * out->recognized false means "not our vocabulary" — the caller
 * emits the '<' literally and continues AFTER it (never skipping
 * unknown bytes). Whitespace is tolerated around names, values,
 * and before '>' — leniency, same rationale as unknown tags. */
static void scan_tag(const char *s, size_t len, size_t i, tag_scan *out) {
    memset(out, 0, sizeof *out);
    size_t p = i + 1;
    if (p >= len) {
        return; /* '<' at end: literal */
    }
    bool closing = false;
    if (s[p] == '/') {
        closing = true;
        p++;
        if (p >= len) {
            return;
        }
    }
    while (p < len && (s[p] == ' ' || s[p] == '\t')) {
        p++;
    }

    /* The name: alphanumerics up to a delimiter. */
    size_t name_start = p;
    while (p < len && s[p] != '>' && s[p] != '=' && s[p] != ' ' &&
           s[p] != '\t' &&
           ((s[p] >= 'a' && s[p] <= 'z') ||
            (s[p] >= 'A' && s[p] <= 'Z') ||
            (s[p] >= '0' && s[p] <= '9'))) {
        p++;
    }
    size_t name_len = p - name_start;

    if (name_len == 1) {
        char name = s[name_start];
        if (!ci_eq(name, 'b') && !ci_eq(name, 'i') && !ci_eq(name, 'u') &&
            !ci_eq(name, 's')) {
            return; /* single letter, not ours: literal */
        }
        while (p < len && (s[p] == ' ' || s[p] == '\t')) {
            p++;
        }
        if (p >= len || s[p] != '>') {
            return; /* '<bx>' etc: literal */
        }
        p++;
        out->recognized = true;
        out->closing = closing;
        if (ci_eq(name, 'b')) {
            out->style = FDK_FONT_STYLE_BOLD;
        } else if (ci_eq(name, 'i')) {
            out->style = FDK_FONT_STYLE_ITALIC;
        } else if (ci_eq(name, 'u')) {
            out->underline = true;
        } else {
            out->strikethrough = true;
        }
        out->consumed = p - i;
        return;
    }

    /* <color=...> / </color>: the one multi-letter name. */
    if (name_len == 5 && ci_eq(s[name_start], 'c') &&
        ci_eq(s[name_start + 1], 'o') && ci_eq(s[name_start + 2], 'l') &&
        ci_eq(s[name_start + 3], 'o') && ci_eq(s[name_start + 4], 'r')) {
        while (p < len && (s[p] == ' ' || s[p] == '\t')) {
            p++;
        }
        if (closing) {
            if (p < len && s[p] == '>') {
                out->recognized = true;
                out->closing = true;
                out->is_color = true;
                out->consumed = p + 1 - i;
            }
            return;
        }
        if (p >= len || s[p] != '=') {
            return; /* <color without a value: literal */
        }
        p++;
        while (p < len && (s[p] == ' ' || s[p] == '\t')) {
            p++;
        }
        if (p < len && (s[p] == '\'' || s[p] == '"')) {
            p++;
        }
        if (p >= len || s[p] != '#') {
            return;
        }
        p++;
        fdk_color color;
        if (!parse_hex_color(s, len, &p, &color)) {
            return;
        }
        if (p < len && (s[p] == '\'' || s[p] == '"')) {
            p++;
        }
        while (p < len && (s[p] == ' ' || s[p] == '\t')) {
            p++;
        }
        if (p >= len || s[p] != '>') {
            return;
        }
        p++;
        out->recognized = true;
        out->is_color = true;
        out->color = color;
        out->consumed = p - i;
        return;
    }
}

static const char *entity_text(const char *name, size_t name_len,
                               size_t *out_len) {
    static const struct {
        const char *name;
        const char *text;
    } table[] = {
        {"amp", "&"},  {"lt", "<"},     {"gt", ">"},
        {"quot", "\""}, {"apos", "'"},  {"nbsp", "\xC2\xA0"},
    };
    for (size_t k = 0; k < sizeof table / sizeof table[0]; k++) {
        size_t nlen = strlen(table[k].name);
        if (name_len == nlen && memcmp(name, table[k].name, nlen) == 0) {
            *out_len = strlen(table[k].text);
            return table[k].text;
        }
    }
    return NULL;
}

/* ---- the scanner ---- */

fdk_result fdk_markup_parse(const char *markup, char **out_plain,
                            fdk_span **out_spans,
                            size_t *out_span_count) {
    if (markup == NULL || out_plain == NULL || out_spans == NULL ||
        out_span_count == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_plain = NULL;
    *out_spans = NULL;
    *out_span_count = 0;

    size_t len = strlen(markup);
    out_text text = {NULL, 0, 0, false};
    span_out so = {NULL, 0, 0, false, {0, false, {0, 0, 0, 0}, false,
                                       false},
                   0, false};
    attr_stack st = {{{0, false, {0, 0, 0, 0}, false, false}}, 0};

    /* Seed the run tracker with the (empty) base attributes so the
     * first change — or the end — emits correctly. */
    attr_level base = stack_effective(&st);
    span_open(&so, &base, 0);

    size_t i = 0;
    while (i < len && !text.oom && !so.oom) {
        char c = markup[i];
        if (c == '<') {
            tag_scan tag;
            scan_tag(markup, len, i, &tag);
            if (tag.recognized) {
                bool acted = true;
                if (!tag.closing) {
                    attr_level lvl = {tag.style, tag.is_color,
                                      tag.color, tag.underline,
                                      tag.strikethrough};
                    if (st.depth < MARKUP_STACK_MAX) {
                        st.levels[st.depth++] = lvl;
                    } else {
                        /* Over-deep: merge into the top level. */
                        st.levels[st.depth - 1].style |= tag.style;
                        st.levels[st.depth - 1].underline |=
                            tag.underline;
                        st.levels[st.depth - 1].strikethrough |=
                            tag.strikethrough;
                        if (tag.is_color) {
                            st.levels[st.depth - 1].color_set = true;
                            st.levels[st.depth - 1].color = tag.color;
                        }
                    }
                } else {
                    /* Closing tags pop ONLY when a matching level is
                     * open. An unmatched close (</b> with no <b>, or
                     * a </color> whose opening form failed to parse
                     * and went literal) is LITERAL TEXT — the same
                     * "markup that isn't ours stays visible" rule as
                     * unknown tags. Nothing vanishes silently. */
                    acted = false;
                    for (size_t d = st.depth; d > 0; d--) {
                        attr_level *l = &st.levels[d - 1];
                        bool match = tag.is_color
                            ? l->color_set
                            : (tag.style != 0 &&
                               (l->style & tag.style) != 0) ||
                                  (tag.style == 0 && tag.underline &&
                                   l->underline) ||
                                  (tag.style == 0 &&
                                   tag.strikethrough &&
                                   l->strikethrough);
                        if (match) {
                            st.depth = d - 1;
                            acted = true;
                            break;
                        }
                    }
                }
                if (acted) {
                    /* Attributes may have changed: open the next run
                     * at the CURRENT plain-text length (zero new
                     * bytes when tags are adjacent — the old run, if
                     * any, only closes when real text preceded the
                     * change). */
                    attr_level now = stack_effective(&st);
                    span_open(&so, &now, text.len);
                    i += tag.consumed;
                    continue;
                }
                /* Unmatched close: literal '<', rescan after it. */
            }
            /* Not our vocabulary (or an unmatched close): literal
             * '<'. */
            out_push(&text, '<');
            i++;
            continue;
        }
        if (c == '&') {
            size_t p = i + 1;
            size_t name_start = p;
            while (p < len && p - name_start < 8 &&
                   ((markup[p] >= 'a' && markup[p] <= 'z') ||
                    (markup[p] >= 'A' && markup[p] <= 'Z') ||
                    (markup[p] >= '0' && markup[p] <= '9'))) {
                p++;
            }
            if (p < len && markup[p] == ';' && p > name_start) {
                size_t repl_len = 0;
                const char *repl =
                    entity_text(markup + name_start, p - name_start,
                                &repl_len);
                if (repl != NULL) {
                    for (size_t k = 0; k < repl_len; k++) {
                        out_push(&text, repl[k]);
                    }
                    i = p + 1;
                    continue;
                }
            }
            /* Unknown entity: literal '&'. */
            out_push(&text, '&');
            i++;
            continue;
        }
        /* Plain byte: markup is caller-owned UTF-8; span byte
         * offsets track the OUTPUT bytes, so multi-byte codepoints
         * copy through untouched and land on codepoint boundaries by
         * construction. */
        out_push(&text, c);
        i++;
    }

    /* Unclosed tags close implicitly: the final run ends at the
     * plain text's end. */
    span_close(&so, text.len);

    if (text.oom || so.oom) {
        fdk_free(text.buf);
        fdk_free(so.items);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    if (text.buf == NULL) {
        text.buf = fdk_alloc(1);
        if (text.buf == NULL) {
            fdk_free(so.items);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        text.buf[0] = '\0';
    }

    *out_plain = text.buf;
    *out_spans = so.items; /* NULL when no run ever emitted */
    *out_span_count = so.count;
    return FDK_OK;
}

void fdk_markup_free(char *plain, fdk_span *spans) {
    fdk_free(plain);
    fdk_free(spans);
}
