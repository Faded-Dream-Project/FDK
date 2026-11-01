/*
 * fdk_attr.h — range attributes for rich text (the §23/B2c round,
 * 2026-11-01): Pango-grade run attributes over FDK's existing
 * span/attribute pipeline.
 *
 * The 1.4.11 span model (fdk_text.h) covers style bits, one
 * foreground color, and underline/strikethrough flags — everything a
 * font at ONE size in ONE family can express. This header adds the
 * attributes that need MORE than the base font:
 *
 *   size        a different pixel size for the range (bigger,
 *               smaller, absolute)
 *   family      a different font family for the range (resolved
 *               through fontconfig at first use, cached on the
 *               base font like the script fallbacks)
 *   background  a highlight box behind the range
 *   underline   styles (single, double, low, error/squiggle) and a
 *               color distinct from the text color
 *   strikethrough color distinct from the text color
 *   rise        vertical displacement (superscript/subscript when
 *               combined with a smaller size; Pango's rise)
 *   letter_spacing  extra tracking between glyphs (px)
 *   link        marks the range as a hyperlink (drawn with the
 *               theme's link affordance; the app gets the URL back
 *               through hit-testing)
 *
 * The model is an ATTRIBUTE LIST: an array of (byte range,
 * attribute) applied over the plain text, converted to
 * per-byte-effective runs by the same flattener the span engine
 * uses (later entries win on overlap — the documented fdk_span
 * contract, kept identical here). Attributes compose with spans:
 * flatteners merge both into one run stream.
 *
 * The markup parser (fdk_markup_parse_ex) emits attributes for the
 * tags that need them: <span size=... font=... bgcolor=...
 * underline=... rise=... letter_spacing=...>, <sub>, <sup>, <big>,
 * <small>, <a href=...>. The classic tags keep their span forms.
 *
 * Sources for the attribute vocabulary (Part C research):
 *   https://docs.gtk.org/Pango/pango_markup.html
 *     — the attribute set and value syntax this models
 */

#ifndef FDK_ATTR_H
#define FDK_ATTR_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Underline styles ----------------------------------------------------
 * Pango's set (docs.gtk.org/Pango/pango_markup.html): none, single,
 * double, low, error. FDK draws error as a squiggle. */

typedef enum fdk_underline_style {
    FDK_UNDERLINE_NONE   = 0,
    FDK_UNDERLINE_SINGLE = 1,
    FDK_UNDERLINE_DOUBLE = 2,
    FDK_UNDERLINE_LOW    = 3, /* under the descenders               */
    FDK_UNDERLINE_ERROR  = 4, /* the spell-check squiggle           */
} fdk_underline_style;

/* ---- The attribute ------------------------------------------------------ */

typedef enum fdk_attr_kind {
    FDK_ATTR_SIZE_PT = 1,     /* f32: absolute pixel size           */
    FDK_ATTR_SIZE_DELTA = 2,  /* f32: added to the base size        */
    FDK_ATTR_FAMILY = 3,      /* char *: font family name           */
    FDK_ATTR_BACKGROUND = 4,  /* color: highlight box               */
    FDK_ATTR_UNDERLINE = 5,   /* i32: fdk_underline_style           */
    FDK_ATTR_UNDERLINE_COLOR = 6, /* color                           */
    FDK_ATTR_STRIKETHROUGH_COLOR = 7, /* color                       */
    FDK_ATTR_RISE = 8,        /* f32: px up (positive) / down       */
    FDK_ATTR_LETTER_SPACING = 9, /* f32: extra px between glyphs    */
    FDK_ATTR_LINK = 10,       /* char *: the href                   */
} fdk_attr_kind;

typedef struct fdk_attr {
    size_t byte_start;   /* first byte (boundary-snapped on apply) */
    size_t byte_end;     /* one past the last byte                 */
    fdk_attr_kind kind;

    /* The value: which member is valid depends on kind. f32s and
     * colors by value; FAMILY and LINK reference caller-owned
     * strings that must outlive the attribute list (the flattener
     * copies what it keeps). */
    union {
        fdk_f32 f;         /* SIZE_PT, SIZE_DELTA, RISE,
                              LETTER_SPACING                        */
        fdk_i32 i;         /* UNDERLINE                              */
        fdk_color color;   /* BACKGROUND, *_COLOR                    */
        const char *s;     /* FAMILY, LINK                           */
    } v;
} fdk_attr;

