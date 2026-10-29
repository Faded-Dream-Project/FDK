/*
 * test_wayland_integration.c — Wayland platform integration test.
 *
 * Runs against a REAL compositor: start weston headless first, e.g.
 *
 *   weston --backend=headless-backend.so --shell=kiosk-shell.so \
 *          --socket=wl-fdk --idle-time=0
 *   WAYLAND_DISPLAY=wl-fdk ./build/tests/test_wayland_integration
 *
 * (scripts/run_wayland_tests.sh does exactly that.) The environment
 * dictates what can be asserted:
 *
 *  - kiosk-shell headless has NO wl_seat (no input devices), so this
 *    test exercises the programmatic surface: decoration mode
 *    negotiation, maximize/unmaximize via xdg_toplevel, minimize
 *    request, configure-driven state events, and pixel verification
 *    through FDK's own framebuffer (what we DREW — the compositor's
 *    end of the pipeline is verified separately by the screenshot
 *    rig with PIL).
 *
 *  - The decoration outcome is compositor policy: compositors that
 *    advertise xdg-decoration may confirm CLIENT_SIDE (band stays,
 *    drawn by FDK) or force SERVER_SIDE (FDK must drop its band +
 *    emit FDK_EVENT_WINDOW_DECORATION) — both real, correct
 *    outcomes, asserted by reaction rather than by one specific
 *    answer. Compositors WITHOUT the protocol (kiosk-shell weston
 *    here, Muffin's experimental Wayland session on real desktops)
 *    never draw chrome at all — client-side is the xdg-shell
 *    default, so the band must simply come up (1.1.5: it used to
 *    return FDK_ERR_UNSUPPORTED, which killed the decorations demo
 *    before it ever mapped a window).
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_dialog.h"
#include "fdk/fdk_theme.h"
#include "fdk/fdk_widgets.h"
#include "fdk/fdk_idle.h"

/* Internal seam (same discipline as test_x11_integration.c): the
 * deferred-first-frame regression below asserts backend commit state
 * through the sanctioned ops wrapper — the Wayland platform header
 * itself never leaves src/platform/wayland/. */
#include "window/window_internal.h"
/* 1.3.5: the key-repeat section drives the platform's keyboard
 * listener state machine through the ops test seams (see
 * platform_internal.h) — same internal-seam discipline. */
#include "core/context_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h> /* free() for clipboard strings */
#include <string.h>
#include <time.h>   /* nanosleep — the pacing regression's guard wait */
#include <fcntl.h>
#include <unistd.h> /* access() — the injector-availability probe */
#include <poll.h>

/* IME end-to-end (1.1.0, M-1): this test ALSO plays the input
 * method — a second wl_display connection binding
 * zwp_input_method_manager_v2 (the fcitx5 role) and driving the
 * app's text-input-v3 through the REAL compositor relay. The
 * generated protocol code is TEST-ONLY (the library itself ships
 * only the client side), compiled straight into this binary. */
#include <wayland-client.h>
#include "platform/wayland/generated/input-method-unstable-v2-client-protocol.h"

static fdk_dialog_response g_wayland_dlg_last =
    (fdk_dialog_response)-99;

/* 1.3.5 key-repeat counters (the repeat section's window callback). */
static int g_rep_downs = 0;
static int g_rep_repeats = 0;
static long long g_rep_first_ms = 0;
static long long g_rep_press_ms = 0;

/* Touch-slot capture (1.1.0, M-3): raw slot events + the synthesis
 * the core engine layers on top (click pairs, scrolls). */
typedef struct {
    int touch_downs, touch_ups, touch_cancels;
    int clicks, releases;
    int scrolls;
    fdk_f32 last_scroll_dx, last_scroll_dy;
    fdk_pointf click_pos;
} wl_touch_capture;
static wl_touch_capture g_wl_touch;

static void wl_touch_evt_cb(fdk_window *w, const fdk_event_data *ev,
                            void *user) {
    (void)w;
    (void)user;
    switch (ev->type) {
    case FDK_EVENT_TOUCH_DOWN:
        g_wl_touch.touch_downs++;
        break;
    case FDK_EVENT_TOUCH_UP:
        g_wl_touch.touch_ups++;
        break;
    case FDK_EVENT_TOUCH_CANCEL:
        g_wl_touch.touch_cancels++;
        break;
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        g_wl_touch.clicks++;
        g_wl_touch.click_pos = ev->pointer_button.position;
        break;
    case FDK_EVENT_POINTER_BUTTON_UP:
        g_wl_touch.releases++;
        break;
    case FDK_EVENT_POINTER_SCROLL:
        g_wl_touch.scrolls++;
        g_wl_touch.last_scroll_dx = ev->scroll.delta_x;
        g_wl_touch.last_scroll_dy = ev->scroll.delta_y;
        break;
    default:
        break;
    }
}

static long long rep_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL +
           (long long)ts.tv_nsec / 1000000LL;
}

static void rep_window_cb(fdk_window *w, const fdk_event_data *ev,
                          void *user) {
    (void)w;
    (void)user;
    if (ev->type == FDK_EVENT_KEY_DOWN) {
        if (ev->key.is_repeat) {
            g_rep_repeats++;
            if (g_rep_first_ms == 0) {
                g_rep_first_ms = rep_now_ms();
            }
        } else {
            g_rep_downs++;
        }
    }
}

static void wayland_dialog_response_cb(fdk_dialog_response response,
                                       void *user) {
    int *count = user;
    (*count)++;
    g_wayland_dlg_last = response;
}

static int g_state_events = 0;
static int g_deco_events = 0;
static int g_last_maximized = -1;
static int g_last_minimized = -1;


/* ---- Virtual-pointer input injection (the interactive section) ----
 *
 * fdk-wl-inject (scripts/, test infra) creates a
 * zwlr_virtual_pointer_v1 device and serves commands over stdin for
 * as long as the pipe is open — the device, and therefore the seat's
 * POINTER CAPABILITY, lives exactly that long (clients binding
 * wl_pointer on the capabilities event, as FDK does, then receive
 * REAL motion/button events with REAL serials — the only source of
 * grab-valid serials under a headless compositor).
 *
 * Absent tooling (plain weston, no injector binary): the
 * interactive section honestly skips. */

#include <stdlib.h>

static FILE *g_injector = NULL;

static bool wayland_injector_start(void) {
    /* popen() hands back a perfectly valid stream even when the
     * command does not exist (sh: not found) — the first write then
     * dies with SIGPIPE and takes the whole test with it, breaking
     * the "absent tooling honestly skips" contract this section is
     * built on. Verify the binary is actually executable first. */
    if (access("/home/z/my-project/scripts/fdk-wl-inject", X_OK) != 0) {
        return false;
    }
    /* ...and that THIS compositor speaks the wlr virtual-pointer
     * protocol: the injector drops a ready-file once the device
     * really exists (weston kiosk-shell has no zwlr manager; the
     * injector exits and any write would SIGPIPE — the file-based
     * handshake keeps the skip honest per-compositor, not just
     * per-machine). */
    char ready_path[128];
    (void)snprintf(ready_path, sizeof ready_path,
                   "/tmp/fdk-wl-inject.%ld.ready", (long)getpid());
    unlink(ready_path);
    char cmd[256];
    (void)snprintf(cmd, sizeof cmd,
                   "FDK_WL_INJECT_READY=%s "
                   "/home/z/my-project/scripts/fdk-wl-inject serve",
                   ready_path);
    g_injector = popen(cmd, "w");
    if (g_injector == NULL) {
        return false;
    }
    for (int i = 0; i < 40; i++) { /* up to 2s for the device to exist */
        if (access(ready_path, F_OK) == 0) {
            unlink(ready_path);
            return true;
        }
        /* pump while waiting: the injector connects in parallel and
         * the compositor needs no help, but sleeping is free. */
        struct timespec tick = { .tv_sec = 0, .tv_nsec = 50L * 1000L * 1000L };
        nanosleep(&tick, NULL);
    }
    pclose(g_injector);
    g_injector = NULL;
    unlink(ready_path);
    return false;
}

static void wayland_inject(const char *cmd) {
    if (g_injector == NULL) {
        return;
    }
    fprintf(g_injector, "%s\n", cmd);
    fflush(g_injector);
}

static void wayland_injector_stop(void) {
    if (g_injector != NULL) {
        pclose(g_injector); /* EOF: the virtual pointer dies with it */
        g_injector = NULL;
    }
}

/* sway's own view of which window is focused (S-6): the compositor's
 * tree, queried over the rig's IPC socket, walked for the focused
 * toplevel and its title (the "name" node field). Returns a static
 * buffer the caller must NOT free, or NULL when swaymsg / the
 * socket / the parse is unavailable (the caller skips honestly).
 *
 * The python3 walker is the same shape the rig's geometry scripts
 * use: recurse dicts/lists, print the focused node's name. Nothing
 * client-side is trusted — focus truth belongs to the compositor. */
static const char *sway_focused_window_name(void) {
    static char name[128];
    name[0] = '\0';
    const char *py =
        "python3 -c 'import json,sys\n"
        "def walk(n):\n"
        "  if isinstance(n, dict):\n"
        "    if n.get(\"focused\") and n.get(\"app_id\"):\n"
        "      print(n.get(\"name\", \"\"))\n"
        "    for v in n.values(): walk(v)\n"
        "  elif isinstance(n, list):\n"
        "    for v in n: walk(v)\n"
        "walk(json.load(sys.stdin))'";
    char cmd[512];
    (void)snprintf(cmd, sizeof cmd, "swaymsg -t get_tree 2>/dev/null | %s",
                   py);
    FILE *f = popen(cmd, "r");
    if (f == NULL) {
        return NULL;
    }
    size_t n = fread(name, 1, sizeof name - 1, f);
    int rc = pclose(f);
    if (rc != 0 || n == 0) {
        return NULL;
    }
    while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r')) {
        n--;
    }
    name[n] = '\0';
    return name[0] != '\0' ? name : NULL;
}

/* One scoped sway configuration command (S-6): used to turn
 * focus-follows-mouse off for the activation section so focus flips
 * stay attributable to activation itself, restoring the default
 * afterwards. Returns the swaymsg exit status (best-effort: callers
 * treat failure as "the rig's default stands"). */
static int swaymsg_config(const char *command) {
    char cmd[256];
    (void)snprintf(cmd, sizeof cmd,
                   "swaymsg -q '%s' >/dev/null 2>&1", command);
    return system(cmd);
}

/* Enter/leave counters for the input-region legs (S-4). */
struct region_counts {
    int *enters;
    int *leaves;
};
static void region_count_cb(fdk_window *w, const fdk_event_data *event,
                            void *user) {
    (void)w;
    struct region_counts *c = user;
    if (event->type == FDK_EVENT_POINTER_ENTER) {
        (*c->enters)++;
    } else if (event->type == FDK_EVENT_POINTER_LEAVE) {
        (*c->leaves)++;
    }
}

/* Real-app loop shape: pump + paint each iteration. Painting matters
 * under sway's headless output — the surface stays live in the
 * compositor's tree while frames flow, and the interactive section
 * needs the window mapped to receive the injected pointer events
 * (an idle-pumping window drops out of the tree on this backend;
 * every FDK example paints in its loop, so this helper IS the
 * documented usage pattern). */
static void pump_and_paint(fdk_context *ctx, fdk_window *win, int ms) {
    int steps = ms / 50;
    if (steps < 1) {
        steps = 1;
    }
    for (int i = 0; i < steps; i++) {
        (void)fdk_pump_events(ctx, 50);
        fdk_window_paint(win);
    }
}

/* S-10: the date picker's changed capture. */
static int g_wl_dp_hits = 0;
static fdk_date g_wl_dp_got;

static void wayland_dp_changed_cb(fdk_widget *picker, const fdk_date *d,
                                  void *user) {
    (void)picker;
    (void)user;
    g_wl_dp_hits++;
    g_wl_dp_got = *d;
}

static void wayland_menu_item_cb(fdk_menu_item *item, void *user) {
    (void)item;
    (*(int *)user)++;
}

/* Monotonic-wall elapsed time for the churn-bound probe: measures
 * how long a resize-churn acquisition takes inside the synchronous
 * configure repaint (must be ONE poll slice, not the full release
 * wait). */
static long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The opener wiring: statics because the callback has no closure
 * state (stock Button signature). */
static fdk_menu *g_wayland_menu = NULL;
static fdk_widget *g_wayland_menu_anchor = NULL;

static void wayland_menu_button_cb(fdk_widget *button, void *user) {
    (void)button;
    (*(int *)user)++;
    if (g_wayland_menu != NULL && g_wayland_menu_anchor != NULL) {
        (void)fdk_menu_popup_at(g_wayland_menu, g_wayland_menu_anchor,
                                0, 30);
    }
}

static void window_callback(fdk_window *window, const fdk_event_data *event,
                            void *user) {
    (void)window;
    (void)user;
    switch (event->type) {
    case FDK_EVENT_WINDOW_STATE:
        g_state_events++;
        g_last_maximized = event->state.maximized;
        g_last_minimized = event->state.minimized;
        break;
    case FDK_EVENT_WINDOW_DECORATION:
        g_deco_events++;
        break;
    default:
        break;
    }
}

/* The themed band fill under the default theme (v1 control bg):
 * 0.16*255=40.8->40, 0.18*255=45.9->45, 0.26*255=66.3->66 with the
 * renderer's exact rounding — read the actual pixel and compare
 * against the SAME lookup the paint hook used, component by component
 * (0.16f, 0.18f, 0.26f). */
static bool pixel_is_band_fill(fdk_u32 px) {
    fdk_color c = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    fdk_u32 r = (fdk_u32)(c.r * 255.0f + 0.5f);
    fdk_u32 g = (fdk_u32)(c.g * 255.0f + 0.5f);
    fdk_u32 b = (fdk_u32)(c.b * 255.0f + 0.5f);
    return ((px >> 16) & 0xFFu) == r && ((px >> 8) & 0xFFu) == g &&
           (px & 0xFFu) == b;
}

/* ---- 1.2.0 DnD receive accounting (the drop-target section) ---- */
static struct {
    int enters, motions, leaves, drops;
    char uris[2][512];
    size_t uri_count;
    char text[256];
} wl_dnd_rx;

static void wl_dnd_rx_event(fdk_window *w, const fdk_event_data *ev,
                            void *user) {
    (void)w; (void)user;
    if (ev->type < FDK_EVENT_DRAG_ENTER || ev->type > FDK_EVENT_DRAG_DROP) {
        return;
    }
    switch (ev->type) {
    case FDK_EVENT_DRAG_ENTER: wl_dnd_rx.enters++; break;
    case FDK_EVENT_DRAG_MOTION: wl_dnd_rx.motions++; break;
    case FDK_EVENT_DRAG_LEAVE: wl_dnd_rx.leaves++; break;
    case FDK_EVENT_DRAG_DROP:
        wl_dnd_rx.drops++;
        for (size_t i = 0; i < ev->drag.uri_count && i < 2; i++) {
            snprintf(wl_dnd_rx.uris[i], sizeof(wl_dnd_rx.uris[i]), "%s",
                     ev->drag.uris[i]);
        }
        wl_dnd_rx.uri_count = ev->drag.uri_count;
        if (ev->drag.text != NULL) {
            snprintf(wl_dnd_rx.text, sizeof(wl_dnd_rx.text), "%s",
                     ev->drag.text);
        }
        break;
    default: break;
    }
}

/* ---- 1.3.3 accelerator section callbacks ---- */

static void wl_accel_app_shortcut(fdk_window *window, void *user) {
    (void)window;
    (*(int *)user)++;
}

