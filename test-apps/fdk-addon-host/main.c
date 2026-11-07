/* main.c — fdk-addon-host: the dlopen side of the add-on proof.
 *
 * Loads libfdk-hud.so (built against an INSTALLED FDK by
 * ../fdk-addon/isolated_build.sh) at runtime, binds its fdk_hud_*
 * API, and drives every piece of it: the panel surface, the gauge
 * API, the command pipe (fdk_ext_loop_add_fd's source), the popup
 * menu, and the full shutdown path. The receipt at exit — kind
 * create/destroy counts, expose repaints, commands processed — is
 * what the smoke run and the visual rig assert on.
 *
 * The host itself links the shared in-tree libfdk: host and add-on
 * then resolve fdk_* from the ONE loaded libfdk.so.1 (the soname
 * dedup), which is the whole model — see docs/EXTENDING.md §11.
 */

#include "app.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../fdk-addon/hud.h" /* the add-on's OWN public header (the
                               * calls go through dlsym, not the link
                               * table) */

/* ---- the dlsym'd API ---- */

typedef struct hud_api {
    int (*api_version)(void);
    fdk_result (*init)(fdk_context *, const fdk_hud_options *,
                       fdk_hud **);
    void (*show)(fdk_hud *);
    void (*hide)(fdk_hud *);
    void (*shutdown)(fdk_hud *);
    void (*set_value)(fdk_hud *, float);
    void (*set_label)(fdk_hud *, const char *);
    void (*show_menu)(fdk_hud *);
    int (*command_fd)(fdk_hud *);
    fdk_hud_stats (*stats_read)(fdk_hud *);
} hud_api;

typedef struct host {
    ta_app ta;
    void *so;
    hud_api api;
    fdk_hud *hud;
    fdk_widget *status;
    fdk_timer *feed;
    long ticks;
    int fd;
    bool load_ok;
    char lib_path[512];
} host;

#define SYM(h, field, name)                                              \
    do {                                                                 \
        *(void **)&((h)->api.field) = dlsym((h)->so, name);              \
        if ((h)->api.field == NULL) {                                    \
            fprintf(stderr, "dlsym %s: %s\n", name, dlerror());          \
            return false;                                                \
        }                                                                \
    } while (0)

static bool bind_api(host *h) {
    SYM(h, api_version, "fdk_hud_api_version");
    SYM(h, init, "fdk_hud_init");
    SYM(h, show, "fdk_hud_show");
    SYM(h, hide, "fdk_hud_hide");
    SYM(h, shutdown, "fdk_hud_shutdown");
    SYM(h, set_value, "fdk_hud_set_value");
    SYM(h, set_label, "fdk_hud_set_label");
    SYM(h, show_menu, "fdk_hud_show_menu");
    SYM(h, command_fd, "fdk_hud_command_fd");
    SYM(h, stats_read, "fdk_hud_stats_read");
    return true;
}

static void host_status(host *h, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fdk_label_set_text(h->status, buf);
}

/* the slider: the host's own UI drives the add-on's gauge through
 * the dlsym'd API — two packages, one process, one libfdk */
static void on_slider(fdk_widget *slider, void *user) {
    host *h = user;
    double v = fdk_slider_get_value(slider);
    h->api.set_value(h->hud, (float)(v / 100.0));
    host_status(h, "slider -> set_value(%.2f)", v / 100.0);
}

static void on_menu(fdk_widget *button, void *user) {
    (void)button;
    host *h = user;
    h->api.show_menu(h->hud);
    host_status(h, "show_menu -> popup");
}

static void on_hide(fdk_widget *button, void *user) {
    (void)button;
    host *h = user;
    h->api.hide(h->hud);
    host_status(h, "hide -> unmapped");
}

static void on_show(fdk_widget *button, void *user) {
    (void)button;
    host *h = user;
    h->api.show(h->hud);
    host_status(h, "show -> mapped + repainted");
}

/* the scripted feed: writes commands into the add-on's pipe — the
 * fdk_ext_loop_add_fd path — and opens the menu once so the visual
 * rig can capture the popup. Timed for the smoke budget (40 frames
 * ≈ 0.7 s): the menu opens at ~0.36 s, the value commands land
 * before the exit. */
static void on_feed(fdk_timer *timer, void *user) {
    (void)timer;
    host *h = user;
    h->ticks++;
    char line[64];
    if (h->ticks == 3) {
        h->api.show_menu(h->hud);
    } else if (h->ticks % 2 == 0) {
        snprintf(line, sizeof line, "value %.2f\n",
                 0.15f + 0.7f * (float)((h->ticks / 2) % 5) / 4.0f);
        if (h->fd >= 0 && write(h->fd, line, strlen(line)) < 0) {
            /* the demo never fills the pipe; a short write is fine */
        }
    } else {
        snprintf(line, sizeof line, "label feed-%ld\n", h->ticks / 2);
        if (h->fd >= 0 && write(h->fd, line, strlen(line)) < 0) {
        }
    }
    fdk_hud_stats st = h->api.stats_read(h->hud);
    host_status(h,
                "addon: creates=%d destroys=%d exposes=%d cmds=%d "
                "menus=%d",
                st.kind_creates, st.kind_destroys, st.exposes,
                st.commands, st.menu_opens);
}

