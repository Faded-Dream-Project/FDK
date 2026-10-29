/*
 * shaping.c — M-2 level 1: complex-script text shaping (1.1.0).
 *
 * The gap this closes (ROADMAP B-02): before this file, every FDK
 * text walk was codepoint-at-a-time LTR with pair kerning — Arabic,
 * Hebrew, Persian, and Urdu rendered in LOGICAL order (backwards),
 * Indic conjuncts came out malformed, mixed-script strings needed
 * manual font switching, and unmapped codepoints painted .notdef.
 *
 * The posture (the GLX/fontconfig precedent, D-002's discipline):
 *
 *   HarfBuzz and FriBidi are RUNTIME-optional, dlopen'd once per
 *   process, never dlclose'd, and NEVER a build-time dependency.
 *   A machine without them gets today's exact walk — bit for bit,
 *   the same arithmetic the four walks have always used. A machine
 *   with them gets the full pipeline: FriBidi resolves the
 *   paragraph direction and reorders runs to visual order (UAX #9,
 *   with bracket mirroring), HarfBuzz shapes each segment through
 *   its real GSUB/GPOS tables (ligatures, contextual forms, mark
 *   attachment, proper kerning), and fontconfig supplies a
 *   per-script fallback font when the primary font has no glyphs
 *   for a script.
 *
 *   $FDK_SHAPING pins it: "0" forces the legacy walk (tests,
 *   diagnostics), "1" REQUIRES the libraries (a loud error, then
 *   legacy, if they are missing), unset is the auto probe.
 *
 * The single seam everything flows through: a shaped range becomes
 * an array of PLACEMENTS — one per output glyph, in visual order,
 * each carrying its rasterized glyph (the same styled, subpixel-
 * phase-baked cache the legacy walk uses), its left-edge x, its
 * cumulative float pen, and the byte cluster it came from. Measure
 * sums placements; draw blends them; the wrap/ellipsize engines and
 * the Entry caret read the per-codepoint logical pen attribution.
 * There is still exactly ONE rounding rule in the text stack: the
 * pen floors per glyph and rounds once at the end.
 *
 * Level-1 scope, honestly stated:
 *   - line breaking stays LOGICAL (spaces and hard breaks); each
 *     emitted line is its own visual paragraph — the standard
 *     level-1 toolkit behavior;
 *   - kerning context does not cross segment boundaries (script,
 *     direction, style, or font changes), which today's walks
 *     already enforce at style boundaries;
 *   - the Entry caret maps through the logical pen; pure-LTR text
 *     (including CJK) is exact, mixed-direction text lands the
 *     caret at segment granularity;
 *   - color emoji stays out of scope (C-3, after HarfBuzz).
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- 1. The vendored ABI declarations ------------------------------
 *
 * No system headers are included: the libraries are dlopen'd, so
 * their types and constants are re-declared here, byte-compatible
 * with the installed headers (verified against harfbuzz 10.2 and
 * fribidi 1.0.16; both sets are stable ABI since harfbuzz 2.0,
 * 2017). If a future major breaks one of these, the probe fails,
 * the symbol table stays NULL, and FDK silently keeps the legacy
 * walk — that is the whole point of the posture. */

typedef struct hb_blob_t hb_blob_t;
typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t;
typedef struct hb_font_funcs_t hb_font_funcs_t;
typedef struct hb_buffer_t hb_buffer_t;
typedef struct hb_feature_t hb_feature_t;

typedef int hb_bool_t;
typedef int hb_codepoint_t;
typedef int hb_position_t;
typedef unsigned int hb_mask_t;
typedef uint32_t hb_var_int_t_u32;
typedef int32_t hb_var_int_t_i32;

typedef struct hb_glyph_extents_t {
    hb_position_t x_bearing;
    hb_position_t y_bearing;
    hb_position_t width;
    hb_position_t height;
} hb_glyph_extents_t;

typedef struct hb_glyph_info_t {
    hb_codepoint_t codepoint;
    hb_mask_t mask;
    uint32_t cluster;
    uint32_t var1;
    uint32_t var2;
} hb_glyph_info_t;

typedef struct hb_glyph_position_t {
    hb_position_t x_advance;
    hb_position_t y_advance;
    hb_position_t x_offset;
    hb_position_t y_offset;
    uint32_t var;
} hb_glyph_position_t;

/* hb_memory_mode_t */ 
#define SH_HB_MEMORY_READONLY 1
/* hb_buffer_cluster_level_t */
#define SH_HB_CLUSTER_MONOTONE_CHARACTERS 1
/* hb_direction_t */
#define SH_HB_DIRECTION_LTR 4
#define SH_HB_DIRECTION_RTL 5

/* The font-funcs callbacks (harfbuzz >= 2.0 signatures — the
 * user_data parameters exist since 2.0, 2017). */
typedef hb_bool_t (*sh_hb_nominal_glyph_fn)(
    hb_font_t *font, void *font_data, hb_codepoint_t unicode,
    hb_codepoint_t *glyph, void *user_data);
typedef hb_position_t (*sh_hb_h_advance_fn)(
    hb_font_t *font, void *font_data, hb_codepoint_t glyph,
    void *user_data);
typedef hb_bool_t (*sh_hb_extents_fn)(
    hb_font_t *font, void *font_data, hb_codepoint_t glyph,
    hb_glyph_extents_t *extents, void *user_data);
typedef void (*sh_hb_destroy_fn)(void *user_data);

typedef struct {
    hb_blob_t *(*hb_blob_create)(const char *data, unsigned int length,
                                 int mode, void *user_data,
                                 sh_hb_destroy_fn destroy);
    void (*hb_blob_destroy)(hb_blob_t *blob);
    hb_face_t *(*hb_face_create)(hb_blob_t *blob, unsigned int index);
    void (*hb_face_destroy)(hb_face_t *face);
    hb_font_t *(*hb_font_create)(hb_face_t *face);
    void (*hb_font_destroy)(hb_font_t *font);
    void (*hb_font_set_scale)(hb_font_t *font, int x, int y);
    hb_font_funcs_t *(*hb_font_funcs_create)(void);
    void (*hb_font_funcs_destroy)(hb_font_funcs_t *ffuncs);
    void (*hb_font_funcs_set_nominal_glyph_func)(
        hb_font_funcs_t *ffuncs, sh_hb_nominal_glyph_fn func,
        void *user_data, sh_hb_destroy_fn destroy);
    void (*hb_font_funcs_set_glyph_h_advance_func)(
        hb_font_funcs_t *ffuncs, sh_hb_h_advance_fn func,
        void *user_data, sh_hb_destroy_fn destroy);
    void (*hb_font_funcs_set_glyph_extents_func)(
        hb_font_funcs_t *ffuncs, sh_hb_extents_fn func,
        void *user_data, sh_hb_destroy_fn destroy);
    void (*hb_font_set_funcs)(hb_font_t *font, hb_font_funcs_t *klass,
                              void *font_data,
                              sh_hb_destroy_fn destroy);
    hb_buffer_t *(*hb_buffer_create)(void);
    void (*hb_buffer_destroy)(hb_buffer_t *buffer);
    void (*hb_buffer_reset)(hb_buffer_t *buffer);
    hb_bool_t (*hb_buffer_add_utf8)(hb_buffer_t *buffer,
                                    const char *text, int text_length,
                                    unsigned int item_offset,
                                    int item_length);
    hb_bool_t (*hb_buffer_add_utf32)(hb_buffer_t *buffer,
                                     const uint32_t *text,
                                     int text_length,
                                     unsigned int item_offset,
                                     int item_length);
    void (*hb_buffer_set_script)(hb_buffer_t *buffer, uint32_t script);
    void (*hb_buffer_set_direction)(hb_buffer_t *buffer, int direction);
    void (*hb_buffer_set_language)(hb_buffer_t *buffer,
                                   const void *language);
    void (*hb_buffer_set_cluster_level)(hb_buffer_t *buffer,
                                        unsigned int cluster_level);
    void (*hb_buffer_guess_segment_properties)(hb_buffer_t *buffer);
    void (*hb_shape)(hb_font_t *font, hb_buffer_t *buffer,
                     const hb_feature_t *features,
                     unsigned int num_features);
    unsigned int (*hb_buffer_get_length)(const hb_buffer_t *buffer);
    hb_glyph_info_t *(*hb_buffer_get_glyph_infos)(
        hb_buffer_t *buffer, unsigned int *length);
    hb_glyph_position_t *(*hb_buffer_get_glyph_positions)(
        hb_buffer_t *buffer, unsigned int *length);
    uint32_t (*hb_script_from_string)(const char *str, int len);
    const void *(*hb_language_from_string)(const char *str, int len);
} sh_hb_api;

