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

#include <stdio.h>
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

    /* ---- §23/B2c: the attribute tags --------------------------------
     * is_attr: this open/close pair rides the ATTRIBUTE stack, not
     * the span stack. The payload carries the flattened attribute
     * the tag contributes; the scanner interns family/href strings
     * through fdk__attr_intern (the parse's arena). */
    bool is_attr;
    fdk_attr_kind kind;
    fdk_f32 num;         /* SIZE_PT / SIZE_DELTA / RISE /
                            LETTER_SPACING value                     */
    fdk_attr_kind kind2; /* the shortcut tags' second attribute
                            (<sub>/<sup> = rise + shrink)            */
    fdk_f32 num2;
    bool num2_valid;
    fdk_i32 integer;     /* UNDERLINE style                           */
    fdk_color at_color;  /* BACKGROUND / *_COLOR                      */
    bool color_valid;
    const char *str;     /* FAMILY / LINK (interned)                  */
    bool span_color_valid; /* a <span color=...> riding the SPAN stack */
    fdk_color span_color;
} tag_scan;

/* The attribute interner (attr.c): copies [s, s+n) into the parse's
 * arena; NULL on OOM. */
const char *fdk__attr_intern(const char *s, size_t n);

/* ---- the attribute stack (§23/B2c) ---------------------------------
 * One level per open attribute tag; each level carries the range's
 * START and its payload. On close, an fdk_attr is appended for the
 * level's [start, now) range. Nesting composes by construction:
 * each open level emits its own range when it closes. */
typedef struct attr_level2 {
    bool open;
    size_t start;
    fdk_attr_kind kind;
    fdk_f32 num;
    fdk_attr_kind kind2;
    fdk_f32 num2;
    bool num2_valid;
    fdk_i32 integer;
    fdk_color at_color;
    bool color_valid;
    const char *str;
    bool span_color_valid;
    fdk_color span_color;
} attr_level2;

#define ATTR_STACK_MAX 16

typedef struct attr_stack2 {
    attr_level2 levels[ATTR_STACK_MAX];
    size_t depth;
} attr_stack2;

static void attr2_push(attr_stack2 *as, const tag_scan *tag,
                       size_t at) {
    if (as->depth >= ATTR_STACK_MAX) {
        return; /* over-deep: the deepest level just extends */
    }
    attr_level2 *l = &as->levels[as->depth++];
    l->open = true;
    l->start = at;
    l->kind = tag->kind;
    l->num = tag->num;
    l->kind2 = tag->kind2;
    l->num2 = tag->num2;
    l->num2_valid = tag->num2_valid;
    l->integer = tag->integer;
    l->at_color = tag->at_color;
    l->color_valid = tag->color_valid;
    l->str = tag->str;
    l->span_color_valid = tag->span_color_valid;
    l->span_color = tag->span_color;
}

static void attr2_emit_one(fdk_attr_list *out, fdk_attr_kind kind,
                           size_t start, size_t end, fdk_f32 num,
                           fdk_i32 integer, fdk_color color,
                           const char *str) {
    fdk_attr a;
    memset(&a, 0, sizeof a);
    a.byte_start = start;
    a.byte_end = end;
    a.kind = kind;
    switch (kind) {
    case FDK_ATTR_SIZE_PT:
    case FDK_ATTR_SIZE_DELTA:
    case FDK_ATTR_RISE:
    case FDK_ATTR_LETTER_SPACING:
        a.v.f = num;
        break;
    case FDK_ATTR_UNDERLINE:
        a.v.i = integer;
        break;
    case FDK_ATTR_BACKGROUND:
    case FDK_ATTR_UNDERLINE_COLOR:
    case FDK_ATTR_STRIKETHROUGH_COLOR:
        a.v.color = color;
        break;
    case FDK_ATTR_FAMILY:
    case FDK_ATTR_LINK:
        a.v.s = str;
        break;
    default:
        return;
    }
    (void)fdk_attr_list_add(out, &a);
}

/* Pops the TOP level (closing tags pop the nearest matching one —
 * for attribute tags, unmatched closes are literal, same as the
 * span stack's rule; here we pop by kind match). Returns the
 * popped level or NULL when nothing matched. */
