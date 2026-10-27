/*
 * theme_internal.h — internal layout of struct fdk_theme
 *
 * Not part of the public API — never installed. The public contract
 * lives in include/fdk/fdk_theme.h.
 *
 * Dependency note: theme.c and the widget layer call each other —
 * the catalog's paint hooks resolve tokens through the theme module,
 * and fdk_theme_set_default() asks the widget core (which owns the
 * live-root registry) to invalidate every tree. Both directions are
 * internal; this is the documented cycle in docs/architecture.md
 * (widget <-> theme), and C linkage resolves it without any header
 * gymnastics because each side only calls functions declared in the
 * other's internal header.
 */

#ifndef FDK_THEME_INTERNAL_H
#define FDK_THEME_INTERNAL_H

#include "fdk/fdk_theme.h"

#include <stdbool.h>
#include <stddef.h>

/* Cap from docs/fdk-theme-format.md: strings hold at most this many
 * bytes of content (name/author); the copy adds the terminator. */
#define FDK_THEME_STRING_MAX 128

/* Input cap (parse from memory and file): 1 MiB. */
#define FDK_THEME_INPUT_MAX (1024u * 1024u)

struct fdk_theme {
    char *name;   /* owned, never NULL ("Faded Dream" default) */
    char *author; /* owned, NULL when unset                  */
    char *path;   /* owned, NULL unless fdk_theme_load() —
                   * informational only (fdk_theme_file_path),
                   * never consulted for behavior            */

    /* Straight RGBA. Indexed by fdk_theme_token. */
    fdk_color colors[FDK_TK_COUNT];

    /* The OVERRIDE mask (1.4.16): colors_set[t] is true when a
     * [colors] key (or fdk_theme_set_color) put the value there,
     * false when it is merely the inherited built-in default. The
     * FALLBACK tokens (the titlebar family — see fdk__token_fallback)
     * consult it: an unset titlebar_background reads through to
     * control_background, so pre-1.4.16 partial themes theme the
     * band exactly as they always did. Base tokens are always
     * effectively set (the built-in initializes them), so only the
     * fallback family ever sees a false bit in practice. */
    bool colors_set[FDK_TK_COUNT];

    /* Indexed by fdk_theme_metric. */
    fdk_i32 metrics[FDK_TM_COUNT];
};

/* The fallback map (theme.c): for the titlebar family, the token a
 * read falls back to when the theme never overrode it. Returns the
 * token itself for every base token (the whole non-titlebar
 * vocabulary, and every titlebar token in a theme that set it). */
fdk_theme_token fdk__token_fallback(fdk_theme_token token);

/* The built-in default theme (v1 palette + metrics). A single static
 * instance, shared and never mutated: create_default() copies it, the
 * fallback current-theme pointer refers to it. */
const fdk_theme *fdk__theme_builtin(void);

/* The current default theme; never NULL. This is what a NULL `theme`
 * argument to the public accessors resolves to. Calling it (or
 * fdk_theme_set_default(), which resolves through it) runs the
 * one-shot global-settings boot on first use — see theme.c. */
fdk_theme *fdk__theme_current(void);

/* theme.c, TEST-only: rewind the one-shot global-settings boot (and
 * drop the theme it installed) so a test process can exercise
 * several boot scenarios. Used by tests/test_theme_discovery.c via
 * the internal-header include precedent (window_internal.h et al.);
 * never part of the installed API. */
void fdk__theme_boot_reset_for_tests(void);

/* ---- the settings engine (theme/settings.c, 1.4.14) --------------------
 *
 * Resolution ($FDK_THEME -> the app's own store -> the global store),
 * the developer whitelist, and the inotify live follow live in
 * settings.c; theme.c contributes the opt-out guard around the
 * current-default switch (below). The window layer bridges the
 * lifecycle and watch calls from core (window_internal.h). */

/* theme.c: install `t` as the SETTINGS-applied current default (NULL
 * reverts to the built-in). Identical to fdk_theme_set_default()
 * except it does NOT set the application opt-out flag — the settings
 * machinery is not an application. Repaints every live tree. */
void fdk__theme_set_default_settings(fdk_theme *theme);

/* theme.c: true once APPLICATION code called fdk_theme_set_default()
 * (or the boot ran after such a call) — the process's theme is the
 * application's property and the settings engine stands down. */
bool fdk__theme_settings_app_owned(void);

/* settings.c: the lazy boot's body (theme.c's one-shot guard calls
 * this). Resolves and applies. */
void fdk__theme_settings_boot(void);

/* settings.c: re-run resolution now (whitelist change, watched-file
 * change). No-op when the application owns the theme. */
void fdk__theme_settings_recheck(void);

