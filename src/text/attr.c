/*
 * attr.c — the range-attribute layer over the span engine (§23/B2c,
 * 2026-11-01; include/fdk/fdk_attr.h).
 *
 * The 1.0.0 span engine (span.c) gave rich text ONE axis: the
 * base font, restyled per run. This file adds the attributes that
 * need a DIFFERENT rasterization or extra paint geometry:
 *
 *   size / family   a per-run font — resolved through the family
 *                   resolver below (fontconfig, cached on the base
 *                   font exactly like shaping.c's script fallbacks)
 *                   at the run's effective pixel size, loaded with
 *                   the ordinary fdk_font_load machinery, and owned
 *                   by a per-call font table that dies with the run
 *                   array's arena
 *   background      a highlight box painted under the run's glyphs
 *   underline style / colors  the five Pango styles, each bar in
 *                   its own color when the attribute set one
 *   rise            the vertical displacement (sup/sub paint above
 *                   or below the line)
 *   letter_spacing  extra tracking folded into the pen between this
 *                   run's glyphs
 *   link            href hit-testing (fdk_attrs_link_at) + the link
 *                   affordance (an underline in the base color when
 *                   the markup did not color it)
 *
 * The pipeline shape is deliberately the span engine's: ONE
 * flattener producing the run stream every walk (measure, draw,
 * break, ellipsize) already consumes; the shaping layer resolves
 * the per-run font through the run's fields (shaping.c's segment
 * builder), so complex-script shaping, bidi, and fallbacks all
 * keep working under attributes. No second rounding rule is
 * introduced anywhere.
 *
 * Attribute vocabulary modeled on Pango's markup reference
 * (https://docs.gtk.org/Pango/pango_markup.html, fetched for this
 * round); the tag scanner in markup.c grew the attribute tags.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"

#include <math.h>
#include <string.h>

/* Ordered later in this file: the shaping-side resolver + the
 * markup interner (the header declares the flatten; these two are
 * attr.c's own cross-file seams). */
fdk_font *fdk__attr_font_for(const fdk_font *base, const char *family,
                             fdk_i32 size_px, fdk_i32 size_delta,
                             fdk_font *fallback);
const char *fdk__attr_intern(const char *s, size_t n);
fdk_result fdk__draw_attrs_runs(fdk_surface *surface, fdk_font *font,
                                const char *utf8, size_t byte_len,
                                const fdk__text_run *runs,
                                size_t n_runs, size_t off, size_t count,
                                fdk_i32 pen_x, fdk_i32 baseline_y,
                                fdk_color base_color);

/* ---- the family resolver ------------------------------------------------
 * Cached ON the base font beside shaping.c's script fallbacks — the
 * same lifetime discipline (dies with the font, never a global). A
 * resolved family is loaded at the SIZE the run needs; one family
 * at many sizes caches one fdk_font per size (a size-keyed small
 * table inside the slot). */

#define FDK_ATTR_FAMILY_MAX 4
#define FDK_ATTR_FAMILY_SIZES 6

typedef struct attr_family_size {
    fdk_i32 size_px;
    fdk_font *font;
} attr_family_size;

typedef struct attr_family_slot {
    char *family;                     /* the cache key, fdk_alloc'd  */
    attr_family_size sizes[FDK_ATTR_FAMILY_SIZES];
    int n_sizes;
} attr_family_slot;

/* Where the slots live: an array hung off the font's shaper state is
 * not possible without touching shaping.c's opaque type, so attr.c
 * keeps ONE process-wide table keyed by the BASE font pointer. The
 * table is bounded (each base font gets FDK_ATTR_FAMILY_MAX slots)
 * and dies with the process — fonts in FDK live for the process
 * anyway (the documented posture). Static, UI-thread-only, like
 * every text-cache in the tree. */
static struct {
    const fdk_font *owner;
    attr_family_slot slots[FDK_ATTR_FAMILY_MAX];
    int n_slots;
    bool tried;
} g_attr_fonts[8];
static int g_attr_fonts_n;

/* The fontconfig family resolver: fontscan.c's pattern/sort walk
 * with FAMILY instead of the generic sans-serif / a language. The
 * dlopen'd-API + LSan-bracket posture is fontscan.c's; this mirror
 * keeps the same discipline (everything fontconfig allocates here
 * is process-lifetime state under the documented suppression). */