static attr_level2 *attr2_pop(attr_stack2 *as, size_t end,
                              fdk_attr_list *out) {
    for (size_t d = as->depth; d > 0; d--) {
        attr_level2 *l = &as->levels[d - 1];
        if (!l->open) {
            continue;
        }
        if (l->start < end) {
            if (l->kind != 0) {
                attr2_emit_one(out, l->kind, l->start, end, l->num,
                               l->integer,
                               l->color_valid ? l->at_color
                                              : (fdk_color){0, 0, 0, 0},
                               l->str);
            }
            if (l->num2_valid && l->kind2 != 0) {
                attr2_emit_one(out, l->kind2, l->start, end, l->num2, 0,
                               (fdk_color){0, 0, 0, 0}, NULL);
            }
        }
        as->depth = d - 1;
        return l;
    }
    return NULL;
}



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

    if (name_len == 1 && !ci_eq(s[name_start], 'a')) {
        /* (the single-letter <a> is the LINK tag — the attribute
         * block below owns it; b/i/u/s stay the span letters) */
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

    /* ---- §23/B2c: the attribute tags ---------------------------------
     * <span attr=...>, <sub>, <sup>, <big>, <small>, <a href=...>:
     * open/close pairs on the ATTRIBUTE stack (each contributes a
     * ranged attribute over its text). All-close forms
     * (</span>, </a>, ...) pop their level. The attribute VALUE
     * grammar follows Pango's markup reference
     * (docs.gtk.org/Pango/pango_markup.html): sizes in points or
     * px, colors #rrggbb[aa], underline keywords, numbers in px
     * (FDK's logical pixels — FDK has no points). */
    {
        static const struct {
            const char *name;
            size_t nlen;
        } attr_tags[] = {
            {"span", 4}, {"sub", 3}, {"sup", 3},
            {"big", 3}, {"small", 5}, {"a", 1},
        };
        for (size_t t = 0; t < sizeof attr_tags / sizeof attr_tags[0];
             t++) {
            if (name_len != attr_tags[t].nlen) {
                continue;
            }
            bool match = true;
            for (size_t q = 0; q < name_len; q++) {
                if (!ci_eq(s[name_start + q], attr_tags[t].name[q])) {
                    match = false;
                    break;
                }
            }
            if (!match) {
                continue;
            }
            char which = attr_tags[t].name[0];

            /* The closing forms: </span> </a> </sub> ... — no
             * values expected. */
            if (closing) {
                while (p < len && (s[p] == ' ' || s[p] == '\t')) {
                    p++;
                }
                if (p < len && s[p] == '>') {
                    out->recognized = true;
                    out->closing = true;
                    out->is_attr = true;
                    out->kind = 0;
                    out->consumed = p + 1 - i;
                }
                return;
            }

            /* <a href=...> needs its href; the rest take optional
             * attributes up to '>'. A bare <a> with no href is
             * literal (nothing to link to). */
            if (which == 'a') {
                size_t q = p;
                while (q < len && (s[q] == ' ' || s[q] == '\t')) {
                    q++;
                }
                if (q < len && s[q] == 'h') {
                    size_t an = q;
                    size_t anlen = 0;
                    while (q < len && s[q] != '=' && s[q] != '>' &&
                           s[q] != ' ') {
                        q++;
                        anlen++;
                    }
                    if (anlen == 4 && ci_eq(s[an], 'h') &&
                        ci_eq(s[an + 1], 'r') &&
                        ci_eq(s[an + 2], 'e') &&
                        ci_eq(s[an + 3], 'f') && q < len && s[q] == '=') {
                        q++;
                        while (q < len && (s[q] == ' ' || s[q] == '\t')) {
                            q++;
                        }
                        char quote = 0;
                        if (q < len && (s[q] == '\'' || s[q] == '"')) {
                            quote = s[q];
                            q++;
                        }
                        size_t vs = q;
                        while (q < len &&
                               (quote ? s[q] != quote
                                      : (s[q] != '>' && s[q] != ' '))) {
                            q++;
                        }
                        if (q > vs) {
                            const char *href = fdk__attr_intern(s + vs,
                                                                q - vs);
                            if (href == NULL) {
                                return; /* OOM: literal, parse still
                                          * fails loudly at the end */
                            }
                            if (quote && q < len) {
                                q++;
                            }
                            while (q < len && (s[q] == ' ' || s[q] == '\t')) {
                                q++;
                            }
                            if (q < len && s[q] == '>') {
                                out->recognized = true;
                                out->is_attr = true;
                                out->kind = FDK_ATTR_LINK;
                                out->str = href;
                                out->consumed = q + 1 - i;
                            }
                        }
                    }
                }
                return; /* <a> without a parseable href: literal */
            }

            /* <span ...>: parse the attribute list up to '>'.
             * The grammar (Pango's spellings; values in FDK's
             * logical pixels — FDK has no points):
             *   size="20" | size="smaller" | size="larger"
             *   bgcolor|background="#rrggbb[aa]"
             *   underline="single|double|low|error|none"
             *   underline_color="#rrggbb[aa]"
             *   rise="3"  letter_spacing="1"
             *   font|font_family|face="Family Name"
             *   color|fgcolor="#rrggbb[aa]"  (the SPAN-stack color)
             * ONE non-color attribute is carried per tag (the
             * stack opens a level per tag; a second attribute
             * composes by nesting — the documented v1 shape); a
             * color attribute rides the SPAN stack in the SAME tag
             * and takes precedence in its role. An unknown
             * attribute name makes the whole tag LITERAL (lenient,
             * the scanner's rule). */
            if (which == 's' && name_len == 4) {
                bool got_any = false;
                bool got_color = false;
                fdk_color span_color = {0, 0, 0, 0};
                size_t q = p;
                fdk_attr_kind kind = (fdk_attr_kind)0;
                fdk_f32 num = 0;
                fdk_i32 integer = 0;
                fdk_color at_color = {0, 0, 0, 0};
                bool color_valid = false;
                const char *str = NULL;
                while (q < len) {
                    while (q < len && (s[q] == ' ' || s[q] == '\t')) {
                        q++;
                    }
                    if (q >= len) {
                        return; /* unterminated: literal */
                    }
                    if (s[q] == '>') {
                        q++;
                        break;
                    }
                    size_t as = q;
                    while (q < len && s[q] != '=' && s[q] != '>' &&
                           s[q] != ' ' && s[q] != '\t') {
                        q++;
                    }
                    size_t alen = q - as;
                    while (q < len && (s[q] == ' ' || s[q] == '\t')) {
                        q++;
                    }
                    if (q >= len || s[q] != '=') {
                        return; /* attr without a value: literal */
                    }
                    q++;
                    while (q < len && (s[q] == ' ' || s[q] == '\t')) {
                        q++;
                    }
                    char quote = 0;
                    if (q < len && (s[q] == '\'' || s[q] == '"')) {
                        quote = s[q];
                        q++;
                    }
                    size_t vs = q;
                    while (q < len &&
                           (quote ? s[q] != quote
                                  : (s[q] != '>' && s[q] != ' '))) {
                        q++;
                    }
                    size_t vlen = q - vs;
                    if (quote && q < len) {
                        q++;
                    }
                    if (vlen == 0) {
                        return;
                    }

                    bool known = true;
                    if (alen == 4 && memcmp(s + as, "size", 4) == 0) {
                        if (vlen == 7 &&
                            memcmp(s + vs, "smaller", 7) == 0) {
                            kind = FDK_ATTR_SIZE_DELTA;
                            num = -2;
                        } else if (vlen == 6 &&
                                   memcmp(s + vs, "larger", 6) == 0) {
                            kind = FDK_ATTR_SIZE_DELTA;
                            num = 2;
                        } else {
                            fdk_f32 v = 0;
                            int used = 0;
                            if (sscanf(s + vs, "%f%n", &v, &used) == 1 &&
                                (size_t)used == vlen && v >= 1.0f &&
                                v <= 512.0f) {
                                kind = FDK_ATTR_SIZE_PT;
                                num = v;
                            } else {
                                return;
                            }
                        }
                    } else if ((alen == 7 &&
                                memcmp(s + as, "bgcolor", 7) == 0) ||
                               (alen == 10 &&
                                memcmp(s + as, "background", 10) == 0)) {
                        if (s[vs] == '#') {
                            size_t hp = vs + 1;
                            if (!parse_hex_color(s, len, &hp, &at_color)) {
                                return;
                            }
                            kind = FDK_ATTR_BACKGROUND;
                            color_valid = true;
                        } else {
                            return;
                        }
                    } else if ((alen == 7 &&
                                memcmp(s + as, "fgcolor", 7) == 0) ||
                               (alen == 5 &&
                                memcmp(s + as, "color", 5) == 0)) {
                        if (s[vs] == '#') {
                            size_t hp = vs + 1;
                            if (!parse_hex_color(s, len, &hp, &span_color)) {
                                return;
                            }
                            got_color = true;
                        } else {
                            return;
                        }
                    } else if (alen == 9 &&
                               memcmp(s + as, "underline", 9) == 0) {
                        if (vlen == 6 &&
                            memcmp(s + vs, "single", 6) == 0) {
                            integer = FDK_UNDERLINE_SINGLE;
                        } else if (vlen == 6 &&
                                   memcmp(s + vs, "double", 6) == 0) {
                            integer = FDK_UNDERLINE_DOUBLE;
                        } else if (vlen == 3 &&
                                   memcmp(s + vs, "low", 3) == 0) {
                            integer = FDK_UNDERLINE_LOW;
                        } else if (vlen == 5 &&
                                   memcmp(s + vs, "error", 5) == 0) {
                            integer = FDK_UNDERLINE_ERROR;
                        } else if (vlen == 4 &&
                                   memcmp(s + vs, "none", 4) == 0) {
                            integer = FDK_UNDERLINE_NONE;
                        } else {
                            return;
                        }
                        kind = FDK_ATTR_UNDERLINE;
                    } else if (alen == 14 &&
                               memcmp(s + as, "underline_color", 14) == 0) {
                        if (s[vs] == '#') {
                            size_t hp = vs + 1;
                            if (!parse_hex_color(s, len, &hp, &at_color)) {
                                return;
                            }
                            kind = FDK_ATTR_UNDERLINE_COLOR;
                            color_valid = true;
                        } else {
                            return;
                        }
                    } else if (alen == 4 &&
                               memcmp(s + as, "rise", 4) == 0) {
                        fdk_f32 v = 0;
                        int used = 0;
                        if (sscanf(s + vs, "%f%n", &v, &used) == 1 &&
                            (size_t)used == vlen) {
                            kind = FDK_ATTR_RISE;
                            num = v;
                        } else {
                            return;
                        }
                    } else if (alen == 14 &&
                               memcmp(s + as, "letter_spacing", 14) == 0) {
                        fdk_f32 v = 0;
                        int used = 0;
                        if (sscanf(s + vs, "%f%n", &v, &used) == 1 &&
                            (size_t)used == vlen) {
                            kind = FDK_ATTR_LETTER_SPACING;
                            num = v;
                        } else {
                            return;
                        }
                    } else if ((alen == 10 &&
                                memcmp(s + as, "font_family", 10) == 0) ||
                               (alen == 6 &&
                                memcmp(s + as, "family", 6) == 0) ||
                               (alen == 4 &&
                                memcmp(s + as, "font", 4) == 0) ||
                               (alen == 4 &&
                                memcmp(s + as, "face", 4) == 0)) {
                        const char *fam = fdk__attr_intern(s + vs, vlen);
                        if (fam == NULL) {
                            return; /* OOM: literal; parse fails below */
                        }
                        kind = FDK_ATTR_FAMILY;
                        str = fam;
                    } else {
                        known = false;
                    }
                    if (!known) {
                        return; /* unknown attribute: literal */
                    }
                    got_any = true;
                }
                if (!got_any) {
                    return; /* <span> with nothing: literal */
                }
                out->recognized = true;
                out->is_attr = (kind != 0);
                out->kind = kind;
                out->num = num;
                out->integer = integer;
                out->at_color = at_color;
                out->color_valid = color_valid;
                out->str = str;
                out->num2_valid = false;
                /* The color rides the SPAN stack through the same
                 * tag: the main loop reads is_color_color below. */
                out->span_color_valid = got_color;
                out->span_color = span_color;
                out->consumed = q - i;
                return;
            }

            /* The shortcuts: <sub> and <sup> rise below/above the
             * baseline AND shrink (Pango's tags do both; a pure rise
             * would collide with the neighbors' ink); <big> and
             * <small> are pure size deltas. sub/sup's rise is
             * proportional to the base size — expressed as a delta
             * fraction through the SIZE_DELTA the flattener adds to
             * the base, so the shortcut stays font-size relative. */
            while (p < len && (s[p] == ' ' || s[p] == '\t')) {
                p++;
            }
            if (p >= len || s[p] != '>') {
                return;
            }
            p++;
            out->recognized = true;
            out->is_attr = true;
            out->num2_valid = true;
            if (which == 's' && name_len == 3 &&
                s[name_start + 1] == 'u' && s[name_start + 2] == 'b') {
                out->kind = FDK_ATTR_RISE;      /* <sub>            */
                out->num = -2;
                out->kind2 = FDK_ATTR_SIZE_DELTA;
                out->num2 = -2;
            } else if (which == 's' && name_len == 3 &&
                       s[name_start + 1] == 'u' &&
                       s[name_start + 2] == 'p') {
                out->kind = FDK_ATTR_RISE;      /* <sup>            */
                out->num = 2;
                out->kind2 = FDK_ATTR_SIZE_DELTA;
                out->num2 = -2;
            } else if (which == 'b') {          /* <big>            */
                out->kind = FDK_ATTR_SIZE_DELTA;
                out->num = 2;
                out->num2_valid = false;
            } else if (which == 's' && name_len == 5) { /* <small>  */
                out->kind = FDK_ATTR_SIZE_DELTA;
                out->num = -2;
                out->num2_valid = false;
            } else {
                out->num2_valid = false;
            }
            out->consumed = p - i;
            return;
        }
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

static fdk_result markup_parse_impl(const char *markup,
                                    char **out_plain,
                                    fdk_span **out_spans,
                                    size_t *out_span_count,
                                    fdk_attr_list *out_attrs) {
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
    attr_stack2 astk = {{{0}}, 0};

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
            if (tag.recognized && tag.is_attr && !tag.is_color) {
                /* The attribute tags: their own stack. */
                bool acted = true;
                if (!tag.closing) {
                    if (tag.kind != 0 || tag.num2_valid ||
                        tag.str != NULL) {
                        attr2_push(&astk, &tag, text.len);
                    } else {
                        acted = false; /* a bare recognized attr tag
                                        * with no payload: literal */
                    }
                } else {
                    attr_level2 *popped =
                        attr2_pop(&astk, text.len, out_attrs);
                    acted = popped != NULL;
                }
                if (acted) {
                    /* A <span color=...> opens a SPAN level too. */
                    if (!tag.closing && tag.span_color_valid) {
                        attr_level lvl = {0, true, tag.span_color,
                                          false, false};
                        if (st.depth < MARKUP_STACK_MAX) {
                            st.levels[st.depth++] = lvl;
                        }
                    }
                    i += tag.consumed;
                    continue;
                }
                /* Unmatched close or empty payload: literal '<'. */
            }
            if (tag.recognized && tag.span_color_valid &&
                !tag.closing) {
                /* A <span color=...> with no other attribute: the
                 * span stack alone (is_attr was false in that case
                 * only when kind==0; the color still opens a level
                 * and consumes the tag). */
                attr_level lvl = {0, true, tag.span_color, false,
                                  false};
                if (st.depth < MARKUP_STACK_MAX) {
                    st.levels[st.depth++] = lvl;
                }
                i += tag.consumed;
                continue;
            }
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
    while (astk.depth > 0) {
        attr2_pop(&astk, text.len, out_attrs);
    }

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

fdk_result fdk_markup_parse(const char *markup, char **out_plain,
                            fdk_span **out_spans,
                            size_t *out_span_count) {
    return markup_parse_impl(markup, out_plain, out_spans,
                             out_span_count, NULL);
}

fdk_result fdk_markup_parse_ex(const char *markup, char **out_plain,
                               fdk_span **out_spans,
                               size_t *out_span_count,
                               fdk_attr_list *out_attrs) {
    if (out_attrs != NULL) {
        fdk_attr_list_init(out_attrs);
    }
    return markup_parse_impl(markup, out_plain, out_spans,
                             out_span_count, out_attrs);
}

void fdk_markup_free(char *plain, fdk_span *spans) {
    fdk_free(plain);
    fdk_free(spans);
}
