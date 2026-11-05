/* colors.c — see colors.h. The 16-slot bases per scheme; the cube
 * and grayscale ramp are computed (xterm's exact formulas). */
#include "colors.h"

#include "fdk/fdk_theme.h"

#include <string.h>

static tscheme g_scheme = TSCHEME_FOLLOW;

/* 16 base colors per scheme: [0..7] normal, [8..15] bright. */
static const uint8_t base_follow_dark[16][3] = {
    {0x17, 0x1a, 0x21}, {0xa3, 0x51, 0x51}, {0x5d, 0x8f, 0x4b},
    {0xa8, 0x84, 0x39}, {0x41, 0x71, 0x99}, {0x86, 0x5a, 0x9c},
    {0x4a, 0x8b, 0x8b}, {0xc5, 0xc5, 0xc5},
    {0x5c, 0x63, 0x6f}, {0xbf, 0x6d, 0x6d}, {0x83, 0xb1, 0x6a},
    {0xc9, 0xa0, 0x4b}, {0x5d, 0x92, 0xc4}, {0xa4, 0x79, 0xbc},
    {0x68, 0xa5, 0xa5}, {0xe6, 0xe6, 0xe6},
};
static const uint8_t base_follow_light[16][3] = {
    {0x2e, 0x34, 0x40}, {0xa0, 0x34, 0x34}, {0x35, 0x6a, 0x2a},
    {0x8f, 0x6b, 0x25}, {0x1c, 0x4d, 0x82}, {0x5c, 0x35, 0x73},
    {0x1f, 0x63, 0x63}, {0xd3, 0xd7, 0xcf},
    {0x55, 0x57, 0x53}, {0xcc, 0x39, 0x39}, {0x4e, 0x9a, 0x3e},
    {0xb4, 0x8a, 0x2b}, {0x34, 0x65, 0xa4}, {0x75, 0x50, 0x7b},
    {0x2e, 0x8b, 0x8b}, {0xee, 0xee, 0xec},
};
static const uint8_t base_solarized_dark[16][3] = {
    {0x07, 0x36, 0x42}, {0xdc, 0x32, 0x2f}, {0x85, 0x99, 0x00},
    {0xb5, 0x89, 0x00}, {0x26, 0x8b, 0xd2}, {0xd3, 0x36, 0x82},
    {0x2a, 0xa1, 0x98}, {0xee, 0xe8, 0xd5},
    {0x00, 0x2b, 0x36}, {0xcb, 0x4b, 0x16}, {0x58, 0x6e, 0x75},
    {0x65, 0x7b, 0x83}, {0x83, 0x94, 0x96}, {0x6c, 0x71, 0xc4},
    {0x93, 0xa1, 0xa1}, {0xfd, 0xf6, 0xe3},
};
static const uint8_t base_solarized_light[16][3] = {
    {0xee, 0xe8, 0xd5}, {0xdc, 0x32, 0x2f}, {0x85, 0x99, 0x00},
    {0xb5, 0x89, 0x00}, {0x26, 0x8b, 0xd2}, {0xd3, 0x36, 0x82},
    {0x2a, 0xa1, 0x98}, {0x07, 0x36, 0x42},
    {0xfd, 0xf6, 0xe3}, {0xcb, 0x4b, 0x16}, {0x93, 0xa1, 0xa1},
    {0x83, 0x94, 0x96}, {0x65, 0x7b, 0x83}, {0x6c, 0x71, 0xc4},
    {0x58, 0x6e, 0x75}, {0x00, 0x2b, 0x36},
};
static const uint8_t base_gruvbox_dark[16][3] = {
    {0x28, 0x28, 0x28}, {0xcc, 0x24, 0x1d}, {0x98, 0x97, 0x1a},
    {0xd7, 0x99, 0x21}, {0x45, 0x85, 0x88}, {0xb1, 0x62, 0x86},
    {0x68, 0x9d, 0x6a}, {0xa8, 0x99, 0x84},
    {0x92, 0x83, 0x74}, {0xfb, 0x49, 0x34}, {0xb8, 0xbb, 0x26},
    {0xfa, 0xbd, 0x2f}, {0x83, 0xa5, 0x98}, {0xd3, 0x86, 0x9b},
    {0x8e, 0xc0, 0x7c}, {0xeb, 0xdb, 0xb2},
};
static const uint8_t base_contrast[16][3] = {
    {0x00, 0x00, 0x00}, {0xd0, 0x00, 0x00}, {0x00, 0xd0, 0x00},
    {0xd0, 0xd0, 0x00}, {0x00, 0x00, 0xd0}, {0xd0, 0x00, 0xd0},
    {0x00, 0xd0, 0xd0}, {0xe8, 0xe8, 0xe8},
    {0x70, 0x70, 0x70}, {0xff, 0x40, 0x40}, {0x40, 0xff, 0x40},
    {0xff, 0xff, 0x40}, {0x40, 0x40, 0xff}, {0xff, 0x40, 0xff},
    {0x40, 0xff, 0xff}, {0xff, 0xff, 0xff},
};

