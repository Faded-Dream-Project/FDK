/* profile.h — terminal profiles: font size, scheme, shell, working
 * dir, login-shell, scrollback size, copy-on-select, bell mode,
 * opacity. Stored through FDK prefs (fdk-prefs CLI configurable);
 * the live profile is "default" plus per-tab overrides. */
#ifndef FDKTERM_PROFILE_H
#define FDKTERM_PROFILE_H

#include "colors.h"

#include <stdbool.h>

struct tapp;

typedef struct tprofile {
    int font_px;            /* 8..32                              */
    tscheme scheme;
    char shell[128];        /* empty = $SHELL / /bin/sh           */
    char workdir[512];      /* empty = home/cwd                   */
    bool login_shell;
    int scrollback;         /* lines; 0..1000000, default 10000   */
    bool copy_on_select;
    bool bell_audible;
    bool bell_visual;
    bool bell_notify;       /* long-command-finish notification   */
    double opacity;         /* 1.0 = opaque                       */
    int cursor_blink_ms;    /* 0 = static cursor                  */
} tprofile;

/* Loads the profile named `name` from prefs ("default" when NULL/
 * empty; unknown names load defaults). */
void profile_load(struct tapp *app, const char *name, tprofile *out);

/* Persists the profile back to prefs. */
void profile_save(struct tapp *app, const char *name, const tprofile *p);

/* The app-level accessors the view uses (they read the ACTIVE
 * profile of the app, not a tab's copy). */
bool profile_copy_on_select(struct tapp *app);
tscheme profile_active_scheme(struct tapp *app);

#endif /* FDKTERM_PROFILE_H */
