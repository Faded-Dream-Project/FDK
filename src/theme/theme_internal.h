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

#endif /* FDK_THEME_INTERNAL_H */