int main(int argc, char **argv) {
    host h;
    memset(&h, 0, sizeof h);
    h.fd = -1;

    if (!ta_init(&h.ta, "addon-host")) {
        return 1;
    }
    if (!ta_open(&h.ta, "fdk-addon-host", 760, 420)) {
        return 1;
    }

    /* the library: the first non-flag argv, $FDK_HUD_LIB, or the
     * in-tree build dir (the smoke target runs us with --smoke) */
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            path = argv[i];
            break;
        }
    }
    if (path == NULL) {
        path = getenv("FDK_HUD_LIB");
    }
    if (path == NULL) {
        path = "build/libfdk-hud.so";
    }
    snprintf(h.lib_path, sizeof h.lib_path, "%s", path);
    h.so = dlopen(h.lib_path, RTLD_NOW | RTLD_LOCAL);
    if (h.so == NULL) {
        fprintf(stderr, "dlopen %s: %s\n", h.lib_path, dlerror());
        ta_status(&h.ta, "ADDON LOAD FAILED (see stderr)");
        ta_run(&h.ta);
        ta_close(&h.ta);
        return 1;
    }
    if (!bind_api(&h)) {
        ta_close(&h.ta);
        return 1;
    }
    int av = h.api.api_version();
    printf("addon api version: %d (expected %d)\n", av,
           FDK_HUD_API_VERSION);

    /* the content: one box, the explainer label, the slider, the
     * button row */
    fdk_widget *col = NULL;
    if (fdk_ok(fdk_box_create(h.ta.content, FDK_VERTICAL, &col))) {
        fdk_box_set_padding(col, 18);
        fdk_box_set_spacing(col, 10);

        fdk_widget *intro = NULL;
        if (fdk_ok(fdk_label_create(
                col, h.ta.font_ui,
                "fdk-hud loaded via dlopen — the panel top-right is "
                "the add-on's own surface kind",
                &intro))) {
            fdk_widget_set_expand(intro, true, false);
        }

        fdk_widget *slider = NULL;
        if (fdk_ok(fdk_slider_create(col, 0.0, 100.0, 30.0, &slider))) {
            fdk_slider_set_on_changed(slider, on_slider, &h);
        }

        fdk_widget *row = NULL;
        if (fdk_ok(fdk_box_create(col, FDK_HORIZONTAL, &row))) {
            fdk_box_set_spacing(row, 8);
            fdk_widget *b1 = NULL, *b2 = NULL, *b3 = NULL;
            if (fdk_ok(fdk_button_create(row, h.ta.font_ui, "Menu",
                                         &b1))) {
                fdk_button_set_on_activate(b1, on_menu, &h);
            }
            if (fdk_ok(fdk_button_create(row, h.ta.font_ui, "Hide",
                                         &b2))) {
                fdk_button_set_on_activate(b2, on_hide, &h);
            }
            if (fdk_ok(fdk_button_create(row, h.ta.font_ui, "Show",
                                         &b3))) {
                fdk_button_set_on_activate(b3, on_show, &h);
            }
        }

        fdk_widget *st = NULL;
        if (fdk_ok(fdk_label_create(col, h.ta.font_ui, "addon: -", &st))) {
            h.status = st;
        }
    }

    /* load the add-on */
    fdk_hud_options o;
    memset(&o, 0, sizeof o);
    o.label = "cpu";
    o.value = 0.30f;
    o.x = 700;
    o.y = 90;
    fdk_result r = h.api.init(h.ta.ctx, &o, &h.hud);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_hud_init: %s\n", fdk_result_to_string(r));
        ta_status(&h.ta, "ADDON INIT FAILED: %s",
                  fdk_result_to_string(r));
        ta_run(&h.ta);
        ta_close(&h.ta);
        return 1;
    }
    h.fd = h.api.command_fd(h.hud);
    h.api.show(h.hud);
    h.feed = fdk_timer_add(h.ta.ctx, 120, true, on_feed, &h);

    ta_run(&h.ta);
    fdk_timer_remove(h.feed); /* quiet teardown: no shutdown warning */

    /* ---- the receipt ---- */
    fdk_hud_stats st = h.api.stats_read(h.hud);
    printf("addon receipt: creates=%d destroys=%d exposes=%d "
           "commands=%d menu_opens=%d menu_picks=%d\n",
           st.kind_creates, st.kind_destroys, st.exposes, st.commands,
           st.menu_opens, st.menu_picks);
    h.api.shutdown(h.hud);
    fdk_hud_stats after = h.api.stats_read(NULL);
    printf("after shutdown (final counters): creates=%d destroys=%d\n",
           after.kind_creates, after.kind_destroys);

    int fails = 0;
    if (av != FDK_HUD_API_VERSION) {
        printf("FAIL api version\n");
        fails++;
    }
    if (st.kind_creates != 1 || after.kind_destroys != 1) {
        printf("FAIL kind lifecycle\n");
        fails++;
    }
    if (st.exposes == 0) {
        printf("FAIL no expose repaints served\n");
        fails++;
    }
    if (st.commands < 3) {
        printf("FAIL command pipe barely used (%d)\n", st.commands);
        fails++;
    }
    if (st.menu_opens < 1) {
        printf("FAIL popup never opened\n");
        fails++;
    }
    dlclose(h.so);
    ta_close(&h.ta);
    if (fails == 0) {
        printf("ADDON-HOST PASS\n");
        return 0;
    }
    printf("ADDON-HOST FAIL (%d checks)\n", fails);
    return 1;
}