/* FriBidi (types from fribidi-types.h / fribidi-bidi-types.h):
 * FriBidiChar is uint32_t, FriBidiStrIndex is int, FriBidiLevel is
 * signed char, the type/par enums are uint32_t, and the paragraph
 * direction constants are composite masks. */
typedef uint32_t sh_fb_char;
typedef int sh_fb_index;
typedef signed char sh_fb_level;
typedef uint32_t sh_fb_chartype;
typedef uint32_t sh_fb_partype;
typedef int sh_fb_boolean;

#define SH_FB_PAR_ON 0x00000040u /* FRIBIDI_MASK_NEUTRAL */

typedef struct {
    sh_fb_level (*fribidi_log2vis)(
        const sh_fb_char *str, const sh_fb_index len,
        sh_fb_partype *pbase_dir, sh_fb_char *visual_str,
        sh_fb_index *positions_L_to_V, sh_fb_index *positions_V_to_L,
        sh_fb_level *embedding_levels);
    sh_fb_boolean (*fribidi_get_mirror_char)(sh_fb_char ch,
                                             sh_fb_char *mirrored_ch);
} sh_fb_api;

/* ---- 2. The dlopen layer ------------------------------------------ */

static sh_hb_api g_hb;
static sh_fb_api g_fb;
static void *g_hb_lib;
static void *g_fb_lib;
static int g_shaping_state; /* 0 unknown, 1 active, -1 legacy */

/* The symbol tables. Any missing symbol disables the whole library
 * (one INFO line — the legacy walk is the documented default, not
 * an error). Both handles stay mapped for the process lifetime:
 * like fontconfig, their global caches (the hb shape planner,
 * fribidi's type tables) are shared process state FDK refuses to
 * rip out from under a host that may be using them itself. */
static bool sh_dlsym_all(void *lib, const char *const *names,
                         void ***out_fns, size_t n) {
    for (size_t i = 0; i < n; i++) {
        *out_fns[i] = dlsym(lib, names[i]);
        if (*out_fns[i] == NULL) {
            return false;
        }
    }
    return true;
}

static void sh_probe(void) {
    if (g_shaping_state != 0) {
        return;
    }
    const char *pin = getenv("FDK_SHAPING");
    if (pin != NULL && pin[0] == '0') {
        g_shaping_state = -1;
        FDK_INFO("shaping: pinned off ($FDK_SHAPING=0) — the legacy "
                 "codepoint walk");
        return;
    }

    /* Debian spells the runtime soname libharfbuzz.so.0b; everyone
     * else libharfbuzz.so.0. FriBidi is libfribidi.so.0 everywhere. */
    static const char *const hb_sonames[] = {"libharfbuzz.so.0",
                                             "libharfbuzz.so.0b"};
    for (size_t i = 0; i < sizeof hb_sonames / sizeof hb_sonames[0];
         i++) {
        g_hb_lib = dlopen(hb_sonames[i], RTLD_NOW | RTLD_LOCAL);
        if (g_hb_lib != NULL) {
            break;
        }
    }
    g_fb_lib = dlopen("libfribidi.so.0", RTLD_NOW | RTLD_LOCAL);

    if (g_hb_lib == NULL || g_fb_lib == NULL) {
        if (pin != NULL && pin[0] == '1') {
            FDK_ERROR("shaping: $FDK_SHAPING=1 but harfbuzz/fribidi "
                      "are not loadable — falling back to the legacy "
                      "walk");
        } else {
            FDK_INFO("shaping: harfbuzz/fribidi not present — the "
                     "legacy codepoint walk (complex scripts will "
                     "render in logical order)");
        }
        g_shaping_state = -1;
        return;
    }

    static const char *const hb_names[] = {
        "hb_blob_create", "hb_blob_destroy", "hb_face_create",
        "hb_face_destroy", "hb_font_create", "hb_font_destroy",
        "hb_font_set_scale", "hb_font_funcs_create",
        "hb_font_funcs_destroy", "hb_font_funcs_set_nominal_glyph_func",
        "hb_font_funcs_set_glyph_h_advance_func",
        "hb_font_funcs_set_glyph_extents_func", "hb_font_set_funcs",
        "hb_buffer_create", "hb_buffer_destroy", "hb_buffer_reset",
        "hb_buffer_add_utf8", "hb_buffer_add_utf32",
        "hb_buffer_set_script",
        "hb_buffer_set_direction", "hb_buffer_set_language",
        "hb_buffer_set_cluster_level",
        "hb_buffer_guess_segment_properties", "hb_shape",
        "hb_buffer_get_length", "hb_buffer_get_glyph_infos",
        "hb_buffer_get_glyph_positions", "hb_script_from_string",
        "hb_language_from_string",
    };
    void **hb_fns[] = {
        (void **)&g_hb.hb_blob_create, (void **)&g_hb.hb_blob_destroy,
        (void **)&g_hb.hb_face_create, (void **)&g_hb.hb_face_destroy,
        (void **)&g_hb.hb_font_create, (void **)&g_hb.hb_font_destroy,
        (void **)&g_hb.hb_font_set_scale,
        (void **)&g_hb.hb_font_funcs_create,
        (void **)&g_hb.hb_font_funcs_destroy,
        (void **)&g_hb.hb_font_funcs_set_nominal_glyph_func,
        (void **)&g_hb.hb_font_funcs_set_glyph_h_advance_func,
        (void **)&g_hb.hb_font_funcs_set_glyph_extents_func,
        (void **)&g_hb.hb_font_set_funcs,
        (void **)&g_hb.hb_buffer_create,
        (void **)&g_hb.hb_buffer_destroy,
        (void **)&g_hb.hb_buffer_reset,
        (void **)&g_hb.hb_buffer_add_utf8,
        (void **)&g_hb.hb_buffer_add_utf32,
        (void **)&g_hb.hb_buffer_set_script,
        (void **)&g_hb.hb_buffer_set_direction,
        (void **)&g_hb.hb_buffer_set_language,
        (void **)&g_hb.hb_buffer_set_cluster_level,
        (void **)&g_hb.hb_buffer_guess_segment_properties,
        (void **)&g_hb.hb_shape, (void **)&g_hb.hb_buffer_get_length,
        (void **)&g_hb.hb_buffer_get_glyph_infos,
        (void **)&g_hb.hb_buffer_get_glyph_positions,
        (void **)&g_hb.hb_script_from_string,
        (void **)&g_hb.hb_language_from_string,
    };
    static const char *const fb_names[] = {"fribidi_log2vis",
                                           "fribidi_get_mirror_char"};
    void **fb_fns[] = {(void **)&g_fb.fribidi_log2vis,
                       (void **)&g_fb.fribidi_get_mirror_char};

    if (!sh_dlsym_all(g_hb_lib, hb_names, hb_fns,
                      sizeof hb_names / sizeof hb_names[0]) ||
        !sh_dlsym_all(g_fb_lib, fb_names, fb_fns,
                      sizeof fb_names / sizeof fb_names[0])) {
        FDK_INFO("shaping: harfbuzz/fribidi lack the expected ABI — "
                 "the legacy codepoint walk");
        g_shaping_state = -1;
        return;
    }
    g_shaping_state = 1;
    FDK_INFO("shaping: harfbuzz + fribidi active — full complex-script "
             "pipeline (bidi, ligatures, per-script fallback)");
}

int fdk_text_shaping_available(void) {
    sh_probe();
    return g_shaping_state == 1;
}

