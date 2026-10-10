/*
 * prefs_internal.h — core-internal preferences surface (1.4.14)
 *
 * Not part of the public API — never installed. The public contract
 * lives in include/fdk/fdk_prefs.h.
 *
 * The one consumer of this header outside core is the theme settings
 * engine (src/theme/settings.c), which needs the file a store WOULD
 * resolve to without opening one — its inotify watch has to watch the
 * directory the store will land in, including before the first save
 * ever creates the file.
 */

#ifndef FDK_PREFS_INTERNAL_H
#define FDK_PREFS_INTERNAL_H

#include <stddef.h>

/* prefs.c: the file the store for `app_id` resolves to, WITHOUT
 * opening it — $FDK_PREFS_FILE (absolute), else
 * $XDG_CONFIG_HOME/<app_id>.prefs, else $HOME/.config/<app_id>.prefs.
 * fdk_alloc'd copy the caller fdk_free's; NULL when nothing resolves
 * (the memory-only store case). Same resolution fdk_prefs_open()
 * applies, factored out so both stay in lockstep by construction. */
char *fdk__prefs_resolve_path(const char *app_id);

#endif /* FDK_PREFS_INTERNAL_H */