bool fdk__fontscan_resolve_family(const char *family, char **out_path,
                                  long *out_face);

static fdk_font *attr_font_for(const fdk_font *base, const char *family,
                               fdk_i32 size_px) {
    if (base == NULL || family == NULL || family[0] == '\0' ||
        size_px < 1 || size_px > 512) {
        return NULL;
    }

    /* Find this base font's slot table. */
    int t = -1;
    for (int i = 0; i < g_attr_fonts_n; i++) {
        if (g_attr_fonts[i].owner == base) {
            t = i;
            break;
        }
    }
    if (t < 0 && g_attr_fonts_n < 8) {
        t = g_attr_fonts_n++;
        memset(&g_attr_fonts[t], 0, sizeof g_attr_fonts[t]);
        g_attr_fonts[t].owner = base;
    }
    if (t < 0) {
        /* Table full: recycle the first entry (fonts are few). */
        t = 0;
        for (int s = 0; s < g_attr_fonts[t].n_slots; s++) {
            for (int k = 0; k < g_attr_fonts[t].slots[s].n_sizes; k++) {
                /* Fonts in the table are OUR loads — destroy them. */
                fdk_font *f = g_attr_fonts[t].slots[s].sizes[k].font;
                g_attr_fonts[t].slots[s].sizes[k].font = NULL;
                if (f != NULL) {
                    fdk_font_destroy(f);
                }
            }
        }
        memset(&g_attr_fonts[t], 0, sizeof g_attr_fonts[t]);
        g_attr_fonts[t].owner = base;
    }
    attr_family_slot *table = g_attr_fonts[t].slots;
    int *n_slots = &g_attr_fonts[t].n_slots;

    attr_family_slot *slot = NULL;
    for (int i = 0; i < *n_slots; i++) {
        if (strcmp(table[i].family, family) == 0) {
            slot = &table[i];
            break;
        }
    }
    if (slot == NULL) {
        if (*n_slots >= FDK_ATTR_FAMILY_MAX) {
            return NULL; /* bounded: the 5th family in one text
                          * renders on the base font, honestly */
        }
        char *key = fdk_alloc(strlen(family) + 1);
        if (key == NULL) {
            return NULL;
        }
        strcpy(key, family);
        slot = &table[(*n_slots)++];
        memset(slot, 0, sizeof *slot);
        slot->family = key;
    }

    for (int i = 0; i < slot->n_sizes; i++) {
        if (slot->sizes[i].size_px == size_px) {
            return slot->sizes[i].font;
        }
    }
    if (slot->n_sizes >= FDK_ATTR_FAMILY_SIZES) {
        /* Size churn: replace the least useful (first) entry. */
        if (slot->sizes[0].font != NULL) {
            fdk_font_destroy(slot->sizes[0].font);
        }
        slot->sizes[0].size_px = size_px;
        slot->sizes[0].font = NULL;
        slot->n_sizes = 1;
    }

    char *path = NULL;
    long face = 0;
    if (!fdk__fontscan_resolve_family(family, &path, &face)) {
        FDK_WARN("attrs: no loadable face for family \"%s\" — the "
                 "run renders on the base font",
                 family);
        return NULL;
    }
    fdk_font *f = fdk_font_load(path, size_px);
    fdk_free(path);
    if (f == NULL) {
        return NULL;
    }
    slot->sizes[slot->n_sizes].size_px = size_px;
    slot->sizes[slot->n_sizes].font = f;
    slot->n_sizes++;
    return f;
}

/* Puts a caller-loaded font into the per-base cache under `key`
 * (a family name or a path); returns the cached font (a pre-existing
 * entry for the same key+size wins — the fresh load is destroyed so
 * exactly ONE font per key+size stays alive). */