static void wl_accel_menu_cb(fdk_menu_item *item, void *user) {
    (void)item;
    (*(int *)user)++;
}

static void wl_accel_window_cb(fdk_window *window,
                               const fdk_event_data *event, void *user) {
    (void)window;
    if (event->type == FDK_EVENT_KEY_DOWN) {
        (*(int *)user)++;
    }
}

/* ---- IME end-to-end section (1.1.0, M-1) ----------------------------------
 *
 * THE RIG: this test plays BOTH sides of the IME protocol against
 * the real compositor relay (sway 1.10 / wlroots 0.18):
 *
 *   APP side  — the FDK library itself: binds text-input-v3, gets
 *               enter on keyboard focus, enables, receives
 *               preedit/commit through the v3 state machine
 *               (src/platform/wayland/wayland_ime.c).
 *   IM side   — a SECOND wl_display connection here, binding
 *               zwp_input_method_manager_v2 and driving the app's
 *               text-input through sway's input-method relay (the
 *               fcitx5 role: activate, surrounding_text, then
 *               set_preedit_string / commit_string + commit).
 *
 * Two legs:
 *   A (pipeline) — a window with NO text widget: PREEDIT/COMMIT
 *       fall through the unconsumed routing to the window callback
 *       (custom text surfaces implement composition there — the
 *       documented contract).
 *   B (entry) — a focused Entry consumes COMMIT (splices into the
 *       buffer) and the IM side's surrounding_text event proves the
 *       FULL loop: entry text -> fdk__widget_ime_context ->
 *       ime_update_surrounding -> set_surrounding_text -> sway
 *       relay -> the IM's surrounding_text event.
 *
 * Skip discipline (the rig's standing shape): no
 * zwp_input_method_manager_v2 (kiosk-shell weston) skips the whole
 * section; no keyboard focus this run (seat without capability —
 * the injector could not start) skips the driven legs but still
 * proves the library-side connection stayed healthy. */

typedef struct {
    struct wl_display *disp;
    struct wl_registry *reg;
    struct wl_seat *seat;
    struct zwp_input_method_manager_v2 *imm;
    struct zwp_input_method_v2 *im;
    int activations;
    int deactivations;
    int done_count; /* commit()'s serial discipline: the number of
                    * done events the compositor has issued */
    char surround[512];
    uint32_t surround_cursor;
    int surround_count;
} ime_client;

static ime_client g_ime;

/* Leg A's capture: IME events that fall through to the window
 * callback (nothing focused consumes them). */
static struct {
    int preedits;
    int commits;
    char last_preedit[256];
    char last_commit[256];
    int last_preedit_cursor;
} g_ime_cb;

static void ime_win_cb(fdk_window *w, const fdk_event_data *ev,
                       void *user) {
    (void)w;
    (void)user;
    if (ev->type == FDK_EVENT_IME_PREEDIT) {
        g_ime_cb.preedits++;
        snprintf(g_ime_cb.last_preedit, sizeof g_ime_cb.last_preedit,
                 "%s", ev->ime.text);
        g_ime_cb.last_preedit_cursor = ev->ime.cursor;
    } else if (ev->type == FDK_EVENT_IME_COMMIT) {
        g_ime_cb.commits++;
        snprintf(g_ime_cb.last_commit, sizeof g_ime_cb.last_commit,
                 "%s", ev->ime.text);
    }
}

/* Leg B's capture: the entry's on_changed (the commit's mutation
 * signal) + the entry's own buffer assertions. */
static int g_ime_entry_changes = 0;

static void ime_entry_changed(fdk_widget *entry, void *user) {
    (void)entry;
    (void)user;
    g_ime_entry_changes++;
}

static void ime_reg_global(void *data, struct wl_registry *reg,
                           uint32_t name, const char *iface,
                           uint32_t version) {
    ime_client *c = data;
    uint32_t v = version < 1 ? version : 1;
    if (strcmp(iface, "zwp_input_method_manager_v2") == 0) {
        c->imm = wl_registry_bind(reg, name,
                                  &zwp_input_method_manager_v2_interface, v);
    } else if (strcmp(iface, "wl_seat") == 0) {
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    }
}

static void ime_reg_global_remove(void *data, struct wl_registry *reg,
                                  uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener ime_reg_listener = {
    .global = ime_reg_global,
    .global_remove = ime_reg_global_remove,
};

static void ime_evt_activate(void *data,
                             struct zwp_input_method_v2 *im) {
    (void)im;
    ime_client *c = data;
    c->activations++;
}

static void ime_evt_deactivate(void *data,
                               struct zwp_input_method_v2 *im) {
    (void)im;
    ime_client *c = data;
    c->deactivations++;
}

static void ime_evt_surrounding(void *data,
                                struct zwp_input_method_v2 *im,
                                const char *text, uint32_t cursor,
                                uint32_t anchor) {
    (void)im;
    (void)anchor;
    ime_client *c = data;
    snprintf(c->surround, sizeof c->surround, "%s",
             text != NULL ? text : "");
    c->surround_cursor = cursor;
    c->surround_count++;
}

static void ime_evt_done(void *data, struct zwp_input_method_v2 *im) {
    (void)im;
    ime_client *c = data;
    c->done_count++;
}

/* libwayland ABORTS on a NULL listener member when its event
 * arrives — sway sends text_change_cause and content_type around
 * every activation, so every member of the listener struct needs a
 * body even where the rig only counts. */
static void ime_evt_change_cause(void *data,
                                 struct zwp_input_method_v2 *im,
                                 uint32_t cause) {
    (void)data;
    (void)im;
    (void)cause;
}

static void ime_evt_content_type(void *data,
                                 struct zwp_input_method_v2 *im,
                                 uint32_t hint, uint32_t purpose) {
    (void)data;
    (void)im;
    (void)hint;
    (void)purpose;
}

static void ime_evt_unavailable(void *data,
                                struct zwp_input_method_v2 *im) {
    (void)im;
    ime_client *c = data;
    c->deactivations++; /* the IM object went inert — treat as a
                         * deactivation for the rig's sequencing */
}

static const struct zwp_input_method_v2_listener ime_im_listener = {
    .activate = ime_evt_activate,
    .deactivate = ime_evt_deactivate,
    .surrounding_text = ime_evt_surrounding,
    .text_change_cause = ime_evt_change_cause,
    .content_type = ime_evt_content_type,
    .done = ime_evt_done,
    .unavailable = ime_evt_unavailable,
};

/* Connect the IM side. Returns false when THIS compositor offers no
 * input-method-v2 (the honest skip — weston kiosk-shell). */
static bool ime_client_start(void) {
    memset(&g_ime, 0, sizeof g_ime);
    g_ime.disp = wl_display_connect(NULL);
    if (g_ime.disp == NULL) {
        return false;
    }
    struct wl_registry *reg = wl_display_get_registry(g_ime.disp);
    wl_registry_add_listener(reg, &ime_reg_listener, &g_ime);
    wl_display_roundtrip(g_ime.disp);
    if (g_ime.imm == NULL || g_ime.seat == NULL) {
        wl_registry_destroy(reg);
        if (g_ime.seat != NULL) {
            wl_seat_destroy(g_ime.seat);
            g_ime.seat = NULL;
        }
        wl_display_disconnect(g_ime.disp);
        g_ime.disp = NULL;
        return false;
    }
    g_ime.reg = reg;
    g_ime.im = zwp_input_method_manager_v2_get_input_method(g_ime.imm,
                                                             g_ime.seat);
    if (g_ime.im == NULL) {
        wl_display_disconnect(g_ime.disp);
        g_ime.disp = NULL;
        return false;
    }
    zwp_input_method_v2_add_listener(g_ime.im, &ime_im_listener, &g_ime);
    wl_display_roundtrip(g_ime.disp); /* listener armed + any queued
                                       * events drained */
    return true;
}

/* Non-blocking dispatch of the IM connection (data-ready only —
 * poll() provides the timeout; wl_display_dispatch never blocks
 * once poll says readable). */
static int ime_client_poll(int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = wl_display_get_fd(g_ime.disp);
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN) != 0) {
        return wl_display_dispatch(g_ime.disp);
    }
    return 0;
}

static void ime_client_stop(void) {
    if (g_ime.im != NULL) {
        zwp_input_method_v2_destroy(g_ime.im);
        g_ime.im = NULL;
    }
    if (g_ime.imm != NULL) {
        zwp_input_method_manager_v2_destroy(g_ime.imm);
        g_ime.imm = NULL;
    }
    if (g_ime.seat != NULL) {
        wl_seat_destroy(g_ime.seat);
        g_ime.seat = NULL;
    }
    if (g_ime.reg != NULL) {
        wl_registry_destroy(g_ime.reg);
        g_ime.reg = NULL;
    }
    if (g_ime.disp != NULL) {
        wl_display_disconnect(g_ime.disp);
        g_ime.disp = NULL;
    }
}

/* One IME batch: preedit revision (or NULL to skip), optional
 * commit text (or NULL), then commit(serial) to apply. */
static void ime_send(const char *preedit, const char *commit_text) {
    if (preedit != NULL) {
        zwp_input_method_v2_set_preedit_string(
            g_ime.im, preedit, 0, (int32_t)strlen(preedit));
    }
    if (commit_text != NULL) {
        zwp_input_method_v2_commit_string(g_ime.im, commit_text);
    }
    zwp_input_method_v2_commit(g_ime.im, (uint32_t)g_ime.done_count);
    wl_display_flush(g_ime.disp);
}

/* The S-6 launcher-child leg (re-exec form, the --fc-fontconfig-leg
 * precedent): the parent mints a token with the injector's button
 * HELD on its own surface, re-executes this binary with the token as
 * XDG_ACTIVATION_TOKEN, and this branch spends it — the exact
 * two-process contract the API documents. Exit 0 = this process saw
 * ITSELF focused through sway's own tree after the activation. */
