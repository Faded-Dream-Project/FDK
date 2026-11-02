/* icons.c — see icons.h. */
#include "icons.h"

#include "fdk/fdk_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the bundled set, embedded at build time ---------------------- */
#include "icons_data.h"

typedef struct cached_icon {
    char *path; /* system tier: the name that answered */
    char *key; /* "name|size|ink" */
    fdk_surface *surface;
    struct cached_icon *next;
} cached_icon;

/* §31/PART-2: the tier that answered the last lookup (the --diag
 * contract) and the system-theme path when that tier won. */
static icons_tier g_last_tier = ICONS_TIER_NONE;
static char g_last_path[512];

/* The bundled set's legacy short names -> the freedesktop standard
 * names (the Icon Naming Specification's exact spellings; the ones
 * the spec does not define — view-list/view-grid/view-hidden — are
 * the names GNOME's own file manager uses and every major theme
 * ships). A "-symbolic" probe is tried FIRST for toolbar/sidebars. */
static const struct {
    const char *legacy;    /* test-apps/icons/ stem            */
    const char *standard;  /* the freedesktop name             */
} g_name_map[] = {
    {"back", "go-previous"},       {"forward", "go-next"},
    {"up", "go-up"},               {"home", "go-home"},
    {"refresh", "view-refresh"},   {"folder-new", "folder-new"},
    {"search", "system-search"},   {"clear", "edit-clear"},
    {"eye", "view-hidden"},        {"open", "document-open"},
    {"save", "document-save"},     {"undo", "edit-undo"},
    {"redo", "edit-redo"},         {"cut", "edit-cut"},
    {"copy", "edit-copy"},         {"paste", "edit-paste"},
    {"delete", "edit-delete"},     {"trash", "user-trash"},
    {"folder", "folder"},          {"doc", "text-x-generic"},
    {"image", "image-x-generic"},  {"audio", "audio-x-generic"},
    {"video", "video-x-generic"},  {"archive", "package-x-generic"},
    {"code", "text-x-script"},     {"disk", "drive-harddisk"},
    {"usb", "drive-removable-media"},
    {"network", "network-server"}, {"star", "emblem-favorite"},
    {"close", "window-close"},     {"add", "list-add"},
    {"remove", "list-remove"},     {"prefs", "preferences-system"},
    {"about", "help-about"},       {"book", "user-bookmarks"},
    {"clock", "document-open-recent"},
    {"battery", "battery"},        {"cpu", "applications-engineering"},
    {"chart", "x-office-spreadsheet"},
    {"app", "system-file-manager"},
    {"check", "object-select"},    {"bold", "format-text-bold"},
    {"italic", "format-text-italic"},
    {"underline", "format-text-underline"},
    {"strike", "format-text-strikethrough"},
    {"bullets", "format-list-unordered"},
    {"numbers", "format-list-ordered"},
    {"link", "insert-link"},       {"left", "go-previous"},
    {"right", "go-next"},          {"down", "go-down"},
    {"trash-full", "user-trash-full"},
    {"desktop", "user-desktop"},
    {"documents", "folder-documents"},
    {"downloads", "folder-download"},
    {"music", "folder-music"},     {"pictures", "folder-pictures"},
    {"videos", "folder-videos"},
};

static const char *standard_name(const char *legacy) {
    if (legacy == NULL) {
        return NULL;
    }
    /* already-standard names pass through untouched */
    for (size_t i = 0;
         i < sizeof(g_name_map) / sizeof(g_name_map[0]); i++) {
        if (strcmp(g_name_map[i].legacy, legacy) == 0) {
            return g_name_map[i].standard;
        }
    }
    return legacy;
}

static cached_icon *g_cache;
static long g_stat_system_hits, g_stat_lookups;