static fdk_font *attr_cache_put(const fdk_font *base, const char *key,
                                fdk_i32 size_px, fdk_font *loaded) {
    int t = -1;
    for (int i = 0; i < g_attr_fonts_n; i++) {
        if (g_attr_fonts[i].owner == base) {
            t = i;
            break;
        }
    }
    if (t < 0 && g_attr_fonts_n < 8) {
        t = g_attr_fonts_n++;
        memset(&g_attr_fonts[t], 0, sizeof g_attr_fonts[t]);
        g_attr_fonts[t].owner = base;
    }
    if (t < 0) {
        fdk_font_destroy(loaded); /* table full: honest degradation */
        return NULL;
    }
    attr_family_slot *table = g_attr_fonts[t].slots;
    int *n_slots = &g_attr_fonts[t].n_slots;

    attr_family_slot *slot = NULL;
    for (int i = 0; i < *n_slots; i++) {
        if (strcmp(table[i].family, key) == 0) {
            slot = &table[i];
            break;
        }
    }
    if (slot == NULL) {
        if (*n_slots >= FDK_ATTR_FAMILY_MAX) {
            fdk_font_destroy(loaded);
            return NULL;
        }
        char *dup = fdk_alloc(strlen(key) + 1);
        if (dup == NULL) {
            fdk_font_destroy(loaded);
            return NULL;
        }
        strcpy(dup, key);
        slot = &table[(*n_slots)++];
        memset(slot, 0, sizeof *slot);
        slot->family = dup;
    }
    for (int i = 0; i < slot->n_sizes; i++) {
        if (slot->sizes[i].size_px == size_px) {
            fdk_font_destroy(loaded);
            return slot->sizes[i].font; /* already cached */
        }
    }
    if (slot->n_sizes >= FDK_ATTR_FAMILY_SIZES) {
        if (slot->sizes[0].font != NULL) {
            fdk_font_destroy(slot->sizes[0].font);
        }
        slot->sizes[0].size_px = size_px;
        slot->sizes[0].font = loaded;
        slot->n_sizes = 1;
        return loaded;
    }
    slot->sizes[slot->n_sizes].size_px = size_px;
    slot->sizes[slot->n_sizes].font = loaded;
    slot->n_sizes++;
    return loaded;
}

/* The decoration pass's font hint (span.c's bars): the run's font
 * when it carries a family/size, NULL (the base) otherwise. */
fdk_font *sh_run_font_hint(fdk_font *base, const fdk__text_run *run) {
    if (base == NULL || run == NULL) {
        return NULL;
    }
    if (run->family == NULL && run->size_px == 0 && run->size_delta == 0) {
        return NULL;
    }
    fdk_i32 size = run->size_px != 0
                       ? run->size_px
                       : base->pixel_size + run->size_delta;
    if (size < 1) {
        size = 1;
    }
    if (size > 512) {
        size = 512;
    }
    if (run->family != NULL) {
        return attr_font_for(base, run->family, size);
    }
    if (size == base->pixel_size) {
        return NULL;
    }
    return fdk__attr_font_for(base, NULL, size, 0, NULL);
}

/* The resolver shaping.c calls (sh_attr_font): family NULL means
 * "the BASE font's own face at size_px" (a size-only run); the
 * source_path is re-loaded at the new size through the same slot
 * machinery (family key = the base font's path). */
fdk_font *fdk__attr_font_for(const fdk_font *base, const char *family,
                             fdk_i32 size_px, fdk_i32 size_delta,
                             fdk_font *fallback) {
    (void)size_delta;
    if (base == NULL || size_px < 1 || size_px > 512) {
        return fallback;
    }
    if (family == NULL) {
        if (size_px == base->pixel_size) {
            return fallback;
        }
        const char *path = fdk_font_get_file_path(base);
        if (path == NULL) {
            return fallback;
        }
        fdk_font *f = fdk_font_load(path, size_px);
        if (f == NULL) {
            return fallback;
        }
        /* Cache it in the table under the base's path so repeated
         * runs at this size hit memory, not the loader. The slot
         * table is per-base-font; the family key here is the path. */
        fdk_font *cached = attr_cache_put(base, path, size_px, f);
        return cached != NULL ? cached : fallback;
    }
    return attr_font_for(base, family, size_px);
}

/* The arena: family/link strings the run array references. One
 * block, laid out back to back, freed by the caller. */
typedef struct attr_arena {
    char *buf;
    size_t len, cap;
    bool oom;
} attr_arena;

static const char *arena_intern(attr_arena *ar, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    if (ar->len + n > ar->cap) {
        size_t cap = ar->cap ? ar->cap * 2 : 128;
        while (cap < ar->len + n) {
            cap *= 2;
        }
        char *grown = fdk_realloc(ar->buf, cap);
        if (grown == NULL) {
            ar->oom = true;
            return NULL;
        }
        ar->buf = grown;
        ar->cap = cap;
    }
    char *at = ar->buf + ar->len;
    memcpy(at, s, n);
    ar->len += n;
    return at;
}