static int activation_child_leg(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_WAYLAND,
                              .app_id = "org.fdk.test" };
    fdk_result r = fdk_init(&ctx, &opts);
    if (!fdk_ok(r)) {
        printf("[child] no compositor\n");
        return 2;
    }
    assert(getenv("XDG_ACTIVATION_TOKEN") == NULL);
    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK activation child",
                                 .width = 200, .height = 150 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, (fdk_color){0.5f, 0.2f, 0.8f, 1.0f});
    fdk_window_show(win);
    fdk_window_paint(win);
    for (int i = 0; i < 20; i++) {
        (void)fdk_pump_events(ctx, 50);
    }
    /* Spend the voucher: the FIRST activate rides the parent's
     * token (the button is still held on the parent's surface — the
     * window the compositor will vouch for). */
    assert(fdk_ok(fdk_window_activate(win)));
    for (int i = 0; i < 30; i++) {
        (void)fdk_pump_events(ctx, 50);
    }
    const char *focused = sway_focused_window_name();
    printf("[child] activation focus verdict: %s\n",
           focused != NULL ? focused : "(unavailable)");
    int ok = focused != NULL &&
             strcmp(focused, "FDK activation child") == 0;
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    return ok ? 0 : 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("FDK_WL_ACT_CHILD") != NULL) {
        return activation_child_leg();
    }
    /* The S-6 launch voucher: a BOGUS token set before fdk_init — the
     * connection captures it (and unsets it — asserted in the
     * activation section below), and the first activate() spends it.
     * sway must REFUSE it: focus-stealing prevention is the point of
     * xdg-activation, and the refusal itself is the assertion. */
    setenv("XDG_ACTIVATION_TOKEN", "rig-bogus-wayland-voucher", 1);
    fdk_context *ctx = NULL;
    /* app_id matters: the sway rig floats windows with this prefix,
     * so client-side resizes (the reflow test) actually take effect
     * instead of being overridden by the tiling layout. */
    fdk_init_options opts = { .backend = FDK_PLATFORM_WAYLAND,
                              .app_id = "org.fdk.test" };
    fdk_result r = fdk_init(&ctx, &opts);
    if (!fdk_ok(r)) {
        printf("[skip] no Wayland compositor reachable (fdk_init: %d)\n", r);
        return 0;
    }

    /* ---- The deferred-first-frame regression (found live in 1.1.0:
     * every example painted before the first configure and never
     * mapped) ----
     *
     * Exact application ordering: create, show, PAINT, only then
     * enter the pump loop. The paint happens before the first
     * xdg configure has been read, so the backend must defer the
     * commit — and must land it by itself the moment the configure
     * is acked. Before the fix this test hangs below: the surface
     * layer had consumed the damage, every later present was a
     * no-op, and rendered_ever never became true. */
    {
        fdk_window *dw = NULL;
        fdk_window_options dwopts = { .title = "FDK deferred frame",
                                      .width = 300, .height = 200 };
        assert(fdk_ok(fdk_window_create(ctx, &dwopts, &dw)));
        fdk_widget *droot = NULL;
        assert(fdk_ok(fdk_window_get_root(dw, &droot)));
        fdk_widget_set_background(droot,
                                  (fdk_color){0.2f, 0.4f, 0.9f, 1.0f});
        fdk_window_show(dw);
        fdk_window_paint(dw); /* BEFORE any pump — the deferred path */
        assert(fdk__window_ever_presented(dw) == 0);

        for (int i = 0; i < 40; i++) {
            (void)fdk_pump_events(ctx, 50);
            if (fdk__window_ever_presented(dw) != 0) {
                break; /* committed at the configure itself */
            }
        }
        if (fdk__window_ever_presented(dw) == 0) {
            /* Configure proposed a different size: the EXPOSE branch
             * re-drove the tree, and the application's NEXT paint
             * (still the documented loop shape) presents at the real
             * size. One more round must land it. */
            fdk_window_paint(dw);
            for (int i = 0; i < 8; i++) {
                (void)fdk_pump_events(ctx, 50);
                if (fdk__window_ever_presented(dw) != 0) {
                    break;
                }
            }
        }
        assert(fdk__window_ever_presented(dw) == 1);
        printf("[ok] deferred first frame: present-before-configure "
               "maps the window (no idle-wedge)\n");
        fdk_window_destroy(dw);
    }

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK wayland state test",
                                 .width = 320, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, window_callback, NULL);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL, (fdk_rect){0, 0, 10, 10},
                                    &content)));
    fdk_widget_set_background(content, (fdk_color){0.0f, 1.0f, 0.0f, 1.0f});
    fdk_window_set_content(win, content);

    fdk_window_show(win);
    /* First configure arrives here (kiosk-shell maximizes everything,
     * so the size may be the output size — size-agnostic assertions
     * only). */
    for (int i = 0; i < 40 && !fdk_window_get_decorated(win); i++) {
        (void)fdk_pump_events(ctx, 50);
    }
    fdk_size size = { 0, 0 };
    assert(fdk_ok(fdk_window_get_size(win, &size)));
    assert(size.width > 0 && size.height > 0);

    /* ---- Decorations: the band must come up on ANY compositor ----
     * (1.1.5) A compositor without zxdg_decoration_manager_v1 never
     * draws chrome itself — client-side is the xdg-shell default —
     * so set_decorated(true) must succeed everywhere: kiosk-shell
     * here, Muffin/Cinnamon Wayland on a real desktop, tiling WMs.
     * Before the fix this branch returned FDK_ERR_UNSUPPORTED and
     * the decorations demo exited before it ever mapped a window. */
    r = fdk_window_set_decorated(win, true);
    assert(fdk_ok(r));
    assert(fdk_window_get_decorated(win));
    (void)fdk_pump_events(ctx, 400);

    if (g_deco_events > 0 && !fdk_window_get_decorated(win)) {
        /* Compositor forced SERVER_SIDE and FDK tore its band down —
         * the no-double-decoration contract. */
        printf("[ok] compositor forced server-side decorations; FDK "
               "dropped its band (decoration event delivered)\n");
    } else {
        /* CLIENT_SIDE confirmed, or no decoration protocol at all
         * (the band just IS the chrome): the band is drawn INSIDE
         * our framebuffer — verify at the pixel level what we
         * render. */
        assert(fdk_ok(fdk_window_paint(win)));
        fdk_surface *surface = NULL;
        assert(fdk_ok(fdk_window_get_surface(win, &surface)));
        fdk_surface_info info;
        assert(fdk_ok(fdk_surface_get_info(surface, &info)));
        /* Band probes in PHYSICAL rows: the band is 28 LOGICAL
         * rows tall (title_bar_height metric), so the below-band
         * probe must clear it at any scale — 40 logical rows up,
         * converted through the live scale. */
        fdk_f32 p_scale = 1.0f;
        (void)fdk_window_get_scale(win, &p_scale);
        fdk_i32 below_row = (fdk_i32)(40.0f * p_scale);
        if (below_row >= info.height) {
            below_row = info.height - 1;
        }
        fdk_u32 mid = info.pixels[(fdk_i32)(info.stride * 2) +
                                  info.width / 2];
        fdk_u32 below = info.pixels[(size_t)info.stride *
                                        (size_t)below_row +
                                    (size_t)(info.width / 2)];
        assert(pixel_is_band_fill(mid));
        assert(!pixel_is_band_fill(below) ||
               info.height < (fdk_i32)(48.0f * p_scale));
        printf("[ok] client-side decorations active: themed band "
               "rendered in-frame (pixel-verified), content below\n");
    }

    /* ---- Maximize: request + configure-driven state event ---- */
    /* The compositor's world dictates what can be asserted — THREE
     * real worlds, all handled:
     *  (a) TILED (sway's default layout): windows report MAXIMIZED in
     *      the toplevel configure AT MAP TIME — before any request of
     *      ours (kiosk-shell weston maximizes everything too).
     *  (b) HONORED: a compositor that answers set_maximized with a
     *      configure carrying MAXIMIZED.
     *  (c) DECLINED: sway 1.10 + a FLOATING window (the rig floats
     *      app_id "org.fdk.test" so client resizes take effect) never
     *      acts on the request — the window keeps its floating
     *      geometry and states (verified against sway's own tree:
     *      floating_con, fullscreen_mode 0). xdg-shell lets the
     *      compositor decide; FDK must not fake a state the
     *      compositor never confirmed, and must not invent an event. */
    bool pre_max = fdk_window_is_maximized(win);
    int before = g_state_events;
    assert(fdk_ok(fdk_window_maximize(win)));
    for (int i = 0; i < 40; i++) {
        (void)fdk_pump_events(ctx, 50);
        if (fdk_window_is_maximized(win)) {
            break;
        }
    }
    if (pre_max) {
        /* (a) Already maximized at map: the state event fired THEN
         * (the map-time configure proved the whole chain — states
         * parsing, change detection, event dispatch); the later
         * request is a no-op that correctly dispatches nothing new.
         * The REQUEST itself is protocol-verified by the rig's
         * WAYLAND_DEBUG counts. */
        assert(g_state_events >= 1);
        assert(fdk_window_is_maximized(win));
        printf("[ok] maximize request sent; compositor tiled the window "
               "(maximized at map, state event then delivered)\n");
    } else if (fdk_window_is_maximized(win)) {
        /* (b) Fresh transition: the configure must have carried
         * MAXIMIZED, and FDK must have delivered the event. */
        assert(g_state_events > before);
        assert(g_last_maximized == 1);
        printf("[ok] maximize: xdg configure reported MAXIMIZED state, "
               "FDK_EVENT_WINDOW_STATE delivered\n");
    } else {
        /* (c) Declined: the request went out (the rig's WAYLAND_DEBUG
         * count proves the wire), the compositor never confirmed, so
         * FDK reports the honest state — still not maximized, and NO
         * spurious state event may fire for a no-change configure. */
        assert(g_state_events == before);
        printf("[ok] maximize request sent; compositor declined it "
               "(floating window keeps its geometry; FDK reports the "
               "compositor's truth, no invented state/event)\n");
    }

    /* ---- Unmaximize ---- */
    before = g_state_events;
    assert(fdk_ok(fdk_window_unmaximize(win)));
    for (int i = 0; i < 40; i++) {
        (void)fdk_pump_events(ctx, 50);
        if (!fdk_window_is_maximized(win)) {
            break;
        }
    }
    printf("[ok] unmaximize request sent (post-state: maximized=%d)\n",
           fdk_window_is_maximized(win) ? 1 : 0);

    /* ---- Minimize: fire-and-forget request, optimistic flag ---- */
    before = g_state_events;
    assert(fdk_ok(fdk_window_minimize(win)));
    (void)fdk_pump_events(ctx, 300);
    assert(fdk_window_is_minimized(win));
    assert(g_state_events > before);
    /* Restore is honestly unsupported on Wayland (no protocol request). */
    assert(fdk_window_restore(win) == FDK_ERR_UNSUPPORTED);
    printf("[ok] minimize: request sent, optimistic state flagged, "
           "restore honestly UNSUPPORTED\n");

    /* ---- Fullscreen (1.4.10): request + configure-driven state ----
     *
     * The same three-world honesty as maximize: (a) compositor
     * honors it -> configure carries FULLSCREEN -> is_fullscreen
     * flips + state event; (b) declines (floating window under sway)
     * -> no fake flag, no invented event. Both are protocol-correct;
     * the rig's WAYLAND_DEBUG count proves the request went out. */
    {
        int before_fs = g_state_events;
        assert(fdk_ok(fdk_window_fullscreen(win)));
        for (int i = 0; i < 40; i++) {
            (void)fdk_pump_events(ctx, 50);
            if (fdk_window_is_fullscreen(win)) {
                break;
            }
        }
        if (fdk_window_is_fullscreen(win)) {
            assert(g_state_events > before_fs);
            printf("[ok] fullscreen: xdg configure reported FULLSCREEN "
                   "state, FDK_EVENT_WINDOW_STATE delivered\n");
        } else {
            assert(g_state_events == before_fs);
            printf("[ok] fullscreen request sent; compositor declined "
                   "(FDK reports the truth, no invented state)\n");
        }
        /* Leave the world as we found it. */
        assert(fdk_ok(fdk_window_unfullscreen(win)));
        for (int i = 0; i < 40; i++) {
            (void)fdk_pump_events(ctx, 50);
            if (!fdk_window_is_fullscreen(win)) {
                break;
            }
        }
        assert(!fdk_window_is_fullscreen(win));
    }

    /* ---- Layout reflow on resize (Phase 5 completion item) ----
     *
     * The Phase 5 roadmap entry recorded a Wayland-side reflow test
     * as blocked on the compositor toolchain; sway headless closes
     * it. A vertical box with a colored header/body/footer is the
     * window content; a resize (client request -> compositor
     * configure) must re-arrange the tree with zero application
     * code, verifiable in the framebuffer: the body panel STRETCHES
     * (expand) and the footer MOVES to the new bottom. */
    {
        fdk_window *rwin = NULL;
        fdk_window_options ropts = { .title = "FDK wayland reflow test",
                                     .width = 300, .height = 200 };
        assert(fdk_ok(fdk_window_create(ctx, &ropts, &rwin)));
        fdk_window_show(rwin);
        for (int i = 0; i < 12; i++) {
            (void)fdk_pump_events(ctx, 30); /* first configure */
        }

        fdk_widget *rroot = NULL;
        assert(fdk_ok(fdk_window_get_root(rwin, &rroot)));
        fdk_widget *vbox = NULL;
        assert(fdk_ok(fdk_box_create(rroot, FDK_VERTICAL, &vbox)));
        fdk_widget *header = NULL;
        assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                        (fdk_rect){0, 0, 10, 30}, &header)));
        fdk_widget_set_background(header,
                                  (fdk_color){0.9f, 0.2f, 0.2f, 1.0f});
        fdk_widget_set_expand(header, false, false);
        fdk_widget_set_natural_size(header, 10, 30);
        fdk_widget *body = NULL;
        assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                        (fdk_rect){0, 0, 10, 10}, &body)));
        fdk_widget_set_background(body,
                                  (fdk_color){0.2f, 0.9f, 0.2f, 1.0f});
        fdk_widget_set_expand(body, false, true); /* fills leftover height */
        fdk_widget *footer = NULL;
        assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                        (fdk_rect){0, 0, 10, 20}, &footer)));
        fdk_widget_set_background(footer,
                                  (fdk_color){0.2f, 0.2f, 0.9f, 1.0f});
        fdk_widget_set_expand(footer, false, false);
        fdk_window_set_content(rwin, vbox);

        /* Paint at the initial size and sample the vertical bands. */
        assert(fdk_ok(fdk_window_paint(rwin)));
        fdk_surface *rs = NULL;
        assert(fdk_ok(fdk_window_get_surface(rwin, &rs)));
        fdk_surface_info rinfo;
        assert(fdk_ok(fdk_surface_get_info(rs, &rinfo)));
        fdk_i32 h0 = rinfo.height;

        /* Resize + pump; on Wayland the recorded size reaches the
         * screen through the app's NEXT COMMIT (a floating toplevel
         * follows the last committed buffer), so the loop paints as
         * it pumps — each paint commits the new size and the tree
         * re-arranges itself with zero application code. */
        fdk_window_resize(rwin, rinfo.width / 2, h0 + 120);
        for (int i = 0; i < 30; i++) {
            (void)fdk_window_paint(rwin);
            (void)fdk_pump_events(ctx, 40);
            fdk_size cur = { 0, 0 };
            (void)fdk_window_get_size(rwin, &cur);
            if (cur.height >= h0 + 100) {
                break;
            }
        }
        assert(fdk_ok(fdk_window_paint(rwin)));
        assert(fdk_ok(fdk_surface_get_info(rs, &rinfo)));

        if (rinfo.height <= h0 + 60) {
            /* COMPOSITOR-OWNED geometry: either the compositor tiles
             * windows (a client resize is overridden by the layout,
             * so the growth never lands) or it configured the window
             * fullscreen/maximized — under which FDK (1.1.5) REFUSES
             * the resize outright, because committing a non-
             * configured size under those states is a protocol error
             * on strict compositors (weston kills the connection).
             * The full reflow verification runs in the rig's FLOATING
             * phase; this branch records honestly that this run's
             * geometry was compositor-owned. */
            fdk_window_destroy(rwin);
            printf("[ok] Wayland layout reflow: growth skipped "
                   "(compositor-owned geometry — tiled or fullscreen; "
                   "the floating rig phase verifies the reflow)\n");
        } else {
        /* The GREEN body (expanding) must reach deep into the grown
         * window; the BLUE footer sits at the very bottom. Sampling
         * the physical framebuffer, scale-agnostic: relative bands. */
        fdk_i32 probe = rinfo.height - rinfo.height / 8;
        fdk_u32 bottom_px =
            rinfo.pixels[(size_t)probe * (size_t)rinfo.stride +
                         (size_t)(rinfo.width / 2)];
        int br = (int)((bottom_px >> 16) & 0xFFu);
        int bg = (int)((bottom_px >> 8) & 0xFFu);
        int bb = (int)(bottom_px & 0xFFu);
        /* The footer is the last 20 LOGICAL rows; the probe at 7/8
         * height should be inside the body (green) or the footer
         * (blue) — but NOT the red header or the black background. */
        int inked = (br > 80 && br > bg + 40) || (bg > 80 && bg > br + 40) ||
                    (bb > 80 && bb > br + 40);
        assert(inked);

        fdk_window_destroy(rwin);
        printf("[ok] Wayland layout reflow: resize configure re-arranges "
               "the content tree (expanding body, moved footer)\n");
        }
    }

    /* ---- HiDPI scale (Phase 3 completion) ----
     *
     * Whatever scale the compositor prefers (sway's config decides:
     * 1x by default, 2x in the rig's scaled run), the invariants are
     * the same: fdk_window_get_scale reports it, the surface's
     * framebuffer is PHYSICAL (logical x scale, ceil), and the widget
     * tree still covers the window proportionally after a paint (the
     * scale > 1 path composites through the logical intermediate). */
    {
        fdk_f32 scale = 1.0f;
        assert(fdk_ok(fdk_window_get_scale(win, &scale)));
        assert(scale >= 0.99f);

        /* Unmaximize first so the window has a compositor-decided
         * (not screen-filling) logical size — cleaner math. */
        (void)fdk_window_unmaximize(win);
        for (int i = 0; i < 40; i++) {
            (void)fdk_pump_events(ctx, 50);
            if (!fdk_window_is_maximized(win)) {
                break;
            }
        }
        /* Let enter/leave + preferred_scale settle. */
        for (int i = 0; i < 8; i++) {
            (void)fdk_pump_events(ctx, 50);
        }

        fdk_size logical = { 0, 0 };
        assert(fdk_ok(fdk_window_get_size(win, &logical)));
        assert(logical.width > 0 && logical.height > 0);

        fdk_f32 scale_now = 1.0f;
        assert(fdk_ok(fdk_window_get_scale(win, &scale_now)));

        fdk_surface *hs = NULL;
        assert(fdk_ok(fdk_window_get_surface(win, &hs)));
        fdk_surface_info hinfo;
        assert(fdk_ok(fdk_surface_get_info(hs, &hinfo)));

        /* Physical = ceil(logical * scale), within one pixel of the
         * float prediction (rounding at the 120th quantum). */
        fdk_f32 want_w = (fdk_f32)logical.width * scale_now;
        fdk_f32 want_h = (fdk_f32)logical.height * scale_now;
        assert((fdk_f32)hinfo.width >= want_w - 1.0f);
        assert((fdk_f32)hinfo.width <= want_w + 1.0f);
        assert((fdk_f32)hinfo.height >= want_h - 1.0f);
        assert((fdk_f32)hinfo.height <= want_h + 1.0f);

        /* Paint through the (possibly scaled) path and verify the
         * content widget's green still fills the area below any
         * decoration band — proportionally correct at any scale. */
        assert(fdk_ok(fdk_window_paint(win)));
        fdk_surface_info hinfo2;
        assert(fdk_ok(fdk_surface_get_info(hs, &hinfo2)));
        fdk_i32 probe_y = hinfo2.height - hinfo2.height / 4;
        fdk_u32 px = hinfo2.pixels[(size_t)probe_y * (size_t)hinfo2.stride +
                                   (size_t)(hinfo2.width / 2)];
        int green = ((px >> 8) & 0xFFu) > 200 &&
                    ((px >> 16) & 0xFFu) < 60 && (px & 0xFFu) < 60;
        assert(green); /* content green, not background black */

        printf("[ok] HiDPI: scale=%.3f logical %dx%d physical %dx%d "
               "(=logical x scale), content paints proportionally\n",
               (double)scale_now, logical.width, logical.height,
               hinfo2.width, hinfo2.height);
    }

    /* ---- Clipboard (Phase 9) ----
     *
     * What the protocol honestly allows us to verify against a
     * headless compositor:
     *  - SET + GET round trip on our own selection (compositors
     *    never send a client its own selection back, so FDK serves
     *    from its tracked copy — this is the real production path
     *    for "did my own copy survive").
     *  - GET with no text selection -> NULL (the data-device
     *    listener machinery correctly reports emptiness).
     *  Cross-client transfers cannot be driven here: set_selection
     *    must cite an input serial, and the headless seat generates
     *    no input events (documented in fdk_clipboard.h). That path
     *    is exercised structurally (protocol objects + requests in
     *    the WAYLAND_DEBUG trace of the rig) and by the X11 suite's
     *    cross-process equivalents.
     *
     * Seat-less compositors honestly skip: weston kiosk-shell
     * advertises wl_data_device_manager but has NO wl_seat, so no
     * data device can exist (found live when the 1.1.5 manager-less
     * rig started running this suite under kiosk-shell; the sway rig
     * has a seat and runs the full assertions). */
    {
        fdk_result cr = fdk_clipboard_set_text(ctx, "wayland phase 9");
        if (cr == FDK_ERR_UNSUPPORTED) {
            printf("[skip] clipboard: no wl_data_device here "
                   "(seat-less compositor — kiosk-shell weston)\n");
        } else {
        assert(fdk_ok(cr));
        char *text = fdk_clipboard_get_text(ctx);
        assert(text != NULL && strcmp(text, "wayland phase 9") == 0);
        printf("[ok] clipboard: set + get round trip on own selection\n");

        assert(fdk_ok(fdk_clipboard_set_text(ctx, "replaced")));
        char *text2 = fdk_clipboard_get_text(ctx);
        assert(text2 != NULL && strcmp(text2, "replaced") == 0);
        printf("[ok] clipboard: replace semantics\n");
        /* text/text2 leak is intentional-free below via explicit free;
         * there is no public fdk_free, and the docs bless free(). */
        free(text);
        free(text2);

        /* Empty text reads as NULL. */
        assert(fdk_ok(fdk_clipboard_set_text(ctx, "")));
        assert(fdk_clipboard_get_text(ctx) == NULL);
        printf("[ok] clipboard: empty own-selection reads as NULL\n");
        }
    }

    /* ---- PRIMARY selection (1.3.4) ----
     *
     * The classic Unix "current selection" buffer over
     * wp_primary_selection_unstable_v1: own-source round trip +
     * replace + empty-as-NULL, and INDEPENDENCE from the CLIPBOARD.
     * Compositors without the protocol (weston) honestly report
     * FDK_ERR_UNSUPPORTED — that's the skip below, not a failure.
     * Cross-client primary transfers ride the interop rig
     * (run_wayland_clip_interop.sh's primary direction): same
     * serial discipline as the clipboard, same focus push. */
    {
        fdk_result pr = fdk_clipboard_set_primary_text(ctx, "wayland primary");
        if (pr == FDK_ERR_UNSUPPORTED) {
            printf("[skip] primary: compositor without the "
                   "primary-selection protocol\n");
        } else {
            assert(fdk_ok(pr));
            char *text = fdk_clipboard_get_primary_text(ctx);
            assert(text != NULL && strcmp(text, "wayland primary") == 0);
            printf("[ok] primary: set + get round trip on own selection\n");

            assert(fdk_ok(fdk_clipboard_set_primary_text(ctx, "re-owned")));
            char *text2 = fdk_clipboard_get_primary_text(ctx);
            assert(text2 != NULL && strcmp(text2, "re-owned") == 0);
            free(text);
            free(text2);
            printf("[ok] primary: replace semantics\n");

            /* Independence: the two selections never cross. */
            assert(fdk_ok(fdk_clipboard_set_text(ctx, "the clipboard")));
            assert(fdk_ok(fdk_clipboard_set_primary_text(ctx, "the primary")));
            char *c = fdk_clipboard_get_text(ctx);
            char *p = fdk_clipboard_get_primary_text(ctx);
            assert(c != NULL && strcmp(c, "the clipboard") == 0);
            assert(p != NULL && strcmp(p, "the primary") == 0);
            free(c);
            free(p);
            printf("[ok] primary: CLIPBOARD and PRIMARY stay independent\n");

            assert(fdk_ok(fdk_clipboard_set_primary_text(ctx, "")));
            assert(fdk_clipboard_get_primary_text(ctx) == NULL);
            printf("[ok] primary: empty own-selection reads as NULL\n");
        }
    }

    /* ---- Key repeat (1.3.5) ----
     *
     * Wayland repeat is CLIENT-driven off wl_keyboard::repeat_info —
     * this is the machinery the X11 backend gets from the server for
     * free. sway headless cannot deliver wl_keyboard.key at all (the
     * seat never adopts a virtual keyboard's keymap — protocol-traced
     * in 1.3.3), so the suite drives the REAL listener bodies through
     * the ops test seams: press arms (after the delay), the pump's
     * next_wakeup cap keeps the loop waking at period speed,
     * dispatch_pending fires the repeats, the release disarms, and
     * rate 0 is the protocol's disabled state. The timings assert
     * generously (a shared headless box jitters) but tightly enough
     * that a missing wakeup cap (repeats only firing on unrelated
     * events) or a missing delay (immediate burst) both fail. */
    {
        if (ctx->ops->test_key == NULL) {
            printf("[skip] key repeat: library built without the "
                   "Wayland test seams\n");
        } else {
            fdk_window *rw = NULL;
            fdk_window_options rwopts = { .title = "FDK key repeat",
                                          .width = 300, .height = 150 };
            assert(fdk_ok(fdk_window_create(ctx, &rwopts, &rw)));
            fdk__window_set_auto_paint(rw, true);
            fdk_window_set_event_callback(rw, rep_window_cb, NULL);
            fdk_window_show(rw);
            for (int i = 0; i < 10; i++) {
                (void)fdk_pump_events(ctx, 50);
            }

            /* Focus probe FIRST: sway headless DOES auto-focus a
             * freshly mapped floating window when the seat has
             * keyboard capability (the probe rig proved the enter
             * arrives ~1s after map) — but only when the seat has
             * input devices at all. The wlr virtual devices come from
             * whatever injector is alive (the interop rig keeps one
             * for its whole run; a bare run has none and the seat
             * reports capabilities(0)). Starting ANOTHER injector
             * here is the fallback, not the first move: a second
             * virtual keyboard on the seat churns the keyboard
             * device (sway re-evaluates focus), which can cost the
             * focus the window already had. */
            /* sway headless's virtual keyboard grants the seat
             * KEYBOARD capability but serves clients an EMPTY
             * keymap (size 0), leaving xkb_state NULL — install the
             * standard default (what a compositor with a real
             * keyboard sends) through the seam first. */
            if (!ctx->ops->test_default_keymap(ctx->conn)) {
                printf("[skip] key repeat: no keymap installable "
                       "(no xkb context?)\n");
                fdk_window_destroy(rw);
            } else {
            int delivered = 0;
            /* sway/pixman headless takes ~1.5-2 s from map to
             * wl_keyboard.enter (the same slow frame pacing the
             * tooltip rig documented) — the probe must outlast it. */
            for (int i = 0; i < 35 && delivered == 0; i++) {
                (void)fdk_pump_events(ctx, 100);
                delivered = ctx->ops->test_key(ctx->conn, 30, 1);
                if (delivered) {
                    ctx->ops->test_key(ctx->conn, 30, 0); /* release the
                                                            probe press */
                }
            }
            if (delivered == 0 && wayland_injector_start()) {
                /* The tap fallback (same discipline as the menu
                 * section: a real click with a real serial). The
                 * window floats at (100,60) per the rig configs;
                 * 300x150 -> center (250,135). */
                wayland_inject("move 250 135");
                pump_and_paint(ctx, rw, 400);
                wayland_inject("tap 1");
                pump_and_paint(ctx, rw, 1200);
                delivered = ctx->ops->test_key(ctx->conn, 30, 1);
                if (delivered == 0) {
                    ctx->ops->test_key(ctx->conn, 30, 0);
                }
            }

            g_rep_downs = 0;
            g_rep_repeats = 0;
            g_rep_first_ms = 0;

            /* 50 Hz, 200 ms delay — fast enough to be decisive in
             * a second, slow enough to be distinguishable from a
             * busy-spin. */
            ctx->ops->test_repeat_info(ctx->conn, 50, 200);
            g_rep_press_ms = rep_now_ms();
            if (delivered) {
                /* the press that starts the held key */
                delivered = ctx->ops->test_key(ctx->conn, 30, 1);
            }
            if (delivered == 0) {
                printf("[skip] key repeat: no keyboard focus/keymap "
                       "on this compositor (seat-less kiosk-shell, or "
                       "no injector to tap-focus under headless "
                       "sway?)\n");
                ctx->ops->test_key(ctx->conn, 30, 0);
            } else {
                assert(g_rep_downs == 1);
                assert(g_rep_repeats == 0); /* the delay has not passed */

                /* Pump ~2 s in pump-sized chunks: repeats must arrive
                 * WITHOUT any compositor event to wake the loop (the
                 * next_wakeup cap is exactly what is under test).
                 * 40x50 ms outlasts the 200 ms delay with a wide
                 * margin — each pump call fires its due batch and
                 * returns, so the count is pump-granularity-proof
                 * (the catch-up design preserves it). */
                for (int i = 0; i < 40; i++) {
                    (void)fdk_pump_events(ctx, 50);
                }
                assert(g_rep_repeats >= 20);
                assert(g_rep_repeats <= 70);
                long long first_after =
                    g_rep_first_ms - g_rep_press_ms;
                assert(first_after >= 150);
                assert(first_after <= 600);
                printf("[ok] key repeat: 50 Hz after a 200 ms delay "
                       "(%d repeats, first at %lld ms)\n",
                       g_rep_repeats, first_after);

                /* Release disarms. */
                ctx->ops->test_key(ctx->conn, 30, 0);
                int held = g_rep_repeats;
                for (int i = 0; i < 5; i++) {
                    (void)fdk_pump_events(ctx, 60);
                }
                assert(g_rep_repeats == held);
                printf("[ok] key repeat: release disarms (no repeats "
                       "after keyup)\n");

                /* rate 0 is the protocol's repeat-disabled state. */
                ctx->ops->test_repeat_info(ctx->conn, 0, 200);
                ctx->ops->test_key(ctx->conn, 30, 1);
                held = g_rep_repeats;
                for (int i = 0; i < 5; i++) {
                    (void)fdk_pump_events(ctx, 60);
                }
                assert(g_rep_repeats == held);
                ctx->ops->test_key(ctx->conn, 30, 0);
                printf("[ok] key repeat: rate 0 disables (the "
                       "protocol contract)\n");
            }
            if (g_injector != NULL) {
                wayland_injector_stop();
            }
            fdk_window_destroy(rw);
            }
        }
    }


    /* ---- Menus / dialogs (Phase 9 completion) ----
     *
     * The popup machinery against a REAL compositor, two layers:
     *
     * 1. INTERACTIVE (when sway's IPC is reachable): swaymsg drives
     *    the seat cursor — REAL pointer events carrying REAL grab
     *    serials, the one thing a headless seat cannot mint any
     *    other way. A button click opens a context menu; the popup
     *    grabs with the click's serial; sway configures + maps it;
     *    a second click lands ON the popup's first item and
     *    ACTIVATES it. The item callback firing is end-to-end proof
     *    of the whole chain (input -> serial -> xdg_popup.grab ->
     *    configure -> auto-paint buffer -> hit-test -> activate ->
     *    chain close).
     *
     * 2. STRUCTURAL (always): popup creation must not error the
     *    protocol (a marshal/protocol error poisons the connection
     *    — the Phase 9 tests caught exactly that in the decoration
     *    path), and destroying the PARENT window sweeps the popup
     *    (the popup-family contract) with clean ASan teardown.
     *
     * The serial-0 grab (programmatic popup with no prior input) is
     * documented in fdk_window.h: sway never configures such a
     * popup — honest behavior, which is why the structural section
     * asserts survival, not mapping. */
    {
        fdk_font *font = fdk_font_load(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16);
        if (font == NULL) {
            printf("[skip] wayland menu popup (no system font)\n");
        } else {
            fdk_menu *menu = NULL;
            assert(fdk_ok(fdk_menu_create(font, &menu)));
            fdk_menu_item *mi_open = NULL;
            assert(fdk_ok(fdk_menu_append(menu, "Open", &mi_open)));
            assert(fdk_ok(fdk_menu_append_separator(menu)));
            assert(fdk_ok(fdk_menu_append_check(menu, "Toolbar",
                                                false, NULL)));

            /* The opener button on the MAIN window: clicking it pops
             * the menu below it — the canonical context-menu flow. */
            static int item_hits = 0;
            static int button_hits = 0;
            fdk_menu_item_set_on_activate(mi_open, wayland_menu_item_cb,
                                          &item_hits);
            fdk_widget *btn = NULL;
            assert(fdk_ok(fdk_button_create(root, font, "Menu", &btn)));
            g_wayland_menu = menu;
            g_wayland_menu_anchor = btn;
            fdk_button_set_on_activate(
                btn, wayland_menu_button_cb, &button_hits);
            fdk_widget_arrange(btn, (fdk_rect){20, 40, 90, 30});
            fdk_window_paint(win);
            (void)fdk_pump_events(ctx, 300);

            if (wayland_injector_start()) {
                /* Wait for the pointer capability to reach us (the
                 * device attaches on the injector's connection; the
                 * capabilities event arrives on our next roundtrip).
                 * Generous: under load the compositor's window
                 * placement can lag the injector by a good second,
                 * and the click below lands blind without this. */
                pump_and_paint(ctx, win, 1500);

                /* Window pinned at (100,60) by the rig's sway config;
                 * the button's center is therefore at (165,115). */
                wayland_inject("move 165 115");
                pump_and_paint(ctx, win, 500);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 600);
                assert(button_hits == 1);
                printf("[ok] wayland menu: REAL seat click reached "
                       "the button (valid grab serial minted)\n");

                /* The popup: anchored below the button (window-local
                 * (20,70), absolute (120,130)); its first row's
                 * center is around (160,143) — move + click there. */
                wayland_inject("move 160 143");
                pump_and_paint(ctx, win, 500);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 500);
                assert(item_hits == 1);
                printf("[ok] wayland menu: popup MAPPED, item "
                       "clicked THROUGH the real compositor, chain "
                       "closed\n");

                /* Outside-click dismissal: open again, click far
                 * away. The popup's grab routes the press to the
                 * popup tree -> sway dismisses (popup_done) -> the
                 * chain closes. Proof it closed: the opener button
                 * is clickable again (a live grab would have eaten
                 * it). */
                wayland_inject("move 165 115");
                pump_and_paint(ctx, win, 300);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 400);
                assert(button_hits == 2);
                wayland_inject("move 400 400");
                pump_and_paint(ctx, win, 300);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 400);
                wayland_inject("move 165 115");
                pump_and_paint(ctx, win, 300);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 400);
                assert(button_hits == 3);
                printf("[ok] wayland menu: outside click dismissed "
                       "the chain (opener reachable again)\n");
                wayland_injector_stop();
            } else {
                printf("[skip] wayland menu INTERACTIVE section "
                       "(fdk-wl-inject unavailable — serial-0 popups "
                       "never map on sway, so only the structural "
                       "checks can run here)\n");
            }

            /* Structural: serial-0 programmatic popup must not
             * poison the protocol (the decoration-marshal bug did),
             * and the parent sweep must take the popup down. */
            assert(fdk_ok(fdk_menu_popup_at(menu, btn, 0, 30)));
            (void)fdk_pump_events(ctx, 300);
            printf("[ok] wayland menu popup: protocol survived the "
                   "serial-0 popup request\n");

            fdk_menu_destroy(menu);
            /* The opener button BORROWS `font` — destroy it before the
             * font or every later repaint of this window's tree reads
             * freed metrics (a latent UAF the 1.1.6 pacing section's
             * configure-repaint first detonated, live under ASan). */
            fdk_widget_destroy(btn);
            fdk_font_destroy(font);
        }

        /* Dialog: a toplevel built and auto-painted by the toolkit;
         * the early-destroy path answers the negative response. */
        static int dlg_responses = 0;
        fdk_dialog_options dopts = {
            .title = "FDK wayland dialog",
            .text = "Hello from the Wayland suite",
            .buttons = FDK_DIALOG_BUTTONS_OK_CANCEL,
        };
        fdk_window *dlg = NULL;
        assert(fdk_ok(fdk_dialog_show_message(
            ctx, &dopts, wayland_dialog_response_cb, &dlg_responses,
            &dlg)));
        (void)fdk_pump_events(ctx, 300);
        assert(dlg_responses == 0); /* nobody answered yet */
        fdk_window_destroy(dlg); /* early destroy answers negative */
        (void)fdk_pump_events(ctx, 200);
        assert(dlg_responses == 1 &&
               g_wayland_dlg_last == FDK_DIALOG_CANCEL);
        printf("[ok] wayland dialog: mapped + auto-painted, early "
               "destroy answers Cancel, clean teardown\n");
    }


    /* ---- S-10: the date picker's popup calendar ----
     *
     * The same interactive discipline as the menu block: REAL seat
     * clicks carry REAL grab serials. A click on the field opens the
     * popup calendar below it; a click on a day cell applies the
     * date, fires on_changed, and closes; the destroy-while-open
     * guard unwinds when the outside click dismisses the orphaned
     * popup. */
    {
        fdk_font *font = fdk_font_load(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16);
        if (font == NULL) {
            printf("[skip] wayland date picker (no system font)\n");
        } else {
            int dp_hits0 = g_wl_dp_hits;
            fdk_widget *picker = NULL;
            assert(fdk_ok(fdk_date_picker_create(root, font, NULL,
                                                 &picker)));
            fdk_date start = {1970, 1, 8};
            assert(fdk_ok(fdk_date_picker_set_date(picker, &start)));
            fdk_date_picker_set_on_changed(picker,
                                            wayland_dp_changed_cb,
                                            NULL);
            fdk_size nat = {0, 0};
            fdk_widget_measure(picker, &nat);
            fdk_widget_arrange(picker, (fdk_rect){20, 90, nat.width,
                                                  nat.height});
            fdk_window_paint(win);
            (void)fdk_pump_events(ctx, 300);

            if (wayland_injector_start()) {
                pump_and_paint(ctx, win, 1500);

                /* The field's center (window at (100,60), field at
                 * (20,90)): absolute (120 + w/2, 150 + h/2). */
                int fx = 100 + 20 + nat.width / 2;
                int fy = 60 + 90 + nat.height / 2;
                char cmd[64];
                snprintf(cmd, sizeof cmd, "move %d %d", fx, fy);
                wayland_inject(cmd);
                pump_and_paint(ctx, win, 500);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 600);
                assert(fdk_date_picker_popup_open(picker));
                printf("[ok] wayland date picker: REAL click opened "
                       "the popup calendar\n");

                /* Jan 15 1970 = grid cell 17 (col 3, row 2). The
                 * popup anchors at the field's bottom-left, flipping
                 * ABOVE the field when the owner's bottom edge would
                 * clip it (the tooltip dance, replicated here — the
                 * 240px window forces the flip). Cell metrics
                 * replicate the widget's font-derived math (the
                 * pin). */
                fdk_font_metrics fm;
                fdk_font_get_metrics(font, &fm);
                fdk_text_metrics tm;
                assert(fdk_ok(fdk_font_measure_utf8(font, "00", 2,
                                                     &tm)));
                int th = fm.ascent + fm.descent;
                int cell_w = tm.advance_width + 10;
                int cell_h = th + 8;
                int grid_y = 6 + (th + 10) + 4 + (th + 6);
                int cal_h = 6 * 2 + (th + 10) + 4 + (th + 6) +
                            6 * cell_h;
                int pop_x = 100 + 20;
                int pop_y = 60 + 90 + (int)nat.height;
                if (pop_y + cal_h > 60 + 240) {
                    pop_y = 60 + 90 - cal_h; /* flip above the field */
                    if (pop_y < 60) {
                        pop_y = 60;
                    }
                }
                int px = pop_x + 6 + 3 * cell_w + cell_w / 2;
                int py = pop_y + grid_y + 2 * cell_h + cell_h / 2;
                snprintf(cmd, sizeof cmd, "move %d %d", px, py);
                wayland_inject(cmd);
                pump_and_paint(ctx, win, 500);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 600);
                assert(g_wl_dp_hits == dp_hits0 + 1);
                assert(g_wl_dp_got.year == 1970 &&
                       g_wl_dp_got.month == 1 && g_wl_dp_got.day == 15);
                assert(!fdk_date_picker_popup_open(picker));
                printf("[ok] wayland date picker: day picked THROUGH "
                       "the compositor, applied + closed\n");

                /* The guard: reopen, destroy the field, dismiss the
                 * orphaned popup with an outside click. */
                snprintf(cmd, sizeof cmd, "move %d %d", fx, fy);
                wayland_inject(cmd);
                pump_and_paint(ctx, win, 400);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 600);
                assert(fdk_date_picker_popup_open(picker));
                fdk_widget_destroy(picker);
                (void)fdk_pump_events(ctx, 200);
                wayland_inject("move 400 400");
                pump_and_paint(ctx, win, 400);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 500);
                printf("[ok] wayland date picker: field destroyed "
                       "while open, orphaned popup dismissed "
                       "cleanly\n");
                wayland_injector_stop();
            } else {
                printf("[skip] wayland date picker INTERACTIVE "
                       "section (injector unavailable)\n");
                fdk_widget_destroy(picker);
            }
            fdk_font_destroy(font);
        }
    }

    /* ---- Activation + startup notification (1.1.0, S-6) ----
     *
     * sway speaks xdg-activation-v1 (wlroots), so this is the REAL
     * protocol round: the mint's private-queue roundtrip catches the
     * compositor's done(token), and activate() flips sway's focus
     * when — and only when — the token is honest.
     *
     * The focus-stealing-prevention proof is two-sided, by design:
     *
     *   1. the BOGUS launch voucher (XDG_ACTIVATION_TOKEN, set
     *      before fdk_init, captured and unset per the spec) is
     *      spent on the first activate and IGNORED by sway — the
     *      focused window does not change; that is the protocol
     *      REFUSING an unvouched demand, which is the entire point.
     *
     *   2. a FRESH mint — carrying the serial of a real injected
     *      click — activates and sway DOES flip the focus to the
     *      minting window.
     *
     * Focus truth comes from sway's own tree (swaymsg -t get_tree),
     * queried through the rig's IPC socket: nothing client-side is
     * trusted. The second window (the one focused first) shares the
     * pinned floating spot with the main window and stacks above it
     * (sway stacks floats in creation order), so a click inside the
     * shared area focuses the SECOND window — the state activation
     * must overwrite. */
    {
        /* The env-capture discipline FIRST: main() set the bogus
         * voucher before fdk_init; the connection must have read it
         * once and unset it (grandchildren never inherit a stale
         * token — the protocol's hygiene rule). */
        assert(getenv("XDG_ACTIVATION_TOKEN") == NULL);
        printf("[ok] wayland activation: XDG_ACTIVATION_TOKEN captured "
               "and unset at connect\n");

        char tok[512];
        fdk_result mr = fdk_window_activation_token(win, tok,
                                                    sizeof tok);
        if (!fdk_ok(mr)) {
            printf("[skip] wayland activation token (this compositor "
                   "offers no xdg-activation-v1)\n");
        } else {
            assert(tok[0] != '\0');
            /* A second mint produces a DIFFERENT string — the
             * compositor mints per-request. */
            char tok2[512];
            assert(fdk_ok(fdk_window_activation_token(win, tok2,
                                                      sizeof tok2)));
            assert(strcmp(tok, tok2) != 0);
            printf("[ok] wayland activation token: compositor-minted "
                   "through the private-queue roundtrip (unique per "
                   "mint)\n");

            if (wayland_injector_start()) {
                pump_and_paint(ctx, win, 1200);

                /* Focus-follows-mouse off, SCOPED to this section:
                 * with sway's default, merely moving the pointer
                 * into a window focuses it, and the activation flips
                 * below could not be attributed to activation alone.
                 * Restored to the default before the section ends. */
                (void)swaymsg_config("seat * focus_follows_mouse no");

                /* B: small, at the pinned spot, leaving the main
                 * window's right side exposed (the launcher click
                 * below must land on the MAIN window's own surface —
                 * wlroots validates the token's origin surface
                 * against the seat's grab). */
                fdk_window *act_b = NULL;
                fdk_window_options bopts = {
                    .title = "FDK activation B", .width = 120,
                    .height = 100
                };
                assert(fdk_ok(fdk_window_create(ctx, &bopts, &act_b)));
                fdk_widget *broot = NULL;
                assert(fdk_ok(fdk_window_get_root(act_b, &broot)));
                fdk_widget_set_background(
                    broot, (fdk_color){0.3f, 0.6f, 0.3f, 1.0f});
                fdk_window_show(act_b);
                pump_and_paint(ctx, act_b, 800);

                const char *focused = sway_focused_window_name();
                if (focused == NULL) {
                    printf("[skip] wayland activation focus legs "
                           "(swaymsg IPC unavailable)\n");
                } else {
                    /* A real click on B: focus lands on B (the
                     * pre-activation state). */
                    wayland_inject("move 150 100");
                    pump_and_paint(ctx, win, 400);
                    wayland_inject("tap 1");
                    pump_and_paint(ctx, act_b, 600);
                    focused = sway_focused_window_name();
                    assert(focused != NULL);
                    assert(strcmp(focused, "FDK activation B") == 0);
                    printf("[ok] wayland activation: injected click "
                           "focused window B (the pre-activation "
                           "state)\n");

                    /* (1) the bogus voucher, spent on the first
                     * activate: sway ignores it — focus unchanged. */
                    assert(fdk_ok(fdk_window_activate(win)));
                    pump_and_paint(ctx, win, 600);
                    focused = sway_focused_window_name();
                    assert(focused != NULL);
                    assert(strcmp(focused, "FDK activation B") == 0);
                    printf("[ok] wayland activation: bogus launch "
                           "voucher REFUSED by the compositor "
                           "(focus-stealing prevention)\n");

                    /* (2) THE LAUNCHER FLOW, two processes — the
                     * exact contract fdk_window_activation_token
                     * documents. The button goes DOWN on the MAIN
                     * window's exposed area and STAYS down (wlroots
                     * validates the mint's serial against the seat's
                     * live grab AND its origin surface); the mint
                     * rides that serial; the re-exec'd child spends
                     * the token as its launch voucher while the
                     * button is still held — and sway, vouched for
                     * by the live grab on the launcher's surface,
                     * flips the focus to the child. A plain tap
                     * would release before the child activates and
                     * the serial would be refused (the held-button
                     * rule every real launcher satisfies by minting
                     * at press time). */
                    wayland_inject("move 300 200");
                    pump_and_paint(ctx, win, 400);
                    wayland_inject("down 1");
                    pump_and_paint(ctx, win, 500);

                    char tok[512];
                    assert(fdk_ok(fdk_window_activation_token(
                        win, tok, sizeof tok)));

                    setenv("XDG_ACTIVATION_TOKEN", tok, 1);
                    setenv("FDK_WL_ACT_CHILD", "1", 1);
                    int child_rc = system("/proc/self/exe > /dev/null 2>&1"
                                          " </dev/null");
                    unsetenv("FDK_WL_ACT_CHILD");
                    unsetenv("XDG_ACTIVATION_TOKEN");
                    wayland_inject("up 1");
                    pump_and_paint(ctx, win, 400);
                    if (child_rc == 2) {
                        printf("[skip] wayland activation launcher "
                               "flow (child could not connect)\n");
                    } else {
                        assert(child_rc == 0);
                        printf("[ok] wayland activation: LAUNCHER "
                               "flow honored — held-button token, "
                               "child process, focus flipped through "
                               "the voucher\n");
                    }

                    /* Health: the parent connection survived the
                     * whole exchange (a protocol error would have
                     * killed it); the surface still presents. */
                    assert(fdk_ok(fdk_window_paint(win)));
                    pump_and_paint(ctx, win, 300);
                }

                (void)swaymsg_config("seat * focus_follows_mouse yes");
                fdk_window_destroy(act_b);
                wayland_injector_stop();
            } else {
                printf("[skip] wayland activation focus legs "
                       "(injector unavailable)\n");
            }
        }
    }

    /* ---- 1.1.6: buffer starvation / frame pacing ----
     *
     * The live report from Muffin's experimental Wayland session: the
     * 250ms starvation guard expired while the compositor was (legally)
     * slow, every guard-forced repaint hit a full pool, and the
     * acquisition path REFUSED — ~80 identical WARN lines per session
     * and frames dropped on the floor (reading as "Wayland feels
     * slower than X11"). Three properties now hold, each asserted
     * deterministically against sway:
     *
     *   1. frame_ready NEVER declares ready while every render slot
     *      is in flight — even past the guard. (Pre-fix: after 250ms
     *      of unacknowledged silence it said 1 and the app slammed
     *      into the refusal.)
     *   2. An acquisition against a full pool WAITS for a release
     *      (bounded, release-queue-dispatched) and succeeds, instead
     *      of dropping the frame. (Pre-fix: FDK_ERR_SURFACE_CREATE.)
     *   3. Pumping recovers readiness — releases and the frame
     *      callback are credited by the ordinary loop.
     *
     * Exhaustion is built by presenting back-to-back WITHOUT pumping:
     * no dispatch means no wl_buffer::release is ever credited, no
     * frame callback ever read — exactly the app-side half of a slow
     * compositor. */
    {
        fdk_window *pw = NULL;
        /* app_id is context-level (org.fdk.test, see fdk_init above)
         * — the sway rig floats every suite window, so the client
         * resize below takes effect like every real decorations
         * user's would. */
        fdk_window_options popts = { .title = "FDK pacing",
                                     .width = 400, .height = 300 };
        assert(fdk_ok(fdk_window_create(ctx, &popts, &pw)));
        fdk_widget *proot = NULL;
        assert(fdk_ok(fdk_window_get_root(pw, &proot)));
        fdk_widget_set_background(proot,
                                  (fdk_color){0.9f, 0.2f, 0.2f, 1.0f});
        fdk_window_show(pw);
        pump_and_paint(ctx, pw, 400); /* mapped + presenting */

        fdk_surface *ps = NULL;
        assert(fdk_ok(fdk_window_get_surface(pw, &ps)));

        /* Exhaust: four damaged presents, zero pumping in between.
         * The commits stay in libwayland's OUTPUT buffer (nothing
         * flushed them), so sway cannot have released anything and
         * the pool is fully busy: pacing must answer NOT-ready. */
        for (int i = 0; i < 4; i++) {
            fdk_widget_set_background(
                proot, (i & 1) ? (fdk_color){0.2f, 0.9f, 0.2f, 1.0f}
                               : (fdk_color){0.2f, 0.2f, 0.9f, 1.0f});
            (void)fdk_window_paint(pw);
        }
        assert(fdk_surface_frame_ready(ps) == 0);
        printf("[ok] wayland pacing: full pool + unacknowledged frame "
               "-> frame_ready says wait\n");

        /* Let the starvation guard expire (300ms) and paint anyway —
         * an app pacing on its own clock, or the dispatch tail's
         * synchronous resize repaint. The acquisition must WAIT for a
         * release (bounded, release-queue-dispatched: the wait's own
         * flush finally hands sway the buffered commits, and its
         * answers arrive within the window) and LAND the frame —
         * pre-1.1.6 this was an instant refusal + WARN + dropped
         * frame (the rig's zero-refusal check pins that half; the
         * pre-fix control build fails it, proving the probe sees the
         * bug). The guard-time full-pool state itself only persists
         * on compositors slower than the wait window (Muffin's
         * experimental Wayland was the live report) — against a
         * healthy sway the wait always recovers, which is exactly
         * what this asserts. */
        struct timespec guard_expire = { .tv_sec = 0,
                                         .tv_nsec = 300L * 1000L * 1000L };
        nanosleep(&guard_expire, NULL);
        fdk_widget_set_background(proot,
                                  (fdk_color){0.9f, 0.9f, 0.2f, 1.0f});
        assert(fdk_ok(fdk_window_paint(pw)));
        printf("[ok] wayland pacing: acquisition against a full pool "
               "waited for the release instead of dropping the frame\n");

        /* Ordinary loop shape recovers readiness. */
        pump_and_paint(ctx, pw, 400);
        assert(fdk_surface_frame_ready(ps) == 1);
        printf("[ok] wayland pacing: pumping recovers readiness "
               "(release + frame callback credited)\n");

        /* 1.1.7: the RESIZE-CHURN path. Re-exhaust the pool at the
         * current size with unflushed presents, then move the window
         * to a NEW size and repaint: every busy slot is now at the
         * wrong size, so the 1.1.6 wait — which exits only on a
         * CURRENT-size release — could never be satisfied on a
         * compositor that holds its buffers past the wait window; it
         * burned the full 100ms budget per frame before the
         * wrong-size reaper fired anyway: ~10fps interactive
         * resizing with an idle CPU (the live report: "still laggy
         * when resize, the CPU is not being fully used"). The
         * classified churn budget reaches progress after ONE poll
         * slice instead.
         *
         * HONEST LIMITATION (same class as the 1.1.6 pacing note):
         * sway releases buffers within a frame, crediting the pool
         * before the acquisition wait even starts — so on THIS
         * compositor pre-fix and post-fix converge to a few ms and
         * the wall-clock bound below is a generous ceiling/tripwire,
         * not the discriminator. The 100ms-vs-10ms difference only
         * bites on compositors slower than the release window
         * (Muffin under load was the live report; the user's re-test
         * is that control). What IS asserted functionally here: the
         * churn acquisition completes without a refusal episode, and
         * the window settles presenting at the new size. */
        for (int i = 0; i < 4; i++) {
            fdk_widget_set_background(
                proot, (i & 1) ? (fdk_color){0.2f, 0.9f, 0.2f, 1.0f}
                               : (fdk_color){0.2f, 0.2f, 0.9f, 1.0f});
            (void)fdk_window_paint(pw);
        }
        long churn_t0 = monotonic_ms();
        /* resize() records the new size and synthesizes the frontend
         * configure — whose dispatch tail repaints synchronously at
         * the new size, straight into the exhausted wrong-size pool.
         * All of that happens inside this call, so the elapsed time
         * IS the acquisition cost. */
        fdk_window_resize(pw, 380, 320);
        long churn_ms = monotonic_ms() - churn_t0;
        assert(churn_ms < 90); /* ceiling tripwire: post-fix is one
                                  poll slice + alloc; a full
                                  FDK_WL_RELEASE_WAIT_MS stall means
                                  the churn budget regressed */
        printf("[ok] wayland pacing: resize-churn acquisition bounded "
               "(%ldms) and frame landed at the new size\n",
               churn_ms);
        pump_and_paint(ctx, pw, 400); /* settle at the new size */
        fdk_size psize;
        assert(fdk_ok(fdk_window_get_size(pw, &psize)));
        if (psize.width == 380 && psize.height == 320) {
            printf("[ok] wayland pacing: window settled presenting at "
                   "the churned size (380x320, no refusal episode)\n");
        } else {
            /* Compositor-owned geometry (kiosk-shell configures every
             * window fullscreen): fdk_window_resize honestly refused,
             * the churned acquisition was the kiosk configure's own
             * synchronous repaint — the same honest skip the reflow
             * test takes here. */
            printf("[ok] wayland pacing: churn bounded under "
                   "compositor-owned geometry (client resize refused, "
                   "size stays %dx%d)\n", psize.width, psize.height);
        }

        fdk_window_destroy(pw);
    }

    /* ---- 1.1.6: cursor shaping (edge affordance) ----
     *
     * The resize cursor that was X11-only in 1.1.4: on Wayland the
     * client owns its cursor pixels, and 1.1.6 ships the XCursor
     * theme loader (wayland_cursor.c) behind the same window_set_cursor
     * op. Driven with REAL injected motion — the serial a set_cursor
     * request must cite is only minted by real input. The rig's sway
     * config floats every org.fdk.test window at (100,60), so the
     * 300x200 window's right edge is output x=399 and its vertical
     * middle is output y=160. */
    if (wayland_injector_start()) {
        fdk_window_destroy(win); /* sole-window determinism (see above) */
        win = NULL;

        fdk_window *cw = NULL;
        fdk_window_options copts = { .title = "FDK cursor",
                                     .width = 300, .height = 200 };
        assert(fdk_ok(fdk_window_create(ctx, &copts, &cw)));
        fdk_widget *croot = NULL;
        assert(fdk_ok(fdk_window_get_root(cw, &croot)));
        fdk_widget_set_background(croot,
                                  (fdk_color){0.25f, 0.25f, 0.35f, 1.0f});
        fdk_window_set_resizable(cw, true);
        /* CSD, like every decorations user: without it sway draws its
         * own titlebar and the surface sits 2/25px inside the placed
         * container, shifting every window-local coordinate. */
        assert(fdk_ok(fdk_window_set_decorated(cw, true)));
        fdk_window_show(cw);
        pump_and_paint(ctx, cw, 500); /* pointer capability arrives */

        /* Right edge, middle height: the E resize cursor. */
        wayland_inject("move 399 160");
        pump_and_paint(ctx, cw, 300);
        assert(fdk__window_cursor_edge(cw) == 3 /* FDK_WRES_E */);
        printf("[ok] wayland cursor: edge hover armed the directional "
               "resize cursor (E)\n");

        /* Interior: back to the default arrow. */
        wayland_inject("move 250 160");
        pump_and_paint(ctx, cw, 300);
        assert(fdk__window_cursor_edge(cw) == 0 /* FDK_WRES_NONE */);
        printf("[ok] wayland cursor: interior motion restored the "
               "default shape\n");

        /* 1.1.7: a TOP-EDGE press must RESIZE, not move. The cursor
         * has promised N here since 1.1.6, but the press filter's
         * origin gate ran BEFORE the compositor-driven begin_resize,
         * and Wayland's ops table has no window_get_position — so
         * every origin-moving edge (N/NE/NW/W/SW) dead-ended out of
         * the filter, the press fell through to the deco band, and
         * the band MOVED the window (the live report: "it grabs it
         * even tho the cursor to resize shows"). The press is now
         * consumed by xdg_toplevel.resize; the rig's WAYLAND_DEBUG
         * assertions pin the protocol half (a resize request, zero
         * move requests anywhere in this suite run). Window sits at
         * (100,60) CSD: output (150,63) is window-local (50,3) —
         * the N edge, inside the band's rows. */
        wayland_inject("move 150 63");
        pump_and_paint(ctx, cw, 300);
        assert(fdk__window_cursor_edge(cw) == 1 /* FDK_WRES_N */);
        printf("[ok] wayland cursor: top edge over the band armed "
               "the N resize cursor\n");
        wayland_inject("tap 1"); /* press: the resize handover;
                                     release 30ms later ends it — no
                                     motion in between, size stays */
        pump_and_paint(ctx, cw, 400);
        printf("[ok] wayland resize: top-edge press handed to the "
               "compositor (xdg_toplevel.resize, rig-verified)\n");

        /* ---- 1.1.0-rc2, S-5: the full named cursor set ----
         *
         * Two proofs against the live compositor: (a) the window
         * layer maps every public shape onto the op space and SENDS
         * it (the injector's real serials satisfy the production
         * op's guard); (b) the test seam drives the apply path per
         * shape and returns the cursor-shape-v1 ENUM each
         * set_shape request cited — the protocol's own numbering,
         * pinned row by row. Compositors without the manager (or
         * seats without a pointer) honestly skip. */
        if (ctx->ops->test_cursor_apply != NULL) {
            /* (a) the window layer: every public shape applies. */
            for (int v = 0; v < (int)FDK_CURSOR__COUNT; v++) {
                fdk_window_set_cursor(cw, (fdk_cursor_shape)v);
                int want = (v == 0) ? 0
                          : (v == 1) ? FDK_PLATFORM_CURSOR_POINTER
                                     : FDK_PLATFORM_CURSOR_NAMED_BASE +
                                       (v - 2);
                assert(fdk__window_cursor_applied(cw) == want);
            }
            printf("[ok] wayland named cursors: all 22 public shapes "
                   "applied through the production op (real serials)\n");

            /* (b) the seam: the v1 enum table, row by row. The
             * protocol's own numbering (cursor-shape-v1): the order
             * is the public enum's. */
            static const int named_v1[FDK_PLATFORM_CURSOR_NAMED_COUNT] = {
                9,  /* TEXT             */
                8,  /* CROSSHAIR        */
                6,  /* WAIT             */
                5,  /* PROGRESS         */
                13, /* MOVE             */
                12, /* COPY             */
                11, /* ALIAS            */
                14, /* NO_DROP          */
                15, /* NOT_ALLOWED      */
                16, /* GRAB             */
                17, /* GRABBING         */
                32, /* ALL_SCROLL       */
                30, /* COL_RESIZE       */
                31, /* ROW_RESIZE       */
                33, /* ZOOM_IN          */
                34, /* ZOOM_OUT         */
                3,  /* HELP             */
                7,  /* CELL             */
                2,  /* CONTEXT_MENU     */
                10, /* VERTICAL_TEXT    */
            };
            int sent = 0;
            for (int i = 0; i < FDK_PLATFORM_CURSOR_NAMED_COUNT; i++) {
                int op = FDK_PLATFORM_CURSOR_NAMED_BASE + i;
                int v1 = ctx->ops->test_cursor_apply(ctx->conn,
                                                     cw->pwindow, op);
                if (v1 < 0) {
                    break; /* no manager on this compositor: skip */
                }
                assert(v1 == named_v1[i]);
                sent++;
            }
            if (sent == FDK_PLATFORM_CURSOR_NAMED_COUNT) {
                /* The legacy trio + two compass rows, same seam. */
                assert(ctx->ops->test_cursor_apply(
                           ctx->conn, cw->pwindow, 0) == 1);  /* DEFAULT */
                assert(ctx->ops->test_cursor_apply(
                           ctx->conn, cw->pwindow,
                           FDK_PLATFORM_CURSOR_POINTER) == 4);
                assert(ctx->ops->test_cursor_apply(
                           ctx->conn, cw->pwindow, FDK_WRES_E) == 18);
                assert(ctx->ops->test_cursor_apply(
                           ctx->conn, cw->pwindow, FDK_WRES_N) == 19);
                printf("[ok] wayland named cursors: the v1 enum table "
                       "pinned row by row (20 named + hand + arrow + "
                       "compass, real set_shape requests on the wire)\n");
            } else {
                printf("[skip] wayland named cursors: compositor "
                       "offers no cursor-shape-v1 manager (%d rows "
                       "sent)\n", sent);
            }
        } else {
            printf("[skip] wayland named cursors: library built "
                   "without the test seam\n");
        }

        /* ---- 1.1.0-rc2, S-4 / A-06: the input region ----
         *
         * The rig floats org.fdk.test windows at (100,60): a 300x200
         * window whose input region covers only its left half — the
         * injector's REAL pointer (a wlr virtual device) moving into
         * the right half must produce NO enter on this surface (the
         * compositor routes by the input region; nothing is beneath
         * in headless, so no events arrive anywhere), while the left
         * half enters normally. The leave side: moving from inside
         * the region to the hole synthesizes FDK's LEAVE. */
        {
            const fdk_rect left_half = {0, 0, 150, 200};
            assert(fdk_ok(fdk_window_set_input_region(cw, &left_half,
                                                      1)));
            int enters = 0, leaves = 0;
            fdk_window_set_event_callback(cw, region_count_cb,
                                          &(struct region_counts){
                                              &enters, &leaves});
            /* Outside the window entirely first, then ZERO the
             * counters (the previous section left the pointer inside
             * this very window — its leave must not pollute). */
            wayland_inject("move 700 400");
            pump_and_paint(ctx, cw, 300);
            enters = 0;
            leaves = 0;
            /* Into the RIGHT half (the hole): NO enter. Output
             * (350,160) = window-local (250,100) — inside the
             * window, outside the region. */
            wayland_inject("move 350 160");
            pump_and_paint(ctx, cw, 300);
            assert(enters == 0);
            /* Into the LEFT half (the region): enter. Output
             * (150,160) = window-local (50,100). */
            wayland_inject("move 150 160");
            pump_and_paint(ctx, cw, 300);
            assert(enters == 1);
            /* Back into the hole: leave. */
            wayland_inject("move 350 160");
            pump_and_paint(ctx, cw, 300);
            assert(leaves == 1);
            /* The clear: full input again — the right half enters. */
            assert(fdk_ok(fdk_window_set_input_region(cw, NULL, 0)));
            wayland_inject("move 350 160");
            pump_and_paint(ctx, cw, 300);
            assert(enters == 2);
            printf("[ok] wayland input region: the hole takes no "
                   "pointer, the region does, leave on exit, clear "
                   "restores full input\n");
        }

        fdk_window_destroy(cw);

        /* ---- 1.2.5: query_pointer hit-testing unification ----
         *
         * The contract the X11 side always ran: "inside" means
         * inside the CURRENT geometry. The Wayland op used to answer
         * from pointer_focus alone, so a window that SHRANK under a
         * stationary pointer reported inside with cached coordinates
         * OUTSIDE the new size — window_revalidate_pointer then
         * routed a synthetic out-of-bounds MOTION where the X11
         * backend delivers the honest LEAVE. The client resize path
         * makes the stale-cache state deterministic: last_size is
         * client-set at request time, while the seat cache still
         * holds the pre-resize position (no compositor round-trip
         * needed to observe it). */
        {
            fdk_window *qw = NULL;
            fdk_window_options qopts = { .title = "FDK query contract",
                                         .width = 300, .height = 200 };
            assert(fdk_ok(fdk_window_create(ctx, &qopts, &qw)));
            fdk_widget *qroot = NULL;
            assert(fdk_ok(fdk_window_get_root(qw, &qroot)));
            fdk_widget_set_background(qroot,
                                      (fdk_color){0.25f, 0.25f, 0.35f, 1.0f});
            fdk_window_set_resizable(qw, true);
            assert(fdk_ok(fdk_window_set_decorated(qw, true)));
            fdk_window_show(qw);
            pump_and_paint(ctx, qw, 500);

            /* Hover the maximize button (the rig floats org.fdk.test
             * windows at (100,60); output = window-local + that). */
            fdk_rect qmax = qw->deco_btn_max;
            assert(qmax.width > 0 && qmax.height > 0);
            char move[64];
            snprintf(move, sizeof move, "move %d %d",
                     100 + qmax.x + qmax.width / 2,
                     60 + qmax.y + qmax.height / 2);
            wayland_inject(move);
            pump_and_paint(ctx, qw, 300);
            assert(fdk__window_deco_hover(qw) == 2);

            /* Shrink under the stationary pointer, then interrogate
             * the op BEFORE any pump: pointer_focus still points
             * here with pre-resize coordinates; last_size is already
             * 120x100. The unified contract must answer OUTSIDE. */
            fdk_window_resize(qw, 120, 100);
            assert(qw->ops->window_query_pointer != NULL);
            fdk_i32 qx = -999, qy = -999;
            assert(qw->ops->window_query_pointer(qw->pwindow, &qx,
                                                 &qy) == 0);
            printf("[ok] wayland query: shrunken geometry reports the "
                   "pointer as outside (the unified hit-test "
                   "contract, same as X11)\n");

            /* The observable half: the revalidation flush delivers
             * the clearing path — hover and cursor neutral. */
            pump_and_paint(ctx, qw, 400);
            assert(fdk__window_deco_hover(qw) == 0);
            assert(fdk__window_cursor_edge(qw) == 0);
            printf("[ok] wayland query: hover and cursor cleared "
                   "after the out-of-bounds shrink (leave "
                   "synthesis)\n");

            fdk_window_destroy(qw);
        }
        wayland_injector_stop();
    } else {
        printf("[skip] wayland cursor section (fdk-wl-inject "
               "unavailable — REAL input serials cannot be minted "
               "here)\n");
    }

    /* ---- 1.2.0: drag and drop (external raw-libwayland source) ----
     *
     * The interop rig: scripts/wl_dnd_source is a REAL external
     * client (plain libwayland + the generated xdg-shell protocol,
     * zero FDK code) that starts a wl_data_device drag when the
     * injected pointer presses and moves on ITS surface. The rig's
     * sway config floats it at (100,380); our drop-target window
     * floats at (100,60) (CSD, so the surface is exactly there).
     * The REAL pointer then drags from its surface onto ours and
     * releases: FDK must answer the drag offer with set_actions,
     * receive the drop, decode the uri-list to POSIX paths, and
     * finish the offer. The X11 suite pins the same protocol shape
     * against raw-Xlib clients; this is the Wayland half. */
    {
        const char *src_bin = getenv("FDK_WL_DND_SOURCE_BIN");
        if (src_bin == NULL) {
            src_bin = "/home/z/my-project/scripts/wl_dnd_source";
        }
        if (access("/home/z/my-project/scripts/fdk-wl-inject", X_OK) == 0 &&
            access(src_bin, X_OK) == 0) {
            /* ORDER MATTERS (the sway lesson): a client connecting
             * AFTER the virtual pointer exists gets caps=0 in its
             * initial capabilities event (and get_pointer before any
             * capability is a protocol error) — the pointer
             * capability only ever arrives as a mid-connection
             * UPDATE when the device is created. So: spawn the
             * external source FIRST (it dispatches, waiting for a
             * pointer), THEN start the injector — its virtual
             * pointer's creation is the update that arms the
             * source's listener. */
            fdk_window *dw = NULL;
            fdk_window_options dopts = { .title = "FDK drop target",
                                         .width = 300, .height = 200 };
            assert(fdk_ok(fdk_window_create(ctx, &dopts, &dw)));
            fdk_window_set_drop_formats(
                dw, FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST);
            fdk_widget *droot = NULL;
            assert(fdk_ok(fdk_window_get_root(dw, &droot)));
            fdk_widget_set_background(droot,
                                      (fdk_color){0.15f, 0.35f, 0.2f, 1.0f});
            /* CSD so the surface sits exactly at the container's
             * (100,60) — no sway titlebar offset. */
            assert(fdk_ok(fdk_window_set_decorated(dw, true)));
            fdk_window_show(dw);
            pump_and_paint(ctx, dw, 400);
            fdk_window_set_event_callback(dw, wl_dnd_rx_event, NULL);

            /* Spawn the source; give it a moment to connect (caps=0,
             * it waits), then create the virtual pointer. */
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "%s both", src_bin);
            FILE *src = popen(cmd, "r");
            assert(src != NULL);
            int src_fd = fileno(src);
            fcntl(src_fd, F_SETFL, O_NONBLOCK);
            char line[512];
            char src_out[4096] = {0};
            for (int i = 0; i < 8; i++) {
                pump_and_paint(ctx, dw, 100);
            }
            if (!wayland_injector_start()) {
                (void)pclose(src);
                fdk_window_destroy(dw);
                printf("[skip] wayland dnd section (virtual pointer "
                       "unavailable on this compositor)\n");
            } else {
            /* PRIME the virtual pointer: sway grants the seat's
             * pointer capability only when the device produces its
             * FIRST event (the injector#1 trace shows caps(1)
             * arriving exactly at the first motion_absolute, 1.5s
             * after create_virtual_pointer). Without a priming
             * motion the source waits for a pointer that never
             * materializes. */
            wayland_inject("move 640 360");
            int ready = 0;
            for (int i = 0; i < 40 && !ready; i++) {
                pump_and_paint(ctx, dw, 100);
                ssize_t n;
                while ((n = read(src_fd, line, sizeof(line) - 1)) > 0) {
                    line[n] = '\0';
                    strncat(src_out, line,
                            sizeof(src_out) - strlen(src_out) - 1);
                }
                if (strstr(src_out, "WL-SRC: ready") != NULL) {
                    ready = 1;
                }
            }
            assert(ready);

            memset(&wl_dnd_rx, 0, sizeof(wl_dnd_rx));
            /* The drag: press on the source surface (center of its
             * (100,380,300x200) rect), move (it starts the drag on
             * press+motion), drag up onto our window, release. */
            wayland_inject("move 250 480");
            pump_and_paint(ctx, dw, 250);
            wayland_inject("down 1");
            pump_and_paint(ctx, dw, 200);
            wayland_inject("move 260 470");
            pump_and_paint(ctx, dw, 250);
            wayland_inject("move 200 300");
            pump_and_paint(ctx, dw, 250);
            wayland_inject("move 250 160"); /* over our surface */
            pump_and_paint(ctx, dw, 300);
            wayland_inject("up 1");
            /* Let the drop transfer + finish flow. */
            for (int i = 0; i < 30; i++) {
                pump_and_paint(ctx, dw, 100);
                ssize_t n;
                while ((n = read(src_fd, line, sizeof(line) - 1)) > 0) {
                    line[n] = '\0';
                    strncat(src_out, line,
                            sizeof(src_out) - strlen(src_out) - 1);
                }
                if (strstr(src_out, "WL-SRC: finished") != NULL ||
                    strstr(src_out, "WL-SRC: cancelled") != NULL) {
                    break;
                }
            }
            fputs(src_out, stdout);
            fflush(stdout);

            assert(wl_dnd_rx.enters >= 1);
            assert(wl_dnd_rx.drops == 1);
            assert(wl_dnd_rx.uri_count == 2);
            assert(strcmp(wl_dnd_rx.uris[0], "/etc/hostname") == 0);
            assert(strstr(src_out, "WL-SRC: send text/uri-list") != NULL);
            /* HONEST LIMITATION (wlroots 0.18): the drop is delivered,
             * the payload served, and FDK's protocol tail is clean
             * (receive -> finish -> destroy, protocol-trace verified
             * by this rig) — but this wlroots sends the SOURCE the
             * ::cancelled end signal instead of ::dnd_finished after
             * the post-drop finish. Both count as "the drag ended";
             * the source-side result model cannot be pinned harder
             * against this compositor (the X11 suite pins XDND's
             * full handshake against raw-Xlib clients instead). */
            assert(strstr(src_out, "WL-SRC: finished") != NULL ||
                   strstr(src_out, "WL-SRC: cancelled") != NULL);
            printf("[ok] wayland dnd: external raw-libwayland source "
                   "dropped 2 files, decoded to POSIX paths, offer "
                   "receive+finish+destroy protocol-clean\n");

            (void)pclose(src);
            fdk_window_destroy(dw);
            wayland_injector_stop();
            }
        } else {
            printf("[skip] wayland dnd section (fdk-wl-inject or "
                   "wl_dnd_source unavailable)\n");
        }
    }

    /* ---- 1.3.3: shortcuts + menu accelerators through the REAL
     * window dispatch ----
     *
     * sway headless cannot deliver wl_keyboard.key to clients at all
     * (the seat never adopts a virtual keyboard's keymap —
     * protocol-traced, see scripts/verify_chooser_wayland.sh), so
     * the backend's 3-line evdev copy is reviewed code and REAL key
     * delivery is covered on the X11 side. What this section pins
     * is the dispatch CHAIN itself on the Wayland-built library:
     * window shortcut table -> live menu-bar accelerator scan ->
     * widget tree -> app callback, exactly as fdk_window_dispatch_
     * event orders them, driven through the same internal seam the
     * rest of this suite uses. */
    {
        fdk_window *aw = NULL;
        fdk_window_options awopts = { .title = "FDK accelerators",
                                      .width = 320, .height = 200 };
        assert(fdk_ok(fdk_window_create(ctx, &awopts, &aw)));
        fdk_widget *aroot = NULL;
        assert(fdk_ok(fdk_window_get_root(aw, &aroot)));
        fdk_font *afont = fdk_font_load_system_default(15);

        fdk_widget *abar = NULL;
        fdk_menu *am = NULL;
        fdk_menu_item *asave = NULL;
        assert(fdk_ok(fdk_menu_create(afont, &am)));
        assert(fdk_ok(fdk_menu_append(am, "Save", &asave)));
        assert(fdk_ok(fdk_menu_item_set_shortcut(asave, "Ctrl+S")));
        if (afont != NULL) {
            assert(fdk_ok(fdk_menu_bar_create(aroot, afont, &abar)));
        }
        /* The bar needs the font; fontless systems still get the
         * application-shortcut half. */
        if (abar != NULL) {
            assert(fdk_ok(fdk_menu_bar_append(abar, "File", am)));
        }

        fdk_widget *aentry = NULL;
        assert(fdk_ok(fdk_entry_create(aroot, afont, "", &aentry)));
        assert(fdk_widget_focus(aentry));

        int app_shortcut_hits = 0;
        int menu_hits = 0;
        int app_keys = 0;
        fdk_window_add_shortcut(aw, "Ctrl+L",
                                wl_accel_app_shortcut, &app_shortcut_hits);
        if (abar != NULL) {
            fdk_menu_set_on_activate(am, wl_accel_menu_cb, &menu_hits);
        }
        fdk_window_set_event_callback(aw, wl_accel_window_cb, &app_keys);

        fdk_event_data k;
        memset(&k, 0, sizeof(k));
        k.type = FDK_EVENT_KEY_DOWN;

        /* Ctrl+L: the app table consumes it (entry never sees the
         * key, the app callback never sees the key). */
        k.key.scancode = 38; /* L */
        k.key.modifiers = FDK_MOD_CTRL;
        fdk_window_dispatch_event(aw, &k);
        assert(app_shortcut_hits == 1);
        assert(strcmp(fdk_entry_get_text(aentry), "") == 0);
        assert(app_keys == 0);

        /* Plain "l" (no mods): nobody claims it; the ENTRY eats it
         * as text (the tree layer is alive under the table). */
        k.key.modifiers = 0;
        k.key.codepoint = 'l';
        fdk_window_dispatch_event(aw, &k);
        assert(strcmp(fdk_entry_get_text(aentry), "l") == 0);
        assert(app_keys == 0); /* consumed by the Entry */

        /* Ctrl+S: the live bar scan fires Save (no popup opened,
         * state machine intact), the app sees no key. */
        if (abar != NULL) {
            k.key.scancode = 31; /* S */
            k.key.modifiers = FDK_MOD_CTRL;
            k.key.codepoint = 0;
            fdk_window_dispatch_event(aw, &k);
            assert(menu_hits == 1);
            assert(app_keys == 0);
            /* Removing the bar's listing kills the accelerator on
             * the very next press (the scan is live). */
            assert(fdk_ok(fdk_menu_bar_remove(abar, 0)));
            fdk_window_dispatch_event(aw, &k);
            assert(menu_hits == 1);
            /* Nobody wants it now; the entry refuses ctrl-modified
             * text, so it falls through to the app callback. */
            assert(app_keys == 1);
        }

        /* Entry undo through the same dispatch: type "ab", Ctrl+Z,
         * Ctrl+Y — restoration on the Wayland build. Note "l", "a",
         * "b" share one contiguous typing frontier, so ALL THREE
         * coalesce into a single undo step (the documented rule). */
        k.key.modifiers = 0;
        k.key.codepoint = 'a';
        k.key.scancode = 30;
        fdk_window_dispatch_event(aw, &k);
        k.key.codepoint = 'b';
        k.key.scancode = 48;
        fdk_window_dispatch_event(aw, &k);
        assert(strcmp(fdk_entry_get_text(aentry), "lab") == 0);
        k.key.modifiers = FDK_MOD_CTRL;
        k.key.scancode = 44; /* Z */
        k.key.codepoint = 'z';
        fdk_window_dispatch_event(aw, &k);
        assert(strcmp(fdk_entry_get_text(aentry), "") == 0);
        k.key.scancode = 21; /* Y */
        k.key.codepoint = 'y';
        fdk_window_dispatch_event(aw, &k);
        assert(strcmp(fdk_entry_get_text(aentry), "lab") == 0);

        if (abar != NULL) {
            fdk_menu_destroy(am);
        }
        fdk_window_destroy(aw);
        if (afont != NULL) {
            fdk_font_destroy(afont);
        }
        printf("[ok] wayland accelerators: dispatch chain (table -> "
               "live bar scan -> tree -> app callback), live removal, "
               "entry undo/redo — driven through fdk_window_dispatch_"
               "event (sway headless cannot route wl_keyboard.key; "
               "real-key e2e is the X11 suite's)\n");
    }

    /* ---- Touch slots (1.1.0, M-3) ----
     *
     * sway headless's seat carries no touch capability (wlroots has
     * no virtual-touch protocol), so the rig drives the REAL wl_touch
     * listener bodies through the ops test seam — the test_key
     * discipline: the bodies the protocol events would call, the
     * same id→window routing, the same dispatch. The CORE engine
     * (tap clicks, two-finger scroll) runs identically behind both
     * backends; its full policy battery lives in tests/test_touch.c,
     * and the X11 suite drives the XI2 twin of this same seam. */
    {
        fdk_window *tw = NULL;
        fdk_window_options topts = { .title = "FDK touch test",
                                     .width = 220, .height = 140 };
        assert(fdk_ok(fdk_window_create(ctx, &topts, &tw)));
        fdk_window_show(tw);
        (void)fdk_pump_events(ctx, 100);

        /* The capture rides the window callback; synthesized clicks
         * arrive through the same dispatch as raw touch events. */
        fdk_window_set_event_callback(tw, wl_touch_evt_cb, &g_wl_touch);
        g_wl_touch = (wl_touch_capture){ 0 };
        fdk_platform_window *tpw = tw->pwindow;
        assert(ctx->ops->test_touch != NULL);

        /* Tap: down + up in place -> raw pair + synthesized click. */
        (void)ctx->ops->test_touch(ctx->conn, tpw, 0, 1, 60.0, 50.0);
        (void)ctx->ops->test_touch(ctx->conn, tpw, 2, 1, 0, 0);
        (void)fdk_pump_events(ctx, 60);
        assert(g_wl_touch.touch_downs == 1);
        assert(g_wl_touch.touch_ups == 1);
        assert(g_wl_touch.clicks == 1);
        assert(g_wl_touch.releases == 1);
        assert(g_wl_touch.click_pos.x == 60.0f);
        assert(g_wl_touch.click_pos.y == 50.0f);

        /* Two-finger scroll: natural deltas, no phantom click. */
        g_wl_touch = (wl_touch_capture){ 0 };
        (void)ctx->ops->test_touch(ctx->conn, tpw, 0, 10, 100.0, 100.0);
        (void)ctx->ops->test_touch(ctx->conn, tpw, 0, 11, 160.0, 100.0);
        (void)ctx->ops->test_touch(ctx->conn, tpw, 1, 10, 120.0, 140.0);
        (void)ctx->ops->test_touch(ctx->conn, tpw, 2, 10, 120.0, 140.0);
        (void)ctx->ops->test_touch(ctx->conn, tpw, 2, 11, 160.0, 100.0);
        (void)fdk_pump_events(ctx, 60);
        assert(g_wl_touch.scrolls == 1);
        assert(g_wl_touch.last_scroll_dx == -10.0f);
        assert(g_wl_touch.last_scroll_dy == -20.0f);
        assert(g_wl_touch.clicks == 0);

        /* Cancel: every slot dies, nothing synthesizes. */
        g_wl_touch = (wl_touch_capture){ 0 };
        (void)ctx->ops->test_touch(ctx->conn, tpw, 0, 20, 30.0, 30.0);
        (void)ctx->ops->test_touch(ctx->conn, tpw, 3, 20, 0, 0);
        (void)ctx->ops->test_touch(ctx->conn, tpw, 2, 20, 30.0, 30.0);
        (void)fdk_pump_events(ctx, 60);
        assert(g_wl_touch.touch_cancels == 1);
        assert(g_wl_touch.clicks == 0);

        /* Unknown surface: a down aimed at NULL drops cleanly. */
        g_wl_touch = (wl_touch_capture){ 0 };
        assert(ctx->ops->test_touch(ctx->conn, NULL, 0, 99, 5.0, 5.0) == 0);

        fdk_window_set_event_callback(tw, NULL, NULL);
        fdk_window_destroy(tw);
        printf("[ok] wayland touch: tap->click, two-finger natural scroll, "
               "cancel — through the real wl_touch listener bodies "
               "(the test seam; sway's seat has no touch device)\n");
    }

    /* ---- Toplevel icon (1.1.0, M-4) ----
     *
     * xdg-toplevel-icon-v1 is compositor-OPTIONAL: sway 1.10/wlroots
     * 0.18 (this rig) and GNOME do not offer it; KDE Plasma 6.3+
     * does. Both worlds must stay healthy through set_icon:
     *   - manager-less: the documented honest no-op (one INFO line,
     *     no protocol traffic, no crash);
     *   - manager-present: icon -> add_buffer -> set_icon + commit,
     *     and the connection must survive the next roundtrip (any
     *     protocol error — bad format, non-square buffer, use of a
     *     destroyed object — kills the connection at the next
     *     dispatch, which the pump below would surface as a
     *     dispatch failure, not a silent pass).
     * A 16x12 NON-square source exercises the letterbox path (the
     * square-buffer protocol requirement) wherever the manager
     * exists; the clear form runs after it. */
    {
        fdk_window *iw = NULL;
        fdk_window_options iopts = { .title = "FDK icon test",
                                     .width = 220, .height = 140 };
        assert(fdk_ok(fdk_window_create(ctx, &iopts, &iw)));
        /* A root + background, the deferred-first-frame test's shape:
         * paint needs a tree to damage, and the present assertion
         * below needs a frame actually committed. */
        fdk_widget *iroot = NULL;
        assert(fdk_ok(fdk_window_get_root(iw, &iroot)));
        fdk_widget_set_background(iroot,
                                  (fdk_color){0.3f, 0.3f, 0.3f, 1.0f});
        fdk_window_show(iw);
        (void)fdk_pump_events(ctx, 100);

        fdk_u32 icon[16 * 12];
        for (int i = 0; i < 16 * 12; i++) {
            icon[i] = 0xFF000000u | (uint32_t)(i * 7) |
                      ((uint32_t)(i * 13) << 8) | ((uint32_t)(i * 29) << 16);
        }
        fdk_window_set_icon(iw, icon, 16, 12);
        (void)fdk_pump_events(ctx, 150);
        fdk_window_set_icon(iw, NULL, 0, 0);
        (void)fdk_pump_events(ctx, 150);
        /* The window is still fully alive: a normal paint round
         * proves the surface path never tripped on icon state. */
        fdk_window_paint(iw);
        (void)fdk_pump_events(ctx, 150);
        assert(fdk__window_ever_presented(iw) != 0);

        fdk_window_destroy(iw);
        printf("[ok] wayland toplevel icon: set + clear survive both "
               "compositor worlds (manager bound or absent), "
               "connection healthy after\n");
    }

    /* ---- IME end-to-end (1.1.0, M-1) ----
     *
     * The IM side connects FIRST (before any window): sway's relay
     * binds the input method when it appears, and every later
     * text-input enable reaches it as activate — the robust order
     * (the IM-after-app order is sway's known start-race window).
     * The injector's virtual keyboard gives the seat KEYBOARD
     * capability so sway auto-focuses the mapped windows: without it
     * (bare run, no injector binary) the text-input never gets
     * enter and the driven legs skip honestly. */
    {
    if (!ime_client_start()) {
        printf("[skip] wayland ime: no zwp_input_method_manager_v2 "
               "here (compositor without an IME relay — kiosk-shell "
               "weston)\n");
    } else {
        int focus_ever = 0;

        /* LEG A — the pipeline: no text widget, the window callback
         * is the composition surface. */
        {
            memset(&g_ime_cb, 0, sizeof g_ime_cb);
            fdk_window *aw = NULL;
            fdk_window_options aopts = { .title = "FDK ime pipeline",
                                         .width = 260, .height = 120 };
            assert(fdk_ok(fdk_window_create(ctx, &aopts, &aw)));
            fdk_widget *aroot = NULL;
            assert(fdk_ok(fdk_window_get_root(aw, &aroot)));
            fdk_widget_set_background(aroot,
                                      (fdk_color){0.25f, 0.25f, 0.3f, 1.0f});
            fdk_window_set_event_callback(aw, ime_win_cb, NULL);
            fdk_window_show(aw);

            (void)wayland_injector_start();
            int base_activations = g_ime.activations;
            for (int i = 0; i < 45 &&
                            g_ime.activations == base_activations;
                 i++) {
                (void)fdk_pump_events(ctx, 100);
                ime_client_poll(25);
            }
            if (g_ime.activations == base_activations) {
                printf("[skip] wayland ime A: no keyboard focus this "
                       "run (seat without keyboard capability — "
                       "injector unavailable)\n");
            } else {
                focus_ever = 1;

                /* The preedit batch: composition text shows (and
                 * revises) at the window callback. */
                ime_send("nihao", NULL);
                for (int i = 0; i < 20 && g_ime_cb.preedits == 0; i++) {
                    (void)fdk_pump_events(ctx, 50);
                    ime_client_poll(20);
                }
                assert(g_ime_cb.preedits == 1);
                assert(strcmp(g_ime_cb.last_preedit, "nihao") == 0);

                /* A revision in the SAME composition: the v3 batch
                 * replaces the preedit. */
                ime_send("ni hao", NULL);
                for (int i = 0; i < 20 && g_ime_cb.preedits == 1; i++) {
                    (void)fdk_pump_events(ctx, 50);
                    ime_client_poll(20);
                }
                assert(g_ime_cb.preedits == 2);
                assert(strcmp(g_ime_cb.last_preedit, "ni hao") == 0);

                /* The commit batch: the preedit clears FIRST, then
                 * the final text lands (the v3 replace semantic). */
                ime_send(NULL, "\xe4\xbd\xa0\xe5\xa5\xbd"); /* 你好 */
                for (int i = 0; i < 20 && g_ime_cb.commits == 0; i++) {
                    (void)fdk_pump_events(ctx, 50);
                    ime_client_poll(20);
                }
                assert(g_ime_cb.commits == 1);
                assert(strcmp(g_ime_cb.last_commit,
                              "\xe4\xbd\xa0\xe5\xa5\xbd") == 0);
                assert(g_ime_cb.preedits == 3); /* the clear arrived */
                assert(g_ime_cb.last_preedit[0] == '\0');

                printf("[ok] wayland ime A: preedit (show/revise/"
                       "clear) + commit reached the window callback "
                       "through the real sway relay\n");
            }
            fdk_window_destroy(aw);
            /* The window's death deactivates the IM on the relay;
             * drain the deactivate before leg B re-activates it. */
            for (int i = 0; i < 10 && g_ime.deactivations == 0; i++) {
                (void)fdk_pump_events(ctx, 50);
                ime_client_poll(20);
            }
        }

        /* LEG B — the Entry: COMMIT splices into the buffer and the
         * surrounding-text loop closes (the IM SEES the entry). */
        {
            g_ime_entry_changes = 0;
            g_ime.surround_count = 0;
            g_ime.surround[0] = '\0';
            fdk_window *bw = NULL;
            fdk_window_options bopts = { .title = "FDK ime entry",
                                         .width = 300, .height = 140 };
            assert(fdk_ok(fdk_window_create(ctx, &bopts, &bw)));
            fdk_widget *broot = NULL;
            assert(fdk_ok(fdk_window_get_root(bw, &broot)));
            fdk_widget *bentry = NULL;
            assert(fdk_ok(fdk_entry_create(broot, NULL, "", &bentry)));
            fdk_entry_set_on_changed(bentry, ime_entry_changed, NULL);
            assert(fdk_widget_focus(bentry));
            fdk_window_show(bw);

            int base_activations = g_ime.activations;
            for (int i = 0; i < 45 &&
                            g_ime.activations == base_activations;
                 i++) {
                (void)fdk_pump_events(ctx, 100);
                ime_client_poll(25);
            }
            if (g_ime.activations == base_activations &&
                g_injector != NULL) {
                /* The click fallback (the key-repeat section's
                 * discipline): a REAL pointer press with a REAL
                 * serial — sway re-evaluates keyboard focus on it.
                 * The window floats at (100,60) per the rig config;
                 * 300x140 -> center (250,130). */
                wayland_inject("move 250 130");
                pump_and_paint(ctx, bw, 300);
                wayland_inject("tap 1");
                for (int i = 0; i < 30 &&
                                g_ime.activations == base_activations;
                     i++) {
                    (void)fdk_pump_events(ctx, 100);
                    ime_client_poll(25);
                }
            }
            if (g_ime.activations == base_activations) {
                printf("[skip] wayland ime B: no keyboard focus this "
                       "run (act=%d deact=%d)\n",
                       g_ime.activations, g_ime.deactivations);
            } else {
                /* The composition first: the entry consumes PREEDIT
                 * (the window callback must NOT see it — the capture
                 * from leg A was reset and stays empty). */
                memset(&g_ime_cb, 0, sizeof g_ime_cb);
                ime_send("nihao", NULL);
                for (int i = 0; i < 20 && g_ime_entry_changes == 0; i++) {
                    (void)fdk_pump_events(ctx, 50);
                    ime_client_poll(20);
                }
                assert(strcmp(fdk_entry_get_text(bentry), "") == 0);
                assert(g_ime_cb.preedits == 0); /* consumed by the
                                                 * entry, never fell
                                                 * through */

                /* The commit: the entry splices it at the caret. */
                ime_send(NULL, "h\xc7\x8eo"); /* "hǎo" */
                for (int i = 0; i < 20 && g_ime_entry_changes == 0;
                     i++) {
                    (void)fdk_pump_events(ctx, 50);
                    ime_client_poll(20);
                }
                assert(g_ime_entry_changes == 1);
                assert(strcmp(fdk_entry_get_text(bentry), "h\xc7\x8eo") == 0);

                /* The full loop: our surrounding push (fired by the
                 * IME routing hook after the commit) must reach the
                 * IM side through the relay — text AND byte cursor. */
                for (int i = 0; i < 20 && g_ime.surround_count == 0;
                     i++) {
                    (void)fdk_pump_events(ctx, 50);
                    ime_client_poll(20);
                }
                assert(g_ime.surround_count > 0);
                assert(strcmp(g_ime.surround, "h\xc7\x8eo") == 0);
                assert(g_ime.surround_cursor == 4); /* h(1) + ǎ(2) + o(1)
                                                      * bytes — the caret
                                                      * at the end */

                printf("[ok] wayland ime B: entry consumed COMMIT "
                       "(splice + on_changed) and the IM received the "
                       "updated surrounding text + caret — the full "
                       "loop through sway\n");
            }
            fdk_window_destroy(bw);
        }

        if (focus_ever == 0) {
            printf("[ok] wayland ime: the text-input-v3 binding stayed "
                   "healthy through an IM-present compositor (no "
                   "focus this run; the driven legs skipped)\n");
        }
        ime_client_stop();
    }
    }

    /* ---- Idle inhibition (1.1.0, S-3) ----
     *
     * sway/wlroots implements zwp_idle_inhibit_manager_v1, so the
     * COMPOSITOR path engages here: the inhibitor object lives on
     * the window's surface, the status flips ACTIVE synchronously
     * (no bus in this rig — the D-Bus half is pinned by
     * test_dbus_live's mock ScreenSaver), and both the release and
     * the window-dies-inhibited shapes keep the connection healthy
     * (any protocol error on the inhibitor would kill the next
     * roundtrip). Weston kiosk-shell offers no manager: the honest
     * UNSUPPORTED handle, still released cleanly. */
    {
        fdk_window *dw = NULL;
        fdk_window_options dopts = { .title = "FDK idle test",
                                     .width = 220, .height = 120 };
        assert(fdk_ok(fdk_window_create(ctx, &dopts, &dw)));
        fdk_widget *droot = NULL;
        assert(fdk_ok(fdk_window_get_root(dw, &droot)));
        fdk_widget_set_background(droot, (fdk_color){0.3f, 0.2f, 0.3f, 1.0f});
        fdk_window_show(dw);
        (void)fdk_pump_events(ctx, 100);

        fdk_idle_inhibitor *inh = NULL;
        assert(fdk_ok(fdk_idle_inhibit(ctx, dw, "fdk-tests",
                                       "the rig display", &inh)));
        assert(inh != NULL);
        if (fdk_idle_inhibitor_status(inh) == FDK_IDLE_INHIBIT_ACTIVE) {
            printf("[ok] wayland idle: compositor inhibitor engaged "
                   "(ACTIVE)\n");
            fdk_idle_uninhibit(inh);
            (void)fdk_pump_events(ctx, 150);
            /* The window is still fully alive: a paint round proves
             * the surface path never tripped on inhibitor state. */
            fdk_window_paint(dw);
            (void)fdk_pump_events(ctx, 150);
            assert(fdk__window_ever_presented(dw) != 0);

            /* The window-dies-inhibited shape: engage, destroy the
             * window WITH the inhibitor up (the destroy path must
             * release it — a surface-scoped proxy outliving its
             * surface would be a protocol error), and the
             * connection must survive the next roundtrip. */
            fdk_idle_inhibitor *inh2 = NULL;
            assert(fdk_ok(fdk_idle_inhibit(ctx, dw, "fdk-tests",
                                           "dying engaged", &inh2)));
            assert(fdk_idle_inhibitor_status(inh2) ==
                   FDK_IDLE_INHIBIT_ACTIVE);
            fdk_window_destroy(dw);
            (void)fdk_pump_events(ctx, 200);
            fdk_idle_uninhibit(inh2); /* severed at destroy: no-op
                                       * on the compositor half */
            printf("[ok] wayland idle: release + destroy-while-"
                   "inhibited both protocol-clean\n");
        } else {
            printf("[ok] wayland idle: no manager here — the honest "
                   "UNSUPPORTED handle, released cleanly\n");
            fdk_idle_uninhibit(inh);
            fdk_window_destroy(dw);
        }
        (void)fdk_pump_events(ctx, 100);
    }

    if (win != NULL) {
        fdk_window_destroy(win);
    }
    fdk_shutdown(ctx);
    printf("[ok] clean teardown under Wayland\n");

    printf("\nall Wayland integration tests passed\n");
    return 0;
}
