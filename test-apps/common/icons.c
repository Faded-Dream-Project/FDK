/* icons.c — see icons.h. */
#include "icons.h"

#include "fdk/fdk_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the bundled set, embedded at build time ---------------------- */
#include "icons_data.h"

typedef struct cached_icon {
    char *key; /* "name|size|ink" */
    fdk_surface *surface;
    struct cached_icon *next;
} cached_icon;

static cached_icon *g_cache;
static char *g_available;

/* Rewrites `currentColor` to a concrete #rrggbb ink, in place (the
 * buffer is ours). */
static void tint_current_color(char *svg, size_t len, fdk_color ink) {
    char hex[8];
    snprintf(hex, sizeof hex, "#%02x%02x%02x",
             (unsigned)(ink.r * 255.0f), (unsigned)(ink.g * 255.0f),
             (unsigned)(ink.b * 255.0f));
    char *at = svg;
    while ((at = strstr(at, "currentColor")) != NULL) {
        size_t remain = len - (size_t)(at - svg) - 12;
        memmove(at + 7, at + 12, remain + 1); /* include NUL */
        memcpy(at, hex, 7);
        at += 7;
        len -= 5; /* the string shrank by 5 */
    }
}

const char *icons_available(void) {
    if (g_available != NULL) {
        return g_available;
    }
    size_t cap = 1024, len = 0;
    g_available = malloc(cap);
    if (g_available == NULL) {
        return "";
    }
    for (size_t i = 0; i < ICONS_DATA_COUNT; i++) {
        size_t need = strlen(ICONS_DATA[i][0]) + 1;
        if (len + need + 1 > cap) {
            cap = (len + need + 1) * 2;
            char *grown = realloc(g_available, cap);
            if (grown == NULL) {
                break;
            }
            g_available = grown;
        }
        memcpy(g_available + len, ICONS_DATA[i][0], need);
        len += need;
    }
    g_available[len] = '\0';
    return g_available;
}

fdk_surface *icons_get(const char *name, fdk_i32 size, fdk_f32 scale) {
    if (name == NULL || name[0] == '\0' || size <= 0) {
        return NULL;
    }
    fdk_i32 px = size;
    if (scale > 0.25f && scale < 4.0f) {
        px = (fdk_i32)((float)size * scale + 0.5f);
    }
    fdk_color ink = fdk_theme_get_color(NULL, FDK_TK_TEXT);

    char key[128];
    snprintf(key, sizeof key, "%s|%d|%02x%02x%02x", name, px,
             (unsigned)(ink.r * 255.0f), (unsigned)(ink.g * 255.0f),
             (unsigned)(ink.b * 255.0f));
    for (cached_icon *c = g_cache; c != NULL; c = c->next) {
        if (strcmp(c->key, key) == 0) {
            return c->surface;
        }
    }

    /* 1. the bundled SVG, tinted */
    const char *svg = NULL;
    for (size_t i = 0; i < ICONS_DATA_COUNT; i++) {
        if (strcmp(ICONS_DATA[i][0], name) == 0) {
            svg = ICONS_DATA[i][1];
            break;
        }
    }
    if (svg != NULL) {
        size_t len = strlen(svg);
        char *buf = malloc(len + 1);
        if (buf != NULL) {
            memcpy(buf, svg, len + 1);
            tint_current_color(buf, len, ink);
            fdk_surface *s = NULL;
            if (fdk_ok(fdk_surface_create_from_image_bytes(
                    buf, len + 1, &s)) &&
                s != NULL) {
                free(buf);
                cached_icon *c = malloc(sizeof *c);
                if (c != NULL) {
                    c->key = strdup(key);
                    c->surface = s;
                    c->next = g_cache;
                    if (c->key != NULL) {
                        g_cache = c;
                        return s;
                    }
                    free(c);
                }
                fdk_surface_destroy(s);
                return NULL;
            }
            free(buf);
        }
    }

    /* 2. the desktop's icon theme (real Adwaita/breeze beats nothing) */
    fdk_surface *themed = fdk_icon_theme_surface(name, px);
    if (themed != NULL) {
        return themed; /* not cached: icon-theme has its own cache */
    }

    /* 3. NULL — the caller falls back to fdk_row_icon glyphs */
    return NULL;
}

void icons_theme_changed(void) {
    cached_icon *c = g_cache;
    while (c != NULL) {
        cached_icon *next = c->next;
        free(c->key);
        fdk_surface_destroy(c->surface);
        free(c);
        c = next;
    }
    g_cache = NULL;
}

void icons_shutdown(void) {
    icons_theme_changed();
    free(g_available);
    g_available = NULL;
}