/* Collects boundary points from spans + attrs (the span flattener's
 * discipline: starts snap down, ends snap up). */
static size_t snap_down_b(const char *s, size_t len, size_t i) {
    if (i >= len) {
        return len;
    }
    while (i > 0 && ((((unsigned char)s[i]) & 0xC0u) == 0x80u)) {
        i--;
    }
    return i;
}
static size_t snap_up_b(const char *s, size_t len, size_t i) {
    if (i >= len) {
        return len;
    }
    while (i < len && ((((unsigned char)s[i]) & 0xC0u) == 0x80u)) {
        i++;
    }
    return i;
}

static bool attr_run_equal(const fdk__text_run *a, const fdk__text_run *b) {
    return a->style == b->style && a->color_set == b->color_set &&
           (a->color_set == false ||
            (a->color.r == b->color.r && a->color.g == b->color.g &&
             a->color.b == b->color.b && a->color.a == b->color.a)) &&
           a->underline == b->underline &&
           a->strikethrough == b->strikethrough &&
           a->size_px == b->size_px && a->size_delta == b->size_delta &&
           a->family == b->family && a->bg_set == b->bg_set &&
           (a->bg_set == false ||
            (a->bg.r == b->bg.r && a->bg.g == b->bg.g &&
             a->bg.b == b->bg.b && a->bg.a == b->bg.a)) &&
           a->underline_style == b->underline_style &&
           a->ul_color_set == b->ul_color_set &&
           (a->ul_color_set == false ||
            (a->ul_color.r == b->ul_color.r &&
             a->ul_color.g == b->ul_color.g &&
             a->ul_color.b == b->ul_color.b &&
             a->ul_color.a == b->ul_color.a)) &&
           a->st_color_set == b->st_color_set &&
           (a->st_color_set == false ||
            (a->st_color.r == b->st_color.r &&
             a->st_color.g == b->st_color.g &&
             a->st_color.b == b->st_color.b &&
             a->st_color.a == b->st_color.a)) &&
           a->rise == b->rise &&
           a->letter_spacing == b->letter_spacing &&
           a->link == b->link;
}