const char *tscheme_name(tscheme s) {
    switch (s) {
    case TSCHEME_SOLARIZED_DARK: return "solarized-dark";
    case TSCHEME_SOLARIZED_LIGHT: return "solarized-light";
    case TSCHEME_GRUVBOX_DARK: return "gruvbox-dark";
    case TSCHEME_CONTRAST: return "contrast";
    case TSCHEME_FOLLOW:
    default: return "follow-theme";
    }
}

bool tscheme_parse(const char *name, tscheme *out) {
    if (name == NULL) {
        return false;
    }
    for (int i = 0; i < TSCHEME_COUNT; i++) {
        if (strcmp(name, tscheme_name((tscheme)i)) == 0) {
            *out = (tscheme)i;
            return true;
        }
    }
    return false;
}

void tcolor_theme_changed(void) {
    /* nothing cached beyond the scheme itself; the FOLLOW branch
     * re-reads the theme on every resolve (cheap: token lookup). */
}

static bool scheme_is_dark(void) {
    fdk_color bg = fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    /* ITU-R BT.601 luma of the theme background */
    double luma = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b;
    return luma < 0.5;
}

static const uint8_t (*base_table(void))[3] {
    switch (g_scheme) {
    case TSCHEME_SOLARIZED_DARK: return base_solarized_dark;
    case TSCHEME_SOLARIZED_LIGHT: return base_solarized_light;
    case TSCHEME_GRUVBOX_DARK: return base_gruvbox_dark;
    case TSCHEME_CONTRAST: return base_contrast;
    case TSCHEME_FOLLOW:
    default:
        return scheme_is_dark() ? base_follow_dark : base_follow_light;
    }
}

void tcolor_set_scheme(tscheme s) {
    g_scheme = s;
}

fdk_color tcolor_index(int index, bool is_bg) {
    fdk_color c = {0, 0, 0, 0};
    if (index < 0 || index > 255) {
        return tcolor_default_bg();
    }
    if (index < 16) {
        const uint8_t (*b)[3] = base_table();
        c.r = (float)b[index][0] / 255.0f;
        c.g = (float)b[index][1] / 255.0f;
        c.b = (float)b[index][2] / 255.0f;
        return c;
    }
    if (index < 232) {
        int n = index - 16;
        int steps[6] = {0, 95, 135, 175, 215, 255};
        c.r = (float)steps[(n / 36) % 6] / 255.0f;
        c.g = (float)steps[(n / 6) % 6] / 255.0f;
        c.b = (float)steps[n % 6] / 255.0f;
        return c;
    }
    int v = 8 + (index - 232) * 10;
    c.r = c.g = c.b = (float)v / 255.0f;
    (void)is_bg;
    return c;
}

fdk_color tcolor_default_fg(void) {
    if (g_scheme == TSCHEME_FOLLOW) {
        return fdk_theme_get_color(NULL, FDK_TK_TEXT);
    }
    return tcolor_index(g_scheme == TSCHEME_SOLARIZED_LIGHT ? 14 : 15,
                        false);
}

fdk_color tcolor_default_bg(void) {
    if (g_scheme == TSCHEME_FOLLOW) {
        return fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    }
    return tcolor_index(0, true);
}

fdk_color tcolor_resolve(tcolor c, bool is_bg) {
    if (c.index >= 0) {
        return tcolor_index(c.index, is_bg);
    }
    if (c.r == 0 && c.g == 0 && c.b == 0) {
        /* unset: the defaults. Foreground unset on default bg is
         * invisible; xterm's convention is fg-default (theme text). */
        return is_bg ? tcolor_default_bg() : tcolor_default_fg();
    }
    fdk_color out = {(float)c.r / 255.0f, (float)c.g / 255.0f, (float)c.b / 255.0f, 1.0f};
    return out;
}
