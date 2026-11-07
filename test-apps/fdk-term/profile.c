/* profile.c — prefs-backed profiles (see profile.h). Keys live
 * under "profile.<name>." so the fdk-prefs CLI addresses them. */
#include "profile.h"
#include "term_app.h"  /* tapp + the fdk_prefs handle */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void key_for(char *out, size_t cap, const char *prof,
                    const char *leaf) {
    const char *p = (prof != NULL && prof[0] != '\0')
                            ? prof : "default";
    snprintf(out, cap, "profile.%s.%s", p, leaf);
}

/* tapp's typed prefs reads (fdk_prefs raw — tapp is not ta_app). */
static int pp_int(struct tapp *app, const char *key, int def) {
    if (app->prefs == NULL) return def;
    const char *s = fdk_prefs_get(app->prefs, key, NULL);
    return (s != NULL && s[0] != '\0') ? atoi(s) : def;
}
static bool pp_bool(struct tapp *app, const char *key, bool def) {
    if (app->prefs == NULL) return def;
    return fdk_prefs_get_bool(app->prefs, key, def);
}
static double pp_double(struct tapp *app, const char *key, double def) {
    if (app->prefs == NULL) return def;
    const char *s = fdk_prefs_get(app->prefs, key, NULL);
    return (s != NULL && s[0] != '\0') ? atof(s) : def;
}
static void pp_str(struct tapp *app, const char *key, const char *def,
                   char *out, size_t cap) {
    const char *s = app->prefs != NULL ? fdk_prefs_get(app->prefs, key, NULL)
                                       : NULL;
    if (s != NULL) snprintf(out, cap, "%s", s);
    else if (def != NULL) snprintf(out, cap, "%s", def);
    else out[0] = '\0';
}

void profile_load(struct tapp *app, const char *name, tprofile *out) {
    /* the defaults: a comfortable modern terminal */
    out->font_px = 13;
    out->scheme = TSCHEME_FOLLOW;
    out->shell[0] = '\0';
    out->workdir[0] = '\0';
    out->login_shell = false;
    out->scrollback = 10000;
    out->copy_on_select = true;
    out->bell_audible = true;
    out->bell_visual = true;
    out->bell_notify = true;
    out->opacity = 1.0;
    out->cursor_blink_ms = 530;

    if (app == NULL || app->prefs == NULL) {
        return;
    }
    char key[160];
    key_for(key, sizeof key, name, "font_px");
    out->font_px = pp_int(app, key, out->font_px);
    if (out->font_px < 8) {
        out->font_px = 8;
    }
    if (out->font_px > 32) {
        out->font_px = 32;
    }
    key_for(key, sizeof key, name, "scheme");
    char sname[32];
    pp_str(app, key, "follow-theme", sname, sizeof sname);
    tscheme s;
    if (tscheme_parse(sname, &s)) {
        out->scheme = s;
    }
    key_for(key, sizeof key, name, "shell");
    pp_str(app, key, "", out->shell, sizeof out->shell);
    key_for(key, sizeof key, name, "workdir");
    pp_str(app, key, "", out->workdir, sizeof out->workdir);
    key_for(key, sizeof key, name, "login_shell");
    out->login_shell = pp_bool(app, key, out->login_shell);
    key_for(key, sizeof key, name, "scrollback");
    out->scrollback = pp_int(app, key, out->scrollback);
    if (out->scrollback < 0) {
        out->scrollback = 0;
    }
    if (out->scrollback > 1000000) {
        out->scrollback = 1000000;
    }
    key_for(key, sizeof key, name, "copy_on_select");
    out->copy_on_select = pp_bool(app, key, out->copy_on_select);
    key_for(key, sizeof key, name, "bell_audible");
    out->bell_audible = pp_bool(app, key, out->bell_audible);
    key_for(key, sizeof key, name, "bell_visual");
    out->bell_visual = pp_bool(app, key, out->bell_visual);
    key_for(key, sizeof key, name, "bell_notify");
    out->bell_notify = pp_bool(app, key, out->bell_notify);
    key_for(key, sizeof key, name, "opacity");
    out->opacity = pp_double(app, key, out->opacity);
    if (!(out->opacity >= 0.05 && out->opacity <= 1.0)) {
        out->opacity = 1.0;
    }
    key_for(key, sizeof key, name, "cursor_blink_ms");
    out->cursor_blink_ms = pp_int(app, key, out->cursor_blink_ms);
    if (out->cursor_blink_ms < 0 || out->cursor_blink_ms > 2000) {
        out->cursor_blink_ms = 0;
    }
}

void profile_save(struct tapp *app, const char *name,
                  const tprofile *p) {
    if (app == NULL || app->prefs == NULL) {
        return;
    }
    char key[160];
    key_for(key, sizeof key, name, "font_px");
    (void)fdk_prefs_set_int(app->prefs, key, p->font_px);
    key_for(key, sizeof key, name, "scheme");
    (void)fdk_prefs_set(app->prefs, key, tscheme_name(p->scheme));
    key_for(key, sizeof key, name, "shell");
    (void)fdk_prefs_set(app->prefs, key, p->shell);
    key_for(key, sizeof key, name, "workdir");
    (void)fdk_prefs_set(app->prefs, key, p->workdir);
    key_for(key, sizeof key, name, "login_shell");
    (void)fdk_prefs_set_bool(app->prefs, key, p->login_shell);
    key_for(key, sizeof key, name, "scrollback");
    (void)fdk_prefs_set_int(app->prefs, key, p->scrollback);
    key_for(key, sizeof key, name, "copy_on_select");
    (void)fdk_prefs_set_bool(app->prefs, key, p->copy_on_select);
    key_for(key, sizeof key, name, "bell_audible");
    (void)fdk_prefs_set_bool(app->prefs, key, p->bell_audible);
    key_for(key, sizeof key, name, "bell_visual");
    (void)fdk_prefs_set_bool(app->prefs, key, p->bell_visual);
    key_for(key, sizeof key, name, "bell_notify");
    (void)fdk_prefs_set_bool(app->prefs, key, p->bell_notify);
    key_for(key, sizeof key, name, "opacity");
    (void)fdk_prefs_set_double(app->prefs, key, p->opacity);
    key_for(key, sizeof key, name, "cursor_blink_ms");
    (void)fdk_prefs_set_int(app->prefs, key, p->cursor_blink_ms);
    (void)fdk_prefs_save(app->prefs);
}

/* The live-profile accessors the view uses. tapp is fdk-term's app
 * (app.h below); the ACTIVE profile is app->active. */
bool profile_copy_on_select(struct tapp *app) {
    return app != NULL && app->active.copy_on_select;
}

tscheme profile_active_scheme(struct tapp *app) {
    return app != NULL ? app->active.scheme : TSCHEME_FOLLOW;
}