/* ---- 3. The script table -------------------------------------------
 *
 * One compact range table for the scripts a real desktop meets
 * (the classic "level 1" set GTK/Qt ship for). Each script carries
 * its HarfBuzz tag and a BCP-47-ish language string for both the
 * shaper and fontconfig's FC_LANG fallback query. Codepoints in no
 * range are "common" (punctuation, spaces, digits) — they inherit
 * the surrounding segment's script and never force a boundary. */

typedef struct {
    fdk_u32 lo, hi;
    int script; /* index into sh_scripts */
} sh_range;

typedef struct {
    const char *hb_tag;  /* HarfBuzz script tag            */
    const char *fc_lang; /* fontconfig FC_LANG fallback    */
    const char *name;    /* diagnostics                    */
} sh_script_def;

static const sh_script_def sh_scripts[] = {
    {"Latn", "en", "Latin"},     {"Grek", "el", "Greek"},
    {"Cyrl", "ru", "Cyrillic"},  {"Armn", "hy", "Armenian"},
    {"Hebr", "he", "Hebrew"},    {"Arab", "ar", "Arabic"},
    {"Syrc", "sy", "Syriac"},    {"Thaa", "dv", "Thaana"},
    {"Deva", "hi", "Devanagari"},{"Beng", "bn", "Bengali"},
    {"Guru", "pa", "Gurmukhi"},  {"Gujr", "gu", "Gujarati"},
    {"Orya", "or", "Oriya"},     {"Taml", "ta", "Tamil"},
    {"Telu", "te", "Telugu"},    {"Knda", "kn", "Kannada"},
    {"Mlym", "ml", "Malayalam"}, {"Sinh", "si", "Sinhala"},
    {"Thai", "th", "Thai"},      {"Laoo", "lo", "Lao"},
    {"Tibt", "bo", "Tibetan"},   {"Mymr", "my", "Myanmar"},
    {"Geor", "ka", "Georgian"},  {"Ethi", "am", "Ethiopic"},
    {"Khmr", "km", "Khmer"},     {"Hani", "zh", "Han"},
    {"Hira", "ja", "Hiragana"},  {"Kana", "ja", "Katakana"},
    {"Hang", "ko", "Hangul"},
    {"Zsye", "und-zsye", "Emoji"}, /* C-3: the color-glyph bucket —
                                       fontconfig's registered emoji
                                       lang; the charset filter (probe
                                       codepoint) does the decisive
                                       work, the lang only ranks */
};
#define SH_N_SCRIPTS ((int)(sizeof sh_scripts / sizeof sh_scripts[0]))
#define SH_SCRIPT_COMMON (-1)
#define SH_SCRIPT_UNKNOWN (-2)

static const sh_range sh_ranges[] = {
    {0x0041, 0x005A, 0},  {0x0061, 0x007A, 0},  {0x00AA, 0x00AA, 0},
    {0x00BA, 0x00BA, 0},  {0x00C0, 0x00FF, 0},  {0x0100, 0x02AF, 0},
    {0x1E00, 0x1EFF, 0},  {0x2C60, 0x2C7F, 0},  {0xA720, 0xA7FF, 0},
    {0x0370, 0x03FF, 1},  {0x1F00, 0x1FFF, 1},
    {0x0400, 0x052F, 2},  {0x2DE0, 0x2DFF, 2},  {0xA640, 0xA69F, 2},
    {0x0530, 0x058F, 3},
    {0x0590, 0x05FF, 4},  {0xFB1D, 0xFB4F, 4},
    {0x0600, 0x06FF, 5},  {0x0750, 0x077F, 5},  {0x08A0, 0x08FF, 5},
    {0xFB50, 0xFDFF, 5},  {0xFE70, 0xFEFF, 5},
    {0x0700, 0x074F, 6},
    {0x0780, 0x07BF, 7},
    {0x0900, 0x097F, 8},  {0x0980, 0x09FF, 9},  {0x0A00, 0x0A7F, 10},
    {0x0A80, 0x0AFF, 11}, {0x0B00, 0x0B7F, 12}, {0x0B80, 0x0BFF, 13},
    {0x0C00, 0x0C7F, 14}, {0x0C80, 0x0CFF, 15}, {0x0D00, 0x0D7F, 16},
    {0x0D80, 0x0DFF, 17},
    {0x0E00, 0x0E7F, 18}, {0x0E80, 0x0EFF, 19},
    {0x0F00, 0x0FFF, 20}, {0x1000, 0x109F, 21},
    {0x10A0, 0x10FF, 22}, {0x2D00, 0x2D2F, 22},
    {0x1200, 0x137F, 23},
    {0x1780, 0x17FF, 24},
    {0x2E80, 0x2FDF, 25}, {0x3004, 0x3004, 25}, {0x3006, 0x3007, 25},
    {0x3021, 0x3029, 25}, {0x3038, 0x303B, 25},
    {0x3400, 0x4DBF, 25}, {0x4E00, 0x9FFF, 25}, {0xF900, 0xFAFF, 25},
    {0x20000, 0x2A6DF, 25}, {0x2A700, 0x2EBEF, 25},
    {0x2F800, 0x2FA1F, 25},
    {0x3041, 0x309F, 26}, {0x1B001, 0x1B0FF, 26},
    {0x30A0, 0x30FF, 27}, {0x31F0, 0x31FF, 27},
    {0x1100, 0x11FF, 28}, {0x3130, 0x318F, 28}, {0xA960, 0xA97F, 28},
    {0xAC00, 0xD7FF, 28},
    /* Emoji (script 29): the Extended_Pictographic core — the
     * blocks the emoji presentation sequences live in. Symbols in
     * 2600-27BF whose default is TEXT presentation still resolve to
     * the primary font when it covers them (the fdk__font_covers
     * gate); VS16 (FE0F) stays "common" and inherits its segment. */
    {0x2600, 0x27BF, 29}, {0x2B00, 0x2B55, 29},
    {0x1F000, 0x1FAFF, 29}, {0x1FB00, 0x1FBFF, 29},
};

static int sh_script_of(fdk_u32 cp) {
    size_t lo = 0;
    size_t hi = sizeof sh_ranges / sizeof sh_ranges[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < sh_ranges[mid].lo) {
            hi = mid;
        } else if (cp > sh_ranges[mid].hi) {
            lo = mid + 1;
        } else {
            return sh_ranges[mid].script;
        }
    }
    return SH_SCRIPT_COMMON;
}

/* ---- 4. Bracket mirroring -----------------------------------------
 *
 * FriBidi's log2vis does not mirror brackets itself (that is
 * fribidi_shape's job); we mirror at the codepoint level before
 * shaping, exactly once per RTL-level character. fribidi's own
 * fribidi_get_mirror_char is used when the symbol resolved (it is
 * in the required table above); this built-in table is the safety
 * net for the pairs that matter on real desktops. */
typedef struct {
    fdk_u32 open, close;
} sh_mirror_pair;

static const sh_mirror_pair sh_mirror_table[] = {
    {0x28, 0x29},        /* ( )    */
    {0x5B, 0x5D},        /* [ ]    */
    {0x7B, 0x7D},        /* { }    */
    {0x3C, 0x3E},        /* < >    */
    {0xAB, 0xBB},        /* << >>  */
    {0x2039, 0x203A},    /* < >    */
    {0x2018, 0x2019},    /* ' '    */
    {0x201C, 0x201D},    /* " "    */
    {0x3008, 0x3009},    /* CJK < > */
    {0x300A, 0x300B},    /* CJK << >> */
    {0x300C, 0x300D},    /* CJK corner */
    {0x300E, 0x300F},
    {0x3010, 0x3011},    /* CJK black lenticular */
    {0x3014, 0x3015},
    {0xFF08, 0xFF09},    /* fullwidth ( ) */
    {0xFF3B, 0xFF3D},    /* fullwidth [ ] */
    {0xFF5B, 0xFF5D},    /* fullwidth { } */
};