long icons_system_hits(void) { return g_stat_system_hits; }
long icons_lookups(void) { return g_stat_lookups; }
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
        g_last_tier = ICONS_TIER_FALLBACK;
        g_last_path[0] = '\0';
        return NULL;
    }
    fdk_i32 px = size;
    if (scale > 0.25f && scale < 4.0f) {
        px = (fdk_i32)((float)size * scale + 0.5f);
    }
    fdk_color ink = fdk_theme_get_color(NULL, FDK_TK_TEXT);

    /* §31/PART-2: THE SYSTEM THEME FIRST (S3). The freedesktop name
     * (mapped from any legacy bundled stem), the -symbolic variant
     * tried before the full-color one — symbolic comes back already
     * tinted with the theme ink, which is exactly what a toolbar
     * wants. THIS LOOKUP IS NOT CACHED HERE: FDK's icon-theme engine
     * owns its own three-layer cache, and a theme switch resets it.
     *
     * The FALLBACK-NAME chain (the Icon Naming Specification's own
     * recommendation for names a theme may lack): view-hidden's
     * alternates, view-compact's, each tried symbolic-then-plain
     * through the system tier BEFORE the bundled set answers. A
     * theme that has NONE of them falls to the embedded SVG — never
     * a blank button. */
    static const struct {
        const char *name;
        const char *const alts[3]; /* NULL-terminated */
    } alt_names[] = {
        {"view-hidden", {"visibility", NULL, NULL}},
        {"view-compact", {"view-list-compact", "view-list-details",
                          NULL}},
        {"view-grid", {"view-list-icons", NULL, NULL}},
        {"system-search", {"edit-find", NULL, NULL}},
        {"user-trash-full", {"user-trash", NULL, NULL}},
        {"folder-documents", {"folder", NULL, NULL}},
        {"folder-download", {"folder", NULL, NULL}},
        {"folder-music", {"folder", NULL, NULL}},
        {"folder-pictures", {"folder", NULL, NULL}},
        {"folder-videos", {"folder", NULL, NULL}},
    };
    /* §31/owner-round-2: the SYSTEM tier is cached HERE as well.
     * fdk_icon_theme_surface() hands out a fresh OWNED clone per
     * call (its documented contract) — the old code returned it
     * borrowed-style, never cached it, never freed it, and every
     * repaint allocated a new surface: 925 leaked clones (2.4 MB)
     * on the owner's machine. Cached under "S|name|px" (the engine
     * bakes the symbolic tint at decode; the app's ink is not part
     * of a system icon's identity). */
    g_stat_lookups++;
    const char *std = standard_name(name);
    if (std != NULL) {
        char skey[160];
        snprintf(skey, sizeof skey, "S|%s|%d", name, px);
        for (cached_icon *c = g_cache; c != NULL; c = c->next) {
            if (strcmp(c->key, skey) == 0) {
                g_last_tier = ICONS_TIER_SYSTEM;
                g_stat_system_hits++;
                snprintf(g_last_path, sizeof g_last_path, "%s",
                         c->path);
                return c->surface;
            }
        }
        /* a small mutable copy: [0]=std, then the alternates */
        const char *alts[4] = {std, NULL, NULL, NULL};
        size_t alts_n = 1;
        for (size_t i = 0;
             i < sizeof alt_names / sizeof alt_names[0]; i++) {
            if (strcmp(alt_names[i].name, std) == 0) {
                for (size_t k = 0; k < 3 && alt_names[i].alts[k] != NULL;
                     k++) {
                    alts[alts_n++] = alt_names[i].alts[k];
                }
                break;
            }
        }
        for (size_t a = 0; a < alts_n; a++) {
            /* §31/owner-round-3: PLAIN first. The symbolic-first
             * order tinted every hit through the alpha mask — on
             * themes that ship colored plains (Papirus et al) the
             * toolbar came out flat monochrome and looked like
             * "not using my theme" while the resolver was hitting
             * it on every lookup. Symbolic stays the fallback for
             * the monochrome-only themes (Adwaita's actions are
             * symbolic-only — owner round 4's box). */
            /* sym sits at ITERATION scope, not inside the fallback
             * if: `which` borrows it and is strdup'd at the store
             * below, after that block closes. The round-3 nested
             * declaration was a stack-use-after-scope — ASAN on
             * owner round 4's log, on every Adwaita boot. */
            char sym[128];
            fdk_surface *themed = fdk_icon_theme_surface(alts[a], px);
            const char *which = alts[a];
            if (themed == NULL) {
                snprintf(sym, sizeof sym, "%s-symbolic", alts[a]);
                themed = fdk_icon_theme_surface(sym, px);
                which = sym;
            }
            if (themed != NULL) {
                cached_icon *c = malloc(sizeof *c);
                if (c != NULL) {
                    snprintf(skey, sizeof skey, "S|%s|%d", name, px);
                    c->key = strdup(skey);
                    c->path = strdup(which);
                    c->surface = themed;
                    if (c->key != NULL && c->path != NULL) {
                        c->next = g_cache;
                        g_cache = c;
                        g_last_tier = ICONS_TIER_SYSTEM;
                        g_stat_system_hits++;
                        snprintf(g_last_path, sizeof g_last_path,
                                 "%s", which);
                        if (getenv("FDK_ICONS_TRACE") != NULL) {
                            fprintf(stderr, "icons: '%s' -> SYSTEM "
                                            "'%s'\n", name, which);
                        }
                        return c->surface;
                    }
                    free(c->key);
                    free(c->path);
                    free(c);
                }
                fdk_surface_destroy(themed);
                g_last_tier = ICONS_TIER_FALLBACK;
                g_last_path[0] = '\0';
                return NULL;
            }
        }
    }
    if (getenv("FDK_ICONS_TRACE") != NULL) {
        fprintf(stderr, "icons: '%s' -> system MISS, trying "
                        "bundled\n", name);
    }

    char key[128];
    snprintf(key, sizeof key, "%s|%d|%02x%02x%02x", name, px,
             (unsigned)(ink.r * 255.0f), (unsigned)(ink.g * 255.0f),
             (unsigned)(ink.b * 255.0f));
    for (cached_icon *c = g_cache; c != NULL; c = c->next) {
        if (strcmp(c->key, key) == 0) {
            g_last_tier = ICONS_TIER_BUNDLED;
            g_last_path[0] = '\0';
            return c->surface;
        }
    }

    /* 2. the bundled SVG, tinted (EMBEDDED — icons_data.h) */
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
            /* §31/FIX-E: SVG BYTES need the SVG rasterizer —
             * fdk_surface_create_from_image_bytes is stbi (raster
             * formats only) and WARN-failed every embedded icon;
             * the bundled tier never rendered anything until this
             * call was routed to the new SVG-from-memory API. */
            if (fdk_ok(fdk_surface_create_from_svg_bytes(
                    buf, px, &s)) &&
                s != NULL) {
                free(buf);
                cached_icon *c = malloc(sizeof *c);
                if (c != NULL) {
                    c->key = strdup(key);
                    c->path = NULL;
                    c->surface = s;
                    c->next = g_cache;
                    if (c->key != NULL) {
                        g_cache = c;
                        g_last_tier = ICONS_TIER_BUNDLED;
                        g_last_path[0] = '\0';
                        if (getenv("FDK_ICONS_TRACE") != NULL) {
                            fprintf(stderr, "icons: '%s' -> BUNDLED (embedded SVG, tinted)\n", name);
                        }
                        return s;
                    }
                    free(c);
                }
                fdk_surface_destroy(s);
                g_last_tier = ICONS_TIER_FALLBACK;
                g_last_path[0] = '\0';
                return NULL;
            }
            free(buf);
        }
    }

    /* 3. NULL — the caller falls back to fdk_row_icon glyphs */
    g_last_tier = ICONS_TIER_FALLBACK;
    g_last_path[0] = '\0';
    return NULL;
}

icons_tier icons_last_tier(char *out_path, size_t path_cap) {
    if (out_path != NULL && path_cap > 0) {
        snprintf(out_path, path_cap, "%s", g_last_path);
    }
    return g_last_tier;
}

void icons_theme_changed(void) {
    cached_icon *c = g_cache;
    while (c != NULL) {
        cached_icon *next = c->next;
        free(c->key);
        free(c->path);
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
