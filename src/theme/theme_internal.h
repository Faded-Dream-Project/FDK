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
    char *name;   /* owned, never NULL ("FDK Dark" default) */
    char *author; /* owned, NULL when unset                  */

    /* Straight RGBA. Indexed by fdk_theme_token. */
    fdk_color colors[FDK_TK_COUNT];

    /* Indexed by fdk_theme_metric. */
    fdk_i32 metrics[FDK_TM_COUNT];
};

/* The built-in default theme (v1 palette + metrics). A single static
 * instance, shared and never mutated: create_default() copies it, the
 * fallback current-theme pointer refers to it. */
const fdk_theme *fdk__theme_builtin(void);

/* The current default theme; never NULL. This is what a NULL `theme`
 * argument to the public accessors resolves to. */
fdk_theme *fdk__theme_current(void);

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