static fdk_u32 sh_mirror_cp(fdk_u32 cp) {
    if (g_fb.fribidi_get_mirror_char != NULL) {
        sh_fb_char out = 0;
        if (g_fb.fribidi_get_mirror_char((sh_fb_char)cp, &out)) {
            return (fdk_u32)out;
        }
        return cp;
    }
    for (size_t i = 0; i < sizeof sh_mirror_table /
                                 sizeof sh_mirror_table[0]; i++) {
        if (sh_mirror_table[i].open == cp) {
            return sh_mirror_table[i].close;
        }
        if (sh_mirror_table[i].close == cp) {
            return sh_mirror_table[i].open;
        }
    }
    return cp;
}

/* ---- 5. The HarfBuzz font: stb-backed font funcs -------------------
 *
 * HarfBuzz needs (a) the raw font bytes, for its own GSUB/GPOS
 * table walk, and (b) glyph metrics: nominal glyph lookup, advances,
 * and extents. (b) is answered from stb_truetype — the same numbers
 * the legacy walk uses, so a shaped run and a legacy walk of the
 * same glyph agree on every advance by construction.
 *
 * Scaling: FDK's pixel size is stbtt_ScaleForPixelHeight's box (the
 * ascent+descent span), not the em square. The funcs below return
 * values PRE-SCALED to 26.6 pixels (value = units * scale * 64, so
 * position = raw advance in font units * scale, at 1/64 px
 * granularity — the subpixel pipeline stays float-accurate). For
 * HarfBuzz's own GPOS math to land in the same units,
 * hb_font_set_scale gets scale*64*upem, because HarfBuzz divides
 * GPOS font-unit values by upem and multiplies by the scale. */

typedef struct {
    hb_blob_t *blob;
    hb_face_t *face;
    hb_font_t *font;
    unsigned int upem;
} sh_hb_font_cache;