/* ---- Building lists ------------------------------------------------------
 * A growable attribute list. Zero-init + fdk_attr_list_destroy, or
 * the fixed-array form: fdk_attr entries in a plain C array passed
 * directly to the measure/draw twins. */

typedef struct fdk_attr_list {
    fdk_attr *items;
    size_t count;
    size_t cap;      /* internal; 0 when items is caller-owned     */
} fdk_attr_list;

fdk_result fdk_attr_list_init(fdk_attr_list *list);
void fdk_attr_list_destroy(fdk_attr_list *list);

/* Appends one attribute (the struct is copied; `s` strings are NOT
 * copied — keep them alive until the list is destroyed). */
fdk_result fdk_attr_list_add(fdk_attr_list *list, const fdk_attr *attr);

/* Convenience: adds a ranged attribute in one call. */
fdk_result fdk_attr_list_add_range(fdk_attr_list *list,
                                   size_t byte_start, size_t byte_end,
                                   fdk_attr_kind kind, fdk_f32 f,
                                   fdk_i32 i, fdk_color color,
                                   const char *s);

/* ---- The attributed entry points ----------------------------------------
 * Same contracts as their span twins (fdk_text.h), with the
 * attribute list applied: attributes and spans may BOTH be given
 * (attributes flatten on top of spans — an attribute wins where
 * they would disagree on the same property). Zero attributes: the
 * call is exactly the span twin. */

fdk_result fdk_font_measure_attrs_utf8(const fdk_font *font,
                                       const char *utf8, size_t byte_len,
                                       const fdk_span *spans,
                                       size_t span_count,
                                       const fdk_attr_list *attrs,
                                       fdk_text_metrics *out);

fdk_result fdk_surface_draw_attrs_utf8(fdk_surface *surface,
                                       fdk_font *font, const char *utf8,
                                       size_t byte_len,
                                       const fdk_span *spans,
                                       size_t span_count,
                                       const fdk_attr_list *attrs,
                                       fdk_i32 pen_x, fdk_i32 baseline_y,
                                       fdk_color base_color);

fdk_result fdk_font_break_lines_attrs_utf8(
    const fdk_font *font, const char *utf8, size_t byte_len,
    const fdk_span *spans, size_t span_count,
    const fdk_attr_list *attrs, fdk_i32 max_width,
    fdk_text_line *out_lines, size_t max_lines,
    size_t *out_line_count, bool *out_truncated);

fdk_result fdk_font_ellipsize_attrs_utf8(
    const fdk_font *font, const char *utf8, size_t byte_len,
    const fdk_span *spans, size_t span_count,
    const fdk_attr_list *attrs, fdk_i32 max_width,
    size_t *out_prefix_bytes, bool *out_fits);

/* ---- Link hit-testing ----------------------------------------------------
 * After drawing attributed text at (pen_x, baseline_y), copies the
 * href of the link under the point (window-local) into out_href
 * (truncated to cap-1 bytes, NUL-terminated) and returns true;
 * false when no link is under the point. The hit box is the run's
 * advance span over the font's line box — a link's target area is
 * its whole line box, like HTML. */
bool fdk_attrs_link_at(const fdk_font *font, const char *utf8,
                       size_t byte_len,
                       const fdk_span *spans, size_t span_count,
                       const fdk_attr_list *attrs,
                       fdk_i32 pen_x, fdk_i32 baseline_y,
                       fdk_i32 x, fdk_i32 y,
                       char *out_href, size_t cap);

/* ---- Markup with attributes ---------------------------------------------
 * fdk_markup_parse's attributed twin: the classic tags still
 * produce spans; the attribute tags (<span ...>, <sub>, <sup>,
 * <big>, <small>, <a href>) produce ATTRIBUTES in *out_attrs
 * (fdk_attr_list, caller destroys with fdk_attr_list_destroy; the
 * FAMILY/LINK strings inside are fdk_alloc'd and owned by the
 * list). Pass NULL for out_attrs to ignore attribute tags (they
 * still parse — a <span size="20">x</span> renders 'x' plain). */
fdk_result fdk_markup_parse_ex(const char *markup, char **out_plain,
                               fdk_span **out_spans,
                               size_t *out_span_count,
                               fdk_attr_list *out_attrs);

/* Releases a list (and its interned strings) filled by
 * fdk_markup_parse_ex. A list built by hand with
 * fdk_attr_list_add uses fdk_attr_list_destroy instead (its
 * strings are caller-owned by contract). */
void fdk_markup_attrs_free(fdk_attr_list *attrs);

#ifdef __cplusplus
}
#endif

#endif /* FDK_ATTR_H */