/* settings.c: the inotify fd the pump should poll (-1 when nothing
 * is watched), and the drain for when it reports readable — returns
 * true when a watched settings file changed (and the recheck ran). */
int fdk__theme_settings_watch_fd(void);
bool fdk__theme_settings_watch_drain(void);

/* settings.c: lifecycle, bridged from core through window.c. */
void fdk__theme_settings_set_app_id(const char *app_id);
/* The current app id (borrowed; NULL when unset). */
const char *fdk__theme_settings_app_id(void);
void fdk__theme_settings_shutdown(void);

/* settings.c: fdk_theme_destroy's ownership slot — forget `theme` if
 * it is the theme the settings engine installed. */
void fdk__theme_settings_forget(fdk_theme *theme);

/* settings.c, TEST-only: rewind boot + installed-theme + whitelist
 * state for multi-scenario test processes. */
void fdk__theme_settings_reset_for_tests(void);

/* settings.c: free a (list, count) of owned strings (the whitelist's
 * storage; also used by set_allowed's rollback paths). */
void fdk__theme_settings_free_list(char **list, size_t count);

/* discover.c, TEST-only: forget the cached available-theme scan so
 * the next available_* call rescans — same contract and rationale as
 * the boot reset above (the cache is process-lifetime BY DESIGN;
 * only the test suite, which changes the environment mid-process,
 * needs to rewind it). */
void fdk__theme_scan_reset_for_tests(void);

/* parse.c: fills `t` (already initialized to a copy of the built-in
 * defaults) from the input, applying overrides. Returns FDK_OK or the
 * error; on error `t` is left in a destroyable state (partial strings
 * may have been swapped in — fdk_theme_destroy handles both). */
fdk_result fdk__theme_parse_into(fdk_theme *t, const char *text,
                                 size_t length);

/* theme.c: fdk_alloc'd copy of s (NULL -> NULL). Shared by the theme
 * module's own name/author handling; the parser uses it too. Kept
 * here rather than borrowing the widget catalog's fdk__strdup so the
 * theme layer never reaches into widget internals (the set_default
 * invalidation call is the one sanctioned cycle, and it goes the
 * other way). */
char *fdk__theme_strdup(const char *s);

/* ---- the icon-theme engine (icon_theme.c) ---------------------------
 *
 * The freedesktop icon-theme resolution infrastructure shared by
 * every internal icon consumer (today: the file/folder pickers).
 * NOT public API and never installed: applications get themed icons
 * through the toolkit's own widgets; exposing a lookup API is a
 * deliberate future decision, recorded in docs/roadmap.md.
 *
 * All functions are UI-thread-only (they stat, read small files,
 * and decode images synchronously — bounded work, but not
 * thread-safe). Everything caches until fdk__icon_theme_reset(). */

/* The kinds of place the pickers' sidebars resolve icons for. */
typedef enum {
    FDK_ICON_PLACE_HOME = 0,
    FDK_ICON_PLACE_RECENT,
    FDK_ICON_PLACE_DRIVE,
    FDK_ICON_PLACE_FOLDER,
} fdk_icon_place_kind;

/* Drops every cache: parsed index.theme data, (name,size) lookups
 * (positive AND negative), decoded surfaces, and the detected
 * current-theme name. CONTRACT: callers must ensure no widget still
 * borrows a cached surface — the file dialogs call this at CREATION
 * (before any row exists), and the test suite owns its surfaces.
 * The next current-name read re-reads the desktop's settings, so a
 * changed icon theme is picked up by the next dialog, never served
 * stale from a previous session. */
void fdk__icon_theme_reset(void);

/* The user's selected icon theme name (static storage, cached until
 * reset). Detection, no bus: $FDK_ICON_THEME > XSETTINGS live (the
 * X11 channel the desktop's own GTK apps obey — see
 * src/platform/x11/x11_xsettings.c) > gtk-3.0/gtk-4.0
 * settings.ini gtk-icon-theme-name > XFCE xfconf's xsettings.xml
 * IconThemeName > kdeglobals [Icons] Theme > the classic icon-theme
 * file > system gtk settings.ini (/etc, /usr/share) > the search
 * path's own default/index.theme Inherits= > "hicolor". */
const char *fdk__icon_theme_current_name(void);

/* ---- the live XSETTINGS seam (src/platform/x11/x11_xsettings.c) ------
 *
 * Reads one STRING setting from the XSETTINGS manager (if any) on
 * the default display, using the reader's own short-lived X
 * connection. Implemented in the X11 backend — the only layer
 * allowed to include Xlib — and declared here because the icon
 * engine consumes it. Returns false (out untouched) when there is
 * no display, no manager, or the property will not parse: the
 * detection chain then falls through to its file channels. */