static int sh_round_half(fdk_f32 v) {
    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* units -> 26.6 pixels */
static hb_position_t sh_units_to_26_6(const fdk_font *f, int units) {
    return (hb_position_t)sh_round_half((fdk_f32)units * f->scale *
                                        64.0f);
}

static hb_bool_t sh_fn_nominal_glyph(hb_font_t *font, void *font_data,
                                     hb_codepoint_t unicode,
                                     hb_codepoint_t *glyph,
                                     void *user_data) {
    (void)font;
    (void)user_data;
    const fdk_font *f = font_data;
    *glyph = (hb_codepoint_t)stbtt_FindGlyphIndex(&f->info,
                                                   (int)unicode);
    return 1; /* glyph 0 (.notdef) is a valid answer */
}

static hb_position_t sh_fn_h_advance(hb_font_t *font, void *font_data,
                                     hb_codepoint_t glyph,
                                     void *user_data) {
    (void)font;
    (void)user_data;
    const fdk_font *f = font_data;
    int advance_raw = 0;
    stbtt_GetGlyphHMetrics(&f->info, (int)glyph, &advance_raw, NULL);
    return sh_units_to_26_6(f, advance_raw);
}

static hb_bool_t sh_fn_extents(hb_font_t *font, void *font_data,
                               hb_codepoint_t glyph,
                               hb_glyph_extents_t *extents,
                               void *user_data) {
    (void)font;
    (void)user_data;
    const fdk_font *f = font_data;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    /* CFF-flavored faces have no glyf table, so stbtt_GetGlyphBox
     * cannot answer for them — the native box (same ink fields a
     * glyf xMin..yMax box covers) steps in (src/text/cff.c). */
    bool have;
    if (f->bitmap_only ||
        (f->emoji != NULL &&
         fdk__emoji_is_color_glyph(f->emoji, (int)glyph))) {
        /* C-3: color glyphs (and every glyph of a bitmap-only face)
         * take their box from the emoji module — strikes or clip
         * boxes, in font units like every other box here. */
        have = fdk__emoji_glyph_box(f->emoji, &f->info, f->cff,
                                    (int)glyph, &x0, &y0, &x1, &y1);
    } else if (f->cff != NULL) {
        /* CFF-flavored faces have no glyf table, so
         * stbtt_GetGlyphBox cannot answer for them — the native box
         * (same ink fields a glyf xMin..yMax box covers) steps in
         * (src/text/cff.c). */
        have = fdk__cff_glyph_box(f->cff, (int)glyph, &x0, &y0, &x1,
                                  &y1);
    } else {
        have = stbtt_GetGlyphBox(&f->info, (int)glyph, &x0, &y0, &x1,
                                 &y1) != 0;
    }
    if (!have) {
        memset(extents, 0, sizeof *extents);
        return 0;
    }
    extents->x_bearing = sh_units_to_26_6(f, x0);
    extents->y_bearing = sh_units_to_26_6(f, y1);
    extents->width = sh_units_to_26_6(f, x1 - x0);
    extents->height = sh_units_to_26_6(f, y0 - y1);
    return 1;
}

/* The shared funcs table (created once, installed on every font
 * with the fdk_font as font_data — no per-font destroy needed). */
static hb_font_funcs_t *g_hb_ffuncs;

/* Lazily builds the per-font HarfBuzz objects, cached on the font
 * (font->shaper). The blob references font->file_data READ-ONLY; the
 * cache is destroyed by fdk__shaping_font_destroy() BEFORE the file
 * data is freed. Returns NULL when anything fails (allocation, or
 * HarfBuzz rejects the face) — the segment then shapes via the
 * legacy path. */
static hb_font_t *sh_hb_font_for(fdk_font *font) {
    sh_probe();
    if (g_shaping_state != 1 || font == NULL) {
        return NULL;
    }
    sh_hb_font_cache *c = font->shaper;
    if (c != NULL) {
        return c->font;
    }
    if (font->file_size == 0) {
        return NULL; /* older font objects without a size: refuse */
    }

    c = fdk_alloc(sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    memset(c, 0, sizeof *c);

    c->blob = g_hb.hb_blob_create(
        (const char *)font->file_data, (unsigned int)font->file_size,
        SH_HB_MEMORY_READONLY, NULL, NULL);
    if (c->blob == NULL) {
        fdk_free(c);
        return NULL;
    }
    c->face = g_hb.hb_face_create(c->blob, 0);
    if (c->face == NULL) {
        g_hb.hb_blob_destroy(c->blob);
        fdk_free(c);
        return NULL;
    }
    c->font = g_hb.hb_font_create(c->face);
    if (c->font == NULL) {
        g_hb.hb_face_destroy(c->face);
        g_hb.hb_blob_destroy(c->blob);
        fdk_free(c);
        return NULL;
    }

    /* upem from the head table (bytes 18-19, big endian). */
    {
        const stbtt_fontinfo *info = &font->info;
        if (info->head <= 0 ||
            (size_t)info->head + 20 > font->file_size) {
            c->upem = 1000;
        } else {
            unsigned int upem =
                (unsigned int)font->file_data[info->head + 18] << 8 |
                (unsigned int)font->file_data[info->head + 19];
            c->upem = upem != 0 ? upem : 1000;
        }
    }

    /* GPOS values: font-units * (scale*64*upem) / upem = 26.6 px. */
    int hb_scale = sh_round_half(font->scale * 64.0f *
                                 (fdk_f32)c->upem);
    if (hb_scale < 1) {
        hb_scale = 1;
    }
    g_hb.hb_font_set_scale(c->font, hb_scale, hb_scale);

    if (g_hb_ffuncs == NULL) {
        g_hb_ffuncs = g_hb.hb_font_funcs_create();
        if (g_hb_ffuncs == NULL) {
            g_hb.hb_font_destroy(c->font);
            g_hb.hb_face_destroy(c->face);
            g_hb.hb_blob_destroy(c->blob);
            fdk_free(c);
            return NULL;
        }
        g_hb.hb_font_funcs_set_nominal_glyph_func(
            g_hb_ffuncs, sh_fn_nominal_glyph, NULL, NULL);
        g_hb.hb_font_funcs_set_glyph_h_advance_func(
            g_hb_ffuncs, sh_fn_h_advance, NULL, NULL);
        g_hb.hb_font_funcs_set_glyph_extents_func(
            g_hb_ffuncs, sh_fn_extents, NULL, NULL);
    }
    g_hb.hb_font_set_funcs(c->font, g_hb_ffuncs, font, NULL);

    font->shaper = c;
    return c->font;
}

/* ---- 6. Per-script fallback fonts ---------------------------------
 *
 * A segment whose glyphs the primary font lacks (FindGlyphIndex
 * answered .notdef) needs another font. fontconfig resolves the
 * script's language to a file (fontscan.c's resolver — the same
 * dlopen'd API, the same loadable-face gate); the loaded fdk_font
 * is cached ON the primary font (font->fallbacks) and dies with it.
 * Fallback fonts never get fallbacks of their own (is_fallback),
 * so the chain is exactly two levels deep and cannot cycle. */

bool fdk__font_covers(fdk_font *font, fdk_u32 cp) {
    if (font == NULL) {
        return false;
    }
    /* Control characters and whitespace: every font "covers" them
     * (their .notdef is legitimately empty ink) — do not let them
     * pull a fallback font in. */
    if (cp < 0x20u || (cp >= 0x80u && cp <= 0xA0u)) {
        return true;
    }
    return stbtt_FindGlyphIndex(&font->info, (int)cp) != 0;
}

bool fdk_font_has_glyph(const fdk_font *font, fdk_u32 codepoint) {
    if (font == NULL) {
        return false;
    }
    return stbtt_FindGlyphIndex(&font->info, (int)codepoint) != 0;
}

static fdk_font *sh_fallback_for(fdk_font *primary, int script,
                                fdk_u32 probe_cp) {
    if (primary == NULL || script < 0 || script >= SH_N_SCRIPTS ||
        primary->is_fallback ||
        primary->n_fallbacks >= FDK_TEXT_FALLBACK_MAX) {
        return NULL;
    }
    /* Already loaded? (One fallback per script: the first slot that
     * covers ANY of the script — fontconfig's answer for a language
     * covers the language.) */
    for (int i = 0; i < primary->n_fallbacks; i++) {
        if (primary->fallback_script[i] == script) {
            return primary->fallbacks[i];
        }
    }

    char *path = NULL;
    long face = 0;
    if (!fdk_text_resolve_fallback_font(
            sh_scripts[script].fc_lang, probe_cp, &path, &face)) {
        /* Remember the miss (negative cache) so a CJK-less machine
         * does not re-query fontconfig for every codepoint. */
        primary->fallback_script[primary->n_fallbacks] = script;
        primary->fallbacks[primary->n_fallbacks] = NULL;
        primary->n_fallbacks++;
        return NULL;
    }
    fdk_font *f = fdk_text_font_load_face(path, face,
                                          primary->pixel_size);
    if (f == NULL) {
        fdk_free(path);
        primary->fallback_script[primary->n_fallbacks] = script;
        primary->fallbacks[primary->n_fallbacks] = NULL;
        primary->n_fallbacks++;
        return NULL;
    }
    /* Coverage verification BEFORE the slot is taken: fontconfig's
     * sort ranks charset-covering fonts first, but it is a ranking,
     * not a guarantee — on the tofu machine (all CJK fonts CFF and
     * rejected by the old loader) the first LOADABLE candidate was a
     * Latin-only font, and caching it for the script would have
     * poisoned every later codepoint of that script. A loaded font
     * that does not map the probe is the same as no answer. */
    if (!fdk__font_covers(f, probe_cp)) {
        fdk_font_destroy(f);
        fdk_free(path);
        primary->fallback_script[primary->n_fallbacks] = script;
        primary->fallbacks[primary->n_fallbacks] = NULL;
        primary->n_fallbacks++;
        return NULL;
    }
    f->is_fallback = true;
    primary->fallback_script[primary->n_fallbacks] = script;
    primary->fallbacks[primary->n_fallbacks] = f;
    primary->n_fallbacks++;
    FDK_INFO("shaping: fallback font %s for %s", path,
             sh_scripts[script].name);
    fdk_free(path);
    return f;
}

/* The font for a codepoint: the primary when it covers it, else the
 * script's fallback, else the primary (a .notdef, honestly drawn). */
static fdk_font *sh_font_for_cp(fdk_font *primary, fdk_u32 cp,
                                int script) {
    if (fdk__font_covers(primary, cp)) {
        return primary;
    }
    if (script >= 0 && script < SH_N_SCRIPTS) {
        fdk_font *fb = sh_fallback_for(primary, script, cp);
        if (fb != NULL && fdk__font_covers(fb, cp)) {
            return fb;
        }
    }
    return primary;
}

/* ---- 7. The shaped pipeline ---------------------------------------- */

/* One shaping segment: a maximal logical run with one direction
 * (bidi level parity), one script, one style, one font. */
typedef struct {
    size_t cp_lo, cp_hi;   /* [lo, hi) into the decoded cps  */
    size_t byte_lo, byte_hi;
    int script;
    unsigned style;
    fdk_font *font;
    sh_fb_level level;
} sh_segment;

typedef struct {
    /* decoded logical text */
    fdk_u32 *cp;
    size_t *cp_byte; /* byte offset of each cp (absolute) */
    size_t n_cp;
    /* fribidi results */
    sh_fb_level *levels;
    sh_fb_index *v2l; /* visual -> logical index */
    size_t visual_len; /* surviving visual positions (embedding
                          controls are REMOVED by the resolution —
                          entries beyond this in v2l are garbage) */
    int rtl_present;
} sh_bidi;

/* Decodes [off, off+count) into cps + byte offsets. No fribidi —
 * the legacy path calls this alone. Returns false on allocation
 * failure. */
static bool sh_decode(sh_bidi *b, const char *utf8, size_t byte_len,
                      size_t off, size_t count) {
    memset(b, 0, sizeof *b);
    b->n_cp = 0;
    size_t cap = count > 0 ? count : 1; /* cps <= bytes */
    b->cp = fdk_alloc_array(cap, sizeof *b->cp);
    b->cp_byte = fdk_alloc_array(cap, sizeof *b->cp_byte);
    if (b->cp == NULL || b->cp_byte == NULL) {
        fdk_free(b->cp);
        fdk_free(b->cp_byte);
        memset(b, 0, sizeof *b);
        return false;
    }
    size_t i = off;
    const size_t end = off + count;
    while (i < end) {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(utf8, byte_len, i, &cp);
        if (i + (size_t)n > end) {
            break; /* a truncated codepoint at the range edge */
        }
        b->cp[b->n_cp] = cp;
        b->cp_byte[b->n_cp] = i;
        b->n_cp++;
        i += (size_t)n;
    }
    return true;
}

/* Full analysis: decode + FriBidi's paragraph direction, embedding
 * levels, and visual->logical map (one line — the range IS the
 * paragraph, the standard level-1 widget semantics). */
static bool sh_bidi_analyze(sh_bidi *b, const char *utf8,
                            size_t byte_len, size_t off,
                            size_t count) {
    if (!sh_decode(b, utf8, byte_len, off, count)) {
        return false;
    }
    if (b->n_cp == 0) {
        return true; /* empty: nothing to reorder */
    }

    b->levels = fdk_alloc_array(b->n_cp, sizeof *b->levels);
    b->v2l = fdk_alloc_array(b->n_cp, sizeof *b->v2l);
    sh_fb_index *l2v = fdk_alloc_array(b->n_cp, sizeof *l2v);
    if (b->levels == NULL || b->v2l == NULL || l2v == NULL) {
        fdk_free(l2v);
        fdk_free(b->levels);
        fdk_free(b->v2l);
        fdk_free(b->cp);
        fdk_free(b->cp_byte);
        memset(b, 0, sizeof *b);
        return false;
    }
    sh_fb_partype pbase = SH_FB_PAR_ON;
    if (g_fb.fribidi_log2vis(b->cp, (sh_fb_index)b->n_cp, &pbase,
                             NULL, l2v, b->v2l, b->levels) == 0) {
        fdk_free(l2v);
        fdk_free(b->levels);
        fdk_free(b->v2l);
        fdk_free(b->cp);
        fdk_free(b->cp_byte);
        memset(b, 0, sizeof *b);
        return false;
    }
    /* The surviving visual length: logical positions the resolution
     * did not remove (their L_to_V entries are valid). */
    b->visual_len = 0;
    for (size_t k = 0; k < b->n_cp; k++) {
        if (l2v[k] >= 0) {
            b->visual_len++;
        }
    }
    fdk_free(l2v);
    for (size_t k = 0; k < b->n_cp; k++) {
        if ((b->levels[k] & 1) != 0) {
            b->rtl_present = 1;
            break;
        }
    }
    return true;
}


/* Segments the logical cps by (level parity, effective script,
 * style, font). Common-script cps inherit the previous cp's script.
 * Segment byte ranges come from the cp boundaries + the range end
 * (segments are contiguous, so [byte_lo, byte_hi) covers exactly
 * [cp_lo, cp_hi)'s bytes). Returns the array + count, or false on
 * OOM. */
static bool sh_make_segments(sh_bidi *b, const fdk__text_run *runs,
                       size_t n_runs, fdk_font *primary,
                       size_t range_end, sh_segment **out,
                       size_t *out_n) {
    size_t cap = b->n_cp > 0 ? b->n_cp : 1;
    sh_segment *segs = fdk_alloc_array(cap, sizeof *segs);
    if (segs == NULL) {
        return false;
    }
    size_t n = 0;
    int prev_script = SH_SCRIPT_COMMON;
    for (size_t k = 0; k < b->n_cp; k++) {
        int script = sh_script_of(b->cp[k]);
        int eff = script;
        if (eff == SH_SCRIPT_COMMON) {
            eff = prev_script;
        }
        unsigned style =
            fdk__run_style_at(runs, n_runs, b->cp_byte[k], primary->style);
        fdk_font *font = sh_font_for_cp(primary, b->cp[k], eff);
        sh_fb_level level = b->levels[k];
        bool boundary = n == 0;
        if (!boundary) {
            const sh_segment *p = &segs[n - 1];
            boundary = (p->script != eff) || (p->style != style) ||
                       (p->font != font) ||
                       ((p->level & 1) != (level & 1));
        }
        if (boundary) {
            sh_segment s;
            s.cp_lo = k;
            s.cp_hi = k + 1;
            s.byte_lo = b->cp_byte[k];
            s.byte_hi = k + 1 < b->n_cp ? b->cp_byte[k + 1]
                                        : range_end;
            s.script = eff;
            s.style = style;
            s.font = font;
            s.level = level;
            segs[n++] = s;
        } else {
            segs[n - 1].cp_hi = k + 1;
            segs[n - 1].byte_hi =
                k + 1 < b->n_cp ? b->cp_byte[k + 1] : range_end;
        }
        prev_script =
            script == SH_SCRIPT_COMMON ? prev_script : script;
    }
    *out = segs;
    *out_n = n;
    return true;
}

/* Shapes one segment through HarfBuzz and appends its placements
 * (in the segment's VISUAL order — HarfBuzz reverses RTL output
 * itself) to the growing placement array at *io_n. seg_cps holds the
 * segment's codepoints ALREADY MIRRORED (the caller did it);
 * add_utf32 is fed those directly, so cluster values are cp indices
 * WITHIN the segment — mapped back to absolute bytes through
 * b->cp_byte. Also records the segment's per-cp attributed advances
 * into acc[] (logical order, absolute cp indices). Returns false on
 * any failure — the caller falls back to the legacy walk for the
 * whole range. */
static bool sh_shape_segment(sh_segment *seg, sh_bidi *b,
                             const fdk_u32 *seg_cps,
                             fdk__placement *places, size_t *io_n,
                             size_t max_places, fdk_f32 *pen,
                             fdk_f32 *acc) {
    hb_font_t *hbfont = sh_hb_font_for(seg->font);
    if (hbfont == NULL) {
        return false;
    }
    hb_buffer_t *buf = g_hb.hb_buffer_create();
    if (buf == NULL) {
        return false;
    }
    g_hb.hb_buffer_reset(buf);
    size_t n = seg->cp_hi - seg->cp_lo;
    if (!g_hb.hb_buffer_add_utf32(buf, seg_cps, (int)n, 0, (int)n)) {
        g_hb.hb_buffer_destroy(buf);
        return false;
    }
    g_hb.hb_buffer_set_cluster_level(buf,
                                     SH_HB_CLUSTER_MONOTONE_CHARACTERS);
    g_hb.hb_buffer_set_direction(
        buf, (seg->level & 1) != 0 ? SH_HB_DIRECTION_RTL
                                   : SH_HB_DIRECTION_LTR);
    if (seg->script >= 0 && seg->script < SH_N_SCRIPTS) {
        const char *tag = sh_scripts[seg->script].hb_tag;
        g_hb.hb_buffer_set_script(
            buf, g_hb.hb_script_from_string(tag, (int)strlen(tag)));
        const char *lang = sh_scripts[seg->script].fc_lang;
        g_hb.hb_buffer_set_language(
            buf, g_hb.hb_language_from_string(lang, (int)strlen(lang)));
    }
    g_hb.hb_shape(hbfont, buf, NULL, 0);

    unsigned int n_glyphs = g_hb.hb_buffer_get_length(buf);
    hb_glyph_info_t *infos =
        g_hb.hb_buffer_get_glyph_infos(buf, &n_glyphs);
    hb_glyph_position_t *poss = NULL;
    unsigned int n_pos = n_glyphs;
    if (infos != NULL) {
        poss = g_hb.hb_buffer_get_glyph_positions(buf, &n_pos);
    }
    if (infos == NULL || poss == NULL || n_pos < n_glyphs) {
        g_hb.hb_buffer_destroy(buf);
        return false;
    }
    /* Capacity: HarfBuzz can emit MORE glyphs than codepoints
     * (decomposed marks); the walk budget is generous and a
     * pathological font degrades to the legacy walk, never a
     * buffer overrun. */
    if (*io_n + n_glyphs > max_places) {
        g_hb.hb_buffer_destroy(buf);
        return false;
    }

    /* The synthetic-bold stem: glyph_slot bakes it into the inked
     * glyph's ADVANCE, so the shaped pen must add it too or bold
     * measure drifts from bold ink (and from the legacy walk). */
    int synth_stem = 0;
    if ((seg->style & FDK_FONT_STYLE_BOLD) != 0) {
        synth_stem = fdk__text_bold_stem(seg->font);
    }

    for (unsigned int i = 0; i < n_glyphs; i++) {
        fdk_f32 x_off = (fdk_f32)poss[i].x_offset / 64.0f;
        fdk_f32 y_off = (fdk_f32)poss[i].y_offset / 64.0f;
        fdk_f32 adv = (fdk_f32)poss[i].x_advance / 64.0f;

        fdk_f32 left = *pen + x_off;
        fdk_i32 gx = (fdk_i32)floorf(left);
        fdk_f32 frac = left - (fdk_f32)gx;
        int phase = (int)(frac * (fdk_f32)FDK_TEXT_SUBPIXEL_PHASES);
        if (phase >= FDK_TEXT_SUBPIXEL_PHASES) {
            phase = FDK_TEXT_SUBPIXEL_PHASES - 1;
        }
        int gid = (int)infos[i].codepoint;
        const fdk_glyph *raster =
            fdk_text_glyph_for_id_styled(seg->font, gid, seg->style, phase);
        if (raster == NULL) {
            g_hb.hb_buffer_destroy(buf);
            return false;
        }

        if (synth_stem != 0 && raster->bits != NULL &&
            raster->w > 0 && raster->h > 0) {
            adv += (fdk_f32)synth_stem; /* inked only — glyph_slot's rule */
        }
        fdk__placement *p = &places[(*io_n)++];
        p->raster = raster;
        p->font = seg->font;
        p->x = gx;
        p->y_shift = (fdk_i32)sh_round_half(y_off);
        *pen += adv;
        p->pen_after = *pen;
        p->cluster =
            b->cp_byte[seg->cp_lo + (size_t)infos[i].cluster];
    }

    /* Per-cp attribution (logical): a glyph's advance lands on the
     * LAST cp of its cluster. Clusters are segment-local cp indices
     * here, so the arithmetic is index math, not byte hunting. */
    {
        size_t *clusters = fdk_alloc_array(n_glyphs, sizeof *clusters);
        if (clusters == NULL) {
            g_hb.hb_buffer_destroy(buf);
            return false;
        }
        size_t n_cl = 0;
        for (unsigned int i = 0; i < n_glyphs; i++) {
            size_t c = (size_t)infos[i].cluster;
            size_t j = n_cl;
            while (j > 0 && clusters[j - 1] > c) {
                clusters[j] = clusters[j - 1];
                j--;
            }
            if (j == 0 || clusters[j - 1] != c) {
                clusters[j] = c;
                n_cl++;
            }
        }
        for (unsigned int i = 0; i < n_glyphs; i++) {
            size_t c = (size_t)infos[i].cluster;
            size_t slot = 0;
            while (slot < n_cl && clusters[slot] != c) {
                slot++;
            }
            size_t next_c = slot + 1 < n_cl ? clusters[slot + 1] : n;
            if (next_c == 0) {
                next_c = 1; /* defensive: never underflow */
            }
            size_t last_cp = seg->cp_lo + next_c - 1;
            if (last_cp < b->n_cp) {
                fdk_f32 adv = (fdk_f32)poss[i].x_advance / 64.0f;
                if (synth_stem != 0) {
                    const fdk_glyph *rg = NULL;
                    /* the placement this glyph produced (same index) */
                    size_t pidx = *io_n - n_glyphs + i;
                    if (pidx < *io_n) {
                        rg = places[pidx].raster;
                    }
                    if (rg != NULL && rg->bits != NULL && rg->w > 0 &&
                        rg->h > 0) {
                        adv += (fdk_f32)synth_stem;
                    }
                }
                acc[last_cp] += adv;
            }
        }
        fdk_free(clusters);
    }

    g_hb.hb_buffer_destroy(buf);
    return true;
}

/* ---- 8. The shaped range (the public seam) ------------------------- */

static bool sh_legacy_build(fdk__shaped *s, fdk_font *font,
                            const char *utf8, size_t byte_len,
                            size_t off, size_t count,
                            const fdk__text_run *runs, size_t n_runs);

/* Builds the shaped result for [off, off+count). ONE rounding rule
 * survives: pen floors per glyph (the placement x), rounds once at
 * the consumer (the total). */
static fdk__shaped *sh_shape_range_impl(fdk_font *font,
                                        const char *utf8,
                                        size_t byte_len, size_t off,
                                        size_t count,
                                        const fdk__text_run *runs,
                                        size_t n_runs) {
    fdk__shaped *s = fdk_alloc(sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof *s);
    s->range_end = off + count;

    sh_bidi b;
    if (!sh_bidi_analyze(&b, utf8, byte_len, off, count)) {
        fdk_free(s);
        return NULL;
    }
    s->n_cp = b.n_cp;
    s->cp = b.cp;
    s->cp_byte = b.cp_byte;
    s->rtl_visual = b.rtl_present;
    /* b.levels / b.v2l ownership moves into the local scope; freed
     * at the end of this function. */

    const size_t max_places = b.n_cp > 0 ? b.n_cp * 4 + 32 : 32;
    s->places = fdk_alloc_array(max_places, sizeof *s->places);
    fdk_f32 *acc = fdk_alloc_array(b.n_cp + 1, sizeof *acc);
    s->cp_pen = fdk_alloc_array(b.n_cp + 1, sizeof *s->cp_pen);
    if (s->places == NULL || acc == NULL || s->cp_pen == NULL) {
        fdk_free(acc);
        fdk_free(b.levels);
        fdk_free(b.v2l);
        fdk__shaped_free(s);
        return NULL;
    }
    memset(acc, 0, (b.n_cp + 1) * sizeof *acc);

    if (b.n_cp == 0) {
        fdk_free(acc);
        fdk_free(b.levels);
        fdk_free(b.v2l);
        s->cp_pen[0] = 0.0f;
        return s; /* empty range: zero placements, zero total */
    }

    /* Segmentation + per-segment shaping in visual order. */
    sh_segment *segs = NULL;
    size_t n_segs = 0;
    if (!sh_make_segments(&b, runs, n_runs, font, off + count, &segs,
                    &n_segs)) {
        fdk_free(acc);
        fdk_free(b.levels);
        fdk_free(b.v2l);
        fdk__shaped_free(s);
        return NULL;
    }
    size_t *seg_of_cp = fdk_alloc_array(b.n_cp, sizeof *seg_of_cp);
    size_t *visual_order = fdk_alloc_array(n_segs, sizeof *visual_order);
    fdk_u32 *seg_cps = fdk_alloc_array(b.n_cp, sizeof *seg_cps);
    if (seg_of_cp == NULL || visual_order == NULL || seg_cps == NULL) {
        fdk_free(segs);
        fdk_free(seg_of_cp);
        fdk_free(visual_order);
        fdk_free(seg_cps);
        fdk_free(acc);
        fdk_free(b.levels);
        fdk_free(b.v2l);
        fdk__shaped_free(s);
        return NULL;
    }

    /* segment index per cp */
    for (size_t i = 0; i < n_segs; i++) {
        for (size_t k = segs[i].cp_lo; k < segs[i].cp_hi; k++) {
            seg_of_cp[k] = i;
        }
    }
    /* visual segment order: walk the visual positions, dedupe
     * (a segment's visual positions are contiguous — UAX #9 level
     * runs reorder as blocks — so each segment appears exactly once).
     * FriBidi REMOVES explicit embedding controls (LRE/RLE/PDF/LRO/
     * RLO and friends) from the visual stream; their map entries are
     * sentinel/garbage — skipped here, and a segment made only of
     * them never appears (its attribution stays zero: invisible, the
     * documented level-1 treatment of directional formatting). */
    size_t n_visual = 0;
    for (size_t v = 0; v < b.visual_len && v < b.n_cp; v++) {
        sh_fb_index logical = b.v2l[v];
        if (logical < 0 || (size_t)logical >= b.n_cp) {
            continue; /* defensive: garbage map entry */
        }
        size_t seg = seg_of_cp[(size_t)logical];
        if (n_visual < n_segs &&
            (n_visual == 0 || visual_order[n_visual - 1] != seg)) {
            visual_order[n_visual++] = seg;
        }
    }

    fdk_f32 pen = 0.0f;
    bool ok = true;
    for (size_t vi = 0; vi < n_visual && ok; vi++) {
        sh_segment *seg = &segs[visual_order[vi]];
        /* the segment's cps, mirrored when the run is RTL */
        size_t n = seg->cp_hi - seg->cp_lo;
        for (size_t k = 0; k < n; k++) {
            fdk_u32 cp = b.cp[seg->cp_lo + k];
            if ((seg->level & 1) != 0) {
                cp = sh_mirror_cp(cp);
            }
            seg_cps[k] = cp;
        }
        ok = sh_shape_segment(seg, &b, seg_cps, s->places,
                              &s->n_places, max_places, &pen, acc);
    }

    fdk_free(segs);
    fdk_free(seg_of_cp);
    fdk_free(visual_order);
    fdk_free(seg_cps);
    fdk_free(b.levels);
    fdk_free(b.v2l);

    if (!ok) {
        /* HarfBuzz failed somewhere: the honest level-1 fallback is
         * the WHOLE range through the legacy walk (one arithmetic,
         * never a half-shaped line). */
        fdk_free(acc);
        fdk_free(s->places);
        s->places = NULL;
        s->n_places = 0;
        fdk_free(s->cp_pen);
        s->cp_pen = NULL;
        if (!sh_legacy_build(s, font, utf8, byte_len, off, count, runs,
                             n_runs)) {
            fdk__shaped_free(s);
            return NULL;
        }
        return s;
    }

    /* ink bounds from the placements */
    for (size_t i = 0; i < s->n_places; i++) {
        const fdk_glyph *g = s->places[i].raster;
        if (g != NULL && g->w > 0 && g->h > 0) {
            fdk_f32 top = (fdk_f32)g->yoff + (fdk_f32)s->places[i].y_shift;
            fdk_f32 bottom = (fdk_f32)(g->yoff + g->h) +
                             (fdk_f32)s->places[i].y_shift;
            if (!s->has_ink) {
                s->ink_top = top;
                s->ink_bottom = bottom;
                s->has_ink = 1;
            } else {
                if (top < s->ink_top) {
                    s->ink_top = top;
                }
                if (bottom > s->ink_bottom) {
                    s->ink_bottom = bottom;
                }
            }
        }
    }

    /* logical per-cp pen: prefix sums of the attribution */
    s->cp_pen[0] = 0.0f;
    for (size_t k = 0; k < b.n_cp; k++) {
        s->cp_pen[k + 1] = s->cp_pen[k] + acc[k];
    }
    s->total = pen;
    fdk_free(acc);
    return s;
}

/* The legacy placement builder: the exact walk the four text passes
 * have always used (per style-run, kerning resets at boundaries,
 * subpixel phases baked per glyph). Produces one placement per
 * codepoint, visual == logical order — the no-libs path and the
 * HarfBuzz-failure fallback, bit-compatible with 1.4.x metrics. */
static bool sh_legacy_build(fdk__shaped *s, fdk_font *font,
                            const char *utf8, size_t byte_len,
                            size_t off, size_t count,
                            const fdk__text_run *runs,
                            size_t n_runs) {
    const size_t max_places = s->n_cp > 0 ? s->n_cp + 1 : 1;
    s->places = fdk_alloc_array(max_places, sizeof *s->places);
    s->cp_pen = fdk_alloc_array(s->n_cp + 1, sizeof *s->cp_pen);
    if (s->places == NULL || s->cp_pen == NULL) {
        return false;
    }
    s->n_places = 0;
    s->total = 0.0f;
    s->has_ink = 0;
    s->ink_top = 0.0f;
    s->ink_bottom = 0.0f;

    const size_t end = off + count;
    size_t i = off;
    fdk_f32 pen = 0.0f;
    /* the cp decode state for the attribution arrays */
    size_t n_cp = 0;
    while (i < end) {
        int prev_g = -1;
        /* the style of the byte being consumed */
        unsigned style = fdk__run_style_at(runs, n_runs, i, font->style);
        size_t r_end = end;
        /* clip to the style run's end so kerning resets there */
        if (runs != NULL && n_runs > 0) {
            for (size_t k = 0; k < n_runs; k++) {
                if (i >= runs[k].start && i < runs[k].end) {
                    r_end = runs[k].end < end ? runs[k].end : end;
                    break;
                }
            }
        }
        while (i < r_end) {
            size_t cp_byte = i;
            const fdk_glyph *glyph = NULL;
            fdk_i32 gx = 0;
            if (!fdk_text_shape_step(font, utf8, byte_len, &i, &prev_g,
                                     &pen, style, &glyph, &gx)) {
                break;
            }
            fdk__placement *p = &s->places[s->n_places++];
            p->raster = glyph;
            p->font = font;
            p->x = gx;
            p->y_shift = 0;
            p->pen_after = pen;
            p->cluster = cp_byte;
            if (glyph != NULL && glyph->w > 0 && glyph->h > 0) {
                fdk_f32 top = (fdk_f32)glyph->yoff;
                fdk_f32 bottom = (fdk_f32)(glyph->yoff + glyph->h);
                if (!s->has_ink) {
                    s->ink_top = top;
                    s->ink_bottom = bottom;
                    s->has_ink = 1;
                } else {
                    if (top < s->ink_top) {
                        s->ink_top = top;
                    }
                    if (bottom > s->ink_bottom) {
                        s->ink_bottom = bottom;
                    }
                }
            }
            if (n_cp < s->n_cp) {
                s->cp_pen[n_cp + 1] = pen;
            }
            n_cp++;
        }
    }
    s->total = pen;
    /* cp_pen[0] = 0 by allocation zeroing? No — set explicitly. */
    if (s->n_cp > 0) {
        s->cp_pen[0] = 0.0f;
    }
    return true;
}

/* The one entry point the consumers call. Shapes [off, off+count)
 * of utf8 under the (optional) flattened style runs. NEVER returns
 * NULL for a non-NULL font except on allocation failure — the
 * legacy path has no failure modes of its own. */
fdk__shaped *fdk__shape_range(fdk_font *font, const char *utf8,
                              size_t byte_len, size_t off,
                              size_t count,
                              const fdk__text_run *runs,
                              size_t n_runs) {
    if (font == NULL || utf8 == NULL || off > byte_len ||
        count > byte_len - off) {
        return NULL;
    }
    sh_probe();
    if (g_shaping_state != 1) {
        fdk__shaped *s = fdk_alloc(sizeof *s);
        if (s == NULL) {
            return NULL;
        }
        memset(s, 0, sizeof *s);
        s->range_end = off + count;
        /* decode the cps for the attribution arrays (no fribidi
         * needed — the legacy walk is logical order only) */
        sh_bidi b;
        if (!sh_decode(&b, utf8, byte_len, off, count)) {
            fdk_free(s);
            return NULL;
        }
        s->cp = b.cp;
        s->cp_byte = b.cp_byte;
        s->n_cp = b.n_cp;
        s->rtl_visual = 0;
        if (!sh_legacy_build(s, font, utf8, byte_len, off, count, runs,
                             n_runs)) {
            fdk__shaped_free(s);
            return NULL;
        }
        return s;
    }
    return sh_shape_range_impl(font, utf8, byte_len, off, count, runs,
                               n_runs);
}

void fdk__shaped_free(fdk__shaped *s) {
    if (s == NULL) {
        return;
    }
    fdk_free(s->places);
    fdk_free(s->cp);
    fdk_free(s->cp_byte);
    fdk_free(s->cp_pen);
    fdk_free(s);
}

/* The visual boundary x for a LOGICAL byte boundary (the Entry
 * caret). Pure-LTR text (including CJK — Han is LTR) is exact: the
 * boundary x is the pen where the next glyph's cluster starts.
 * Mixed-direction text lands at the glyph cluster that contains the
 * boundary — segment granularity, honestly documented. */
fdk_i32 fdk__shaped_caret_x(const fdk__shaped *s, size_t byte) {
    if (s == NULL || s->n_places == 0) {
        return 0;
    }
    if (byte >= s->range_end) {
        return (fdk_i32)(s->total + 0.5f);
    }
    /* The visual pen BEFORE the first placement whose cluster is
     * at/after the boundary; if none, the pen after the last. */
    fdk_f32 pen = 0.0f;
    for (size_t i = 0; i < s->n_places; i++) {
        if (s->places[i].cluster >= byte) {
            break;
        }
        pen = s->places[i].pen_after;
    }
    return (fdk_i32)(pen + 0.5f);
}

/* ---- 9. Lifecycle --------------------------------------------------- */

/* Called from fdk_font_destroy BEFORE the file data is freed: the
 * HarfBuzz blob references font->file_data READ-ONLY, and the
 * fallback fonts are owned by this font. */
void fdk__shaping_font_destroy(fdk_font *font) {
    if (font == NULL) {
        return;
    }
    sh_hb_font_cache *c = font->shaper;
    if (c != NULL) {
        if (g_hb.hb_font_destroy != NULL && c->font != NULL) {
            g_hb.hb_font_destroy(c->font);
        }
        if (g_hb.hb_face_destroy != NULL && c->face != NULL) {
            g_hb.hb_face_destroy(c->face);
        }
        if (g_hb.hb_blob_destroy != NULL && c->blob != NULL) {
            g_hb.hb_blob_destroy(c->blob);
        }
        fdk_free(c);
        font->shaper = NULL;
    }
    for (int i = 0; i < font->n_fallbacks; i++) {
        if (font->fallbacks[i] != NULL) {
            fdk_font_destroy(font->fallbacks[i]);
        }
        font->fallbacks[i] = NULL;
    }
    font->n_fallbacks = 0;
}