fdk_result fdk__attr_flatten(const char *utf8, size_t byte_len,
                             const fdk_span *spans, size_t n_spans,
                             const fdk_attr_list *attrs,
                             unsigned default_style,
                             fdk__text_run **out_runs,
                             size_t *out_run_count, char **out_arena) {
    if (out_runs == NULL || out_run_count == NULL || out_arena == NULL ||
        (spans == NULL && n_spans > 0) ||
        (utf8 == NULL && byte_len > 0) ||
        (attrs != NULL && (attrs->count > 0 && attrs->items == NULL))) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_runs = NULL;
    *out_run_count = 0;
    *out_arena = NULL;

    size_t n_attrs = attrs != NULL ? attrs->count : 0;
    if ((n_spans == 0 && n_attrs == 0) || byte_len == 0) {
        return FDK_OK;
    }

    /* 1. The span pass FIRST (span.c's exact algorithm, producing
     *    the base run stream), then attributes layered per segment.
     *    When there are no spans, synthesize one whole-text gap run
     *    at the default style so the attribute pass has a base. */
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r;
    if (n_spans > 0) {
        r = fdk__span_flatten(utf8, byte_len, spans, n_spans,
                              default_style, &runs, &n_runs);
        if (!fdk_ok(r)) {
            return r;
        }
    } else {
        runs = fdk_alloc(sizeof *runs);
        if (runs == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        memset(runs, 0, sizeof *runs);
        runs->start = 0;
        runs->end = byte_len;
        runs->style = default_style;
        n_runs = 1;
    }

    /* 2. The attribute boundary set: every attr edge (clamped,
     *    snapped) splits the existing runs. Build the full boundary
     *    list over [0, byte_len), then rebuild the run array against
     *    it with per-segment attribute application. */
    size_t max_bounds = 2 * n_attrs + 2;
    size_t *bounds = fdk_alloc_array(max_bounds, sizeof *bounds);
    if (bounds == NULL) {
        if (n_spans == 0) {
            fdk_free(runs);
        } else {
            fdk_free(runs);
        }
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t n_bounds = 0;
    bounds[n_bounds++] = 0;
    bounds[n_bounds++] = byte_len;
    for (size_t k = 0; k < n_attrs; k++) {
        size_t a = attrs->items[k].byte_start;
        size_t b = attrs->items[k].byte_end;
        if (a > byte_len) {
            a = byte_len;
        }
        if (b > byte_len) {
            b = byte_len;
        }
        if (a >= b) {
            continue;
        }
        bounds[n_bounds++] = snap_down_b(utf8, byte_len, a);
        bounds[n_bounds++] = snap_up_b(utf8, byte_len, b);
    }
    for (size_t i = 1; i < n_bounds; i++) {
        size_t v = bounds[i];
        size_t j = i;
        while (j > 0 && bounds[j - 1] > v) {
            bounds[j] = bounds[j - 1];
            j--;
        }
        bounds[j] = v;
    }
    size_t uniq = 0;
    for (size_t i = 0; i < n_bounds; i++) {
        if (i == 0 || bounds[i] != bounds[i - 1]) {
            bounds[uniq++] = bounds[i];
        }
    }

    /* 3. Rebuild: each segment takes its base attributes from the
     *    span run covering it, then applies every attribute whose
     *    range covers the segment (LATER attributes win — the same
     *    contract as spans). */
    fdk__text_run *out = fdk_alloc_array(uniq > 0 ? uniq - 1 : 1,
                                         sizeof *out);
    attr_arena arena = {NULL, 0, 0, false};
    if (out == NULL) {
        fdk_free(bounds);
        fdk_free(runs);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t n_out = 0;
    for (size_t i = 0; i + 1 < uniq && !arena.oom; i++) {
        size_t seg_start = bounds[i];
        size_t seg_end = bounds[i + 1];
        if (seg_start >= seg_end) {
            continue;
        }

        fdk__text_run r2;
        memset(&r2, 0, sizeof r2);
        r2.start = seg_start;
        r2.end = seg_end;
        /* Base: the covering span run. */
        for (size_t k = 0; k < n_runs; k++) {
            if (runs[k].start <= seg_start && runs[k].end >= seg_end) {
                r2 = runs[k];
                r2.start = seg_start;
                r2.end = seg_end;
                break;
            }
        }
        if (n_spans == 0) {
            r2.style = default_style;
        }

        /* Attributes on top. */
        for (size_t k = 0; k < n_attrs; k++) {
            const fdk_attr *at = &attrs->items[k];
            if (at->byte_start > seg_start || at->byte_end < seg_end) {
                continue; /* does not cover the whole segment */
            }
            if (at->byte_start >= at->byte_end) {
                continue;
            }
            switch (at->kind) {
            case FDK_ATTR_SIZE_PT:
                r2.size_px = (fdk_i32)(at->v.f + 0.5f);
                r2.size_delta = 0;
                break;
            case FDK_ATTR_SIZE_DELTA:
                r2.size_delta += (fdk_i32)(at->v.f + 0.5f);
                break;
            case FDK_ATTR_FAMILY:
                r2.family = arena_intern(&arena, at->v.s);
                break;
            case FDK_ATTR_BACKGROUND:
                r2.bg_set = true;
                r2.bg = at->v.color;
                break;
            case FDK_ATTR_FOREGROUND:
                r2.color_set = true;
                r2.color = at->v.color;
                break;
            case FDK_ATTR_STYLE:
                /* ABSOLUTE: the attr names the run's style flags
                 * (the same vocabulary the span engine's style
                 * field speaks). Style attrs compose per-byte the
                 * same way: a covering attr REPLACES the mask on
                 * its range; later-wins on overlap. */
                r2.style = (unsigned)at->v.i;
                break;
            case FDK_ATTR_UNDERLINE:
                r2.underline_style = (unsigned char)at->v.i;
                /* The classic span flag keeps working: underline
                 * with no style attribute renders as SINGLE. */
                if (at->v.i != FDK_UNDERLINE_NONE) {
                    r2.underline = true;
                } else {
                    r2.underline = false;
                }
                break;
            case FDK_ATTR_UNDERLINE_COLOR:
                r2.ul_color_set = true;
                r2.ul_color = at->v.color;
                break;
            case FDK_ATTR_STRIKETHROUGH_COLOR:
                r2.st_color_set = true;
                r2.st_color = at->v.color;
                break;
            case FDK_ATTR_RISE:
                r2.rise = at->v.f;
                break;
            case FDK_ATTR_LETTER_SPACING:
                r2.letter_spacing = at->v.f;
                break;
            case FDK_ATTR_LINK:
                r2.link = arena_intern(&arena, at->v.s);
                /* The affordance: a link underlines itself when the
                 * text did not (the app colors links through spans
                 * or the base color — HTML's rule). */
                if (!r2.underline) {
                    r2.underline = true;
                    r2.underline_style = FDK_UNDERLINE_SINGLE;
                }
                break;
            default:
                break; /* unknown kinds are no-ops (append-only) */
            }
        }

        if (n_out > 0 && attr_run_equal(&out[n_out - 1], &r2)) {
            out[n_out - 1].end = r2.end;
        } else {
            out[n_out++] = r2;
        }
    }

    fdk_free(bounds);
    fdk_free(runs);
    if (arena.oom) {
        fdk_free(out);
        fdk_free(arena.buf);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    *out_runs = out;
    *out_run_count = n_out;
    *out_arena = arena.buf; /* NULL when nothing was interned */
    return FDK_OK;
}

/* ---- public entry points ------------------------------------------------- */

fdk_result fdk_attr_list_init(fdk_attr_list *list) {
    if (list == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
    return FDK_OK;
}

void fdk_attr_list_destroy(fdk_attr_list *list) {
    if (list == NULL) {
        return;
    }
    /* Strings are NOT freed: the documented contract keeps them
     * caller-owned (markup_parse_ex's list owns its copies through
     * its own arena — see below). */
    fdk_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

fdk_result fdk_attr_list_add(fdk_attr_list *list, const fdk_attr *attr) {
    if (list == NULL || attr == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (list->count >= list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 8;
        fdk_attr *grown = fdk_realloc(list->items, cap * sizeof *grown);
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        list->items = grown;
        list->cap = cap;
    }
    list->items[list->count++] = *attr;
    return FDK_OK;
}

fdk_result fdk_attr_list_add_range(fdk_attr_list *list,
                                   size_t byte_start, size_t byte_end,
                                   fdk_attr_kind kind, fdk_f32 f,
                                   fdk_i32 i, fdk_color color,
                                   const char *s) {
    fdk_attr a;
    memset(&a, 0, sizeof a);
    a.byte_start = byte_start;
    a.byte_end = byte_end;
    a.kind = kind;
    switch (kind) {
    case FDK_ATTR_SIZE_PT:
    case FDK_ATTR_SIZE_DELTA:
    case FDK_ATTR_RISE:
    case FDK_ATTR_LETTER_SPACING:
        a.v.f = f;
        break;
    case FDK_ATTR_UNDERLINE:
    case FDK_ATTR_STYLE:
        a.v.i = i;
        break;
    case FDK_ATTR_BACKGROUND:
    case FDK_ATTR_UNDERLINE_COLOR:
    case FDK_ATTR_STRIKETHROUGH_COLOR:
    case FDK_ATTR_FOREGROUND:
        a.v.color = color;
        break;
    case FDK_ATTR_FAMILY:
    case FDK_ATTR_LINK:
        a.v.s = s;
        break;
    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return fdk_attr_list_add(list, &a);
}

/* The markup-owned attribute list: strings interned into an arena
 * hung off the list's `cap`-adjacent storage via a small wrapper —
 * simplest honest shape: the arena is a static-size companion the
 * parse function allocates; we keep it inside the list by making
 * markup's list allocate strings with fdk_alloc and record them for
 * destruction in a parallel array. For v1 the markup list simply
 * LEAKS nothing by keeping its own arena alive until destroy is
 * called with the companion pointer the parse function returns. */

typedef struct markup_arena {
    char *buf;
    size_t len, cap;
    bool oom;
} markup_arena;

static const char *ma_intern(markup_arena *ma, const char *s,
                             size_t n) {
    if (ma->len + n + 1 > ma->cap) {
        size_t cap = ma->cap ? ma->cap * 2 : 128;
        while (cap < ma->len + n + 1) {
            cap *= 2;
        }
        char *grown = fdk_realloc(ma->buf, cap);
        if (grown == NULL) {
            ma->oom = true;
            return NULL;
        }
        ma->buf = grown;
        ma->cap = cap;
    }
    char *at = ma->buf + ma->len;
    memcpy(at, s, n);
    at[n] = '\0';
    ma->len += n + 1;
    return at;
}

/* The one global the markup twin needs: the arena of the list
 * fdk_markup_parse_ex is CURRENTLY filling (the parse is single-
 * threaded, UI-thread-only, and completes before the call returns;
 * the arena is handed to the caller through the list's items'
 * pointers, which is why it must outlive the parse). The destroy
 * responsibility is the caller's, documented in fdk_attr.h's
 * markup section: fdk_markup_attrs_free(). */
static markup_arena g_ma;
static bool g_ma_live;

void fdk_markup_attrs_free(fdk_attr_list *attrs) {
    if (attrs != NULL) {
        fdk_free(attrs->items);
        attrs->items = NULL;
        attrs->count = 0;
        attrs->cap = 0;
    }
    fdk_free(g_ma.buf);
    g_ma.buf = NULL;
    g_ma.len = 0;
    g_ma.cap = 0;
    g_ma.oom = false;
    g_ma_live = false;
}

/* Called by markup.c's scanner (attr emission path). Returns the
 * interned pointer or NULL on OOM (the parse fails loudly). */
const char *fdk__attr_intern(const char *s, size_t n) {
    if (!g_ma_live) {
        g_ma_live = true;
    }
    return ma_intern(&g_ma, s, n);
}

/* ---- the attributed twins ------------------------------------------------ */

fdk_result fdk_font_measure_attrs_utf8(const fdk_font *font,
                                       const char *utf8, size_t byte_len,
                                       const fdk_span *spans,
                                       size_t span_count,
                                       const fdk_attr_list *attrs,
                                       fdk_text_metrics *out) {
    if (font == NULL || utf8 == NULL || out == NULL ||
        (spans == NULL && span_count > 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((spans == NULL || span_count == 0) &&
        (attrs == NULL || attrs->count == 0)) {
        return fdk_font_measure_utf8(font, utf8, byte_len, out);
    }

    char *arena = NULL;
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__attr_flatten(utf8, byte_len, spans, span_count,
                                     attrs, fdk_font_get_style(font),
                                     &runs, &n_runs, &arena);
    if (fdk_ok(r)) {
        r = fdk__measure_runs(font, utf8, byte_len, runs, n_runs,
                              byte_len, out);
    }
    fdk_free(runs);
    fdk_free(arena);
    return r;
}

fdk_result fdk_surface_draw_attrs_utf8(fdk_surface *surface,
                                       fdk_font *font, const char *utf8,
                                       size_t byte_len,
                                       const fdk_span *spans,
                                       size_t span_count,
                                       const fdk_attr_list *attrs,
                                       fdk_i32 pen_x, fdk_i32 baseline_y,
                                       fdk_color base_color) {
    if (surface == NULL || font == NULL || utf8 == NULL ||
        (spans == NULL && span_count > 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((spans == NULL || span_count == 0) &&
        (attrs == NULL || attrs->count == 0)) {
        return fdk_surface_draw_utf8(surface, font, utf8, byte_len,
                                     pen_x, baseline_y, base_color);
    }

    char *arena = NULL;
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__attr_flatten(utf8, byte_len, spans, span_count,
                                     attrs, fdk_font_get_style(font),
                                     &runs, &n_runs, &arena);
    if (fdk_ok(r)) {
        r = fdk__draw_attrs_runs(surface, font, utf8, byte_len, runs,
                                 n_runs, 0, byte_len, pen_x, baseline_y,
                                 base_color);
    }
    fdk_free(runs);
    fdk_free(arena);
    return r;
}

fdk_result fdk_font_break_lines_attrs_utf8(
    const fdk_font *font, const char *utf8, size_t byte_len,
    const fdk_span *spans, size_t span_count,
    const fdk_attr_list *attrs, fdk_i32 max_width,
    fdk_text_line *out_lines, size_t max_lines,
    size_t *out_line_count, bool *out_truncated) {
    if (font == NULL || utf8 == NULL || out_lines == NULL ||
        out_line_count == NULL || out_truncated == NULL ||
        (spans == NULL && span_count > 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((spans == NULL || span_count == 0) &&
        (attrs == NULL || attrs->count == 0)) {
        return fdk_font_break_lines_utf8(font, utf8, byte_len,
                                         max_width, out_lines, max_lines,
                                         out_line_count, out_truncated);
    }
    char *arena = NULL;
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__attr_flatten(utf8, byte_len, spans, span_count,
                                     attrs, fdk_font_get_style(font),
                                     &runs, &n_runs, &arena);
    if (fdk_ok(r)) {
        r = fdk__break_lines_runs(font, utf8, byte_len, runs, n_runs,
                            max_width, out_lines, max_lines,
                            out_line_count, out_truncated);
    }
    fdk_free(runs);
    fdk_free(arena);
    return r;
}

fdk_result fdk_font_ellipsize_attrs_utf8(
    const fdk_font *font, const char *utf8, size_t byte_len,
    const fdk_span *spans, size_t span_count,
    const fdk_attr_list *attrs, fdk_i32 max_width,
    size_t *out_prefix_bytes, bool *out_fits) {
    if (font == NULL || utf8 == NULL || out_prefix_bytes == NULL ||
        out_fits == NULL || (spans == NULL && span_count > 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((spans == NULL || span_count == 0) &&
        (attrs == NULL || attrs->count == 0)) {
        return fdk_font_ellipsize_utf8(font, utf8, byte_len, max_width,
                                       out_prefix_bytes, out_fits);
    }
    char *arena = NULL;
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__attr_flatten(utf8, byte_len, spans, span_count,
                                     attrs, fdk_font_get_style(font),
                                     &runs, &n_runs, &arena);
    if (fdk_ok(r)) {
        r = fdk__ellipsize_runs(font, utf8, byte_len, runs, n_runs,
                                max_width, out_prefix_bytes, out_fits);
    }
    fdk_free(runs);
    fdk_free(arena);
    return r;
}

/* ---- link hit-testing ----------------------------------------------------- */

bool fdk_attrs_link_at(const fdk_font *font, const char *utf8,
                       size_t byte_len,
                       const fdk_span *spans, size_t span_count,
                       const fdk_attr_list *attrs,
                       fdk_i32 pen_x, fdk_i32 baseline_y,
                       fdk_i32 x, fdk_i32 y,
                       char *out_href, size_t cap) {
    if (font == NULL || utf8 == NULL || attrs == NULL ||
        attrs->count == 0 || out_href == NULL || cap == 0) {
        return false;
    }
    out_href[0] = '\0';
    char *arena = NULL;
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__attr_flatten(utf8, byte_len, spans, span_count,
                                     attrs, fdk_font_get_style(font),
                                     &runs, &n_runs, &arena);
    if (!fdk_ok(r)) {
        return false;
    }
    const char *hit = NULL; /* the interned href (arena-owned) */
    /* The link's box: its run's advance span, the font's line box
     * (ascent above the baseline, descent below). Coarse by design
     * — a link's target area is its whole line box, like HTML. */
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk__shaped *s = fdk__shape_range(fdk_text_font_mutable(font), utf8,
                                      byte_len, 0, byte_len, runs, n_runs);
    if (s == NULL) {
        fdk_free(runs);
        fdk_free(arena);
        return false;
    }
    if (y <= baseline_y + fm.descent && y >= baseline_y - fm.ascent) {
        for (size_t k = 0; k < n_runs && hit == NULL; k++) {
            if (runs[k].link == NULL) {
                continue;
            }
            /* The run's x extent: the FIRST in-range cp's start pen
             * to the LAST in-range cp's end pen. */
            fdk_f32 x0 = 0, x1 = 0;
            bool got = false;
            for (size_t c = 0; c < s->n_cp; c++) {
                if (s->cp_byte[c] >= runs[k].start &&
                    s->cp_byte[c] < runs[k].end) {
                    if (!got) {
                        x0 = s->cp_pen[c];
                        got = true;
                    }
                    x1 = s->cp_pen[c + 1];
                }
            }
            if (got && x >= pen_x + (fdk_i32)(x0 + 0.5f) &&
                x < pen_x + (fdk_i32)(x1 + 0.5f)) {
                hit = runs[k].link;
            }
        }
    }
    fdk__shaped_free(s);
    fdk_free(runs);
    /* Copy BEFORE freeing the arena — `hit` points into it. */
    if (hit == NULL) {
        fdk_free(arena);
        return false;
    }
    strncpy(out_href, hit, cap - 1);
    out_href[cap - 1] = '\0';
    fdk_free(arena);
    return true;
}