bool fdk__x11_xsettings_string(const char *setting_name, char *out,
                               size_t cap);

/* ---- the color-scheme module (1.1.0, color_scheme.c) --------------------
 *
 * The desktop's light/dark preference: sourced from XSETTINGS
 * (X11) or org.freedesktop.portal.Settings (the D-Bus client),
 * cached for the public getter, and fed to theme resolution's
 * variant selection. */

/* Re-reads XSETTINGS Gtk/ColorScheme (own short-lived connection;
 * no-op without a display). Called lazily at the first query and
 * on the backend's PropertyNotify for the settings property. */
void fdk__color_scheme_poll_x11(void);

/* True when the XSETTINGS channel ever produced a value (the
 * portal is the fallback otherwise). */
bool fdk__color_scheme_x11_spoke(void);

/* Attaches the portal singleton when appropriate (called from the
 * pump's theme tick; 30 s retry throttle after a failure). */
void fdk__color_scheme_portal_tick(fdk_context *ctx);

/* Closes the portal singleton (fdk_shutdown's theme teardown). */
void fdk__color_scheme_shutdown(void);

/* Test seam: full module-state reset. */
void fdk__color_scheme_reset_for_tests(void);

/* Resolves one icon name at one size through the full chain (the
 * current theme, its Inherits, hicolor, then /usr/share/pixmaps).
 * Returns an fdk_alloc'd absolute path (caller frees) or NULL.
 * PNG or SVG: same-directory probes prefer PNG (exact pixels) and
 * fall back to the vendored nanosvg rasterizer for SVG (rendered
 * at the REQUESTED size). XPM is never offered — no decoder. */
char *fdk__icon_theme_lookup(const char *icon_name, fdk_i32 size);

/* ---- the SVG policy layer (icon_svg.c) -------------------------------
 *
 * Rasterizes ONE themed SVG file into an fdk_surface at the exact
 * requested size, with the same reject-first/bounded/honest-miss
 * discipline as surface_image.c (see icon_svg.c's header for the
 * full policy). Used only by icon_theme.c's decode step. */
fdk_result fdk__icon_svg_raster(const char *path, fdk_i32 size,
                                fdk_surface **out_surface);

/* Decodes (once; cached; borrowed until reset) the icon at the
 * requested size. A candidate whose name ends in "-symbolic" is
 * tinted through its alpha mask with the current theme's text ink.
 * NULL when nothing decodable exists — the caller falls back to the
 * procedural glyph set, never to a wrong-type icon. */
fdk_surface *fdk__icon_theme_surface(const char *icon_name, fdk_i32 size);

/* Resolves the FIRST chain entry that yields a decodable surface
 * (borrowed, cached), or NULL when the whole chain misses. */
fdk_surface *fdk__icon_theme_surface_chain(const char *const *names,
                                           fdk_i32 size);

/* Classification: the candidate-name chain for a filesystem entry.
 * `name` is matched case-insensitively against the extension table
 * (images / audio / video / archives / documents / fonts / source
 * code / system files); unknown or extension-less files pay ONE
 * stat to split on the executable bit. Every chain ends in a
 * generic virtually every theme ships. Returns static storage. */
const char *const *fdk__icon_chain_for_entry(const char *dir,
                                             const char *name,
                                             bool is_dir);
/* The extension-table chain for a bare name, or NULL when the
 * extension is unknown (the caller decides what that means). */
const char *const *fdk__icon_chain_for_name(const char *name);

/* The places-sidebar chains (symbolic-first, per the desktop norm). */
const char *const *fdk__icon_chain_place(fdk_icon_place_kind kind);


/* ---- the ui-metrics module (1.1.0, ui_metrics.c) --------------------------
 *
 * The process's UI font size and scale (see fdk_theme.h's public
 * getters). Same live channels as the color scheme: this X11 poll
 * (the backend's PropertyNotify), the portal tick (the pump), the
 * settings-file invalidation. */

/* A historically-fixed widget/chrome font size, re-derived for the
 * current UI metrics: round(ref * fdk_ui_font_px() / 14), clamped
 * to [8, 96]. The 14 baseline is the size the widget set was
 * designed against (the shells' old hardcode); at
 * FDK_UI_FONT_SIZE=14 every value lands exactly on its historical
 * pixel size — the backward-compatible escape hatch. */
int fdk__ui_derived_px(int reference_px);

void fdk__ui_metrics_poll_x11(void);
void fdk__ui_metrics_portal_tick(fdk_context *ctx);
void fdk__ui_metrics_shutdown(void);
void fdk__ui_metrics_invalidate(void);

#endif /* FDK_THEME_INTERNAL_H */
