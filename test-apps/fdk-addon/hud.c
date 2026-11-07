/* hud.c — the fdk-hud add-on: a floating gauge HUD panel.
 *
 * The release-proof third-party add-on for FDK's extension API. It
 * is built as its own shared library against an INSTALLED FDK (see
 * isolated_build.sh) and loaded with dlopen(); it touches exactly
 * the public surface docs/EXTENDING.md grants add-ons:
 *
 *   fdk_ext_register_kind / fdk_ext_surface_create   the panel
 *   fdk_ext_get_native_display                       the Xlib Display*
 *   fdk_ext_popup_create                             the right-click menu
 *   fdk_ext_loop_add_fd                              the command pipe
 *   fdk_ext_surface_get_surface + the public fdk_surface draw API
 *   fdk_timer_add                                    the animation
 *   fdk_quit                                         the menu's Quit
 *
 * The X11 role it speaks: an override-redirect always-on-top HUD
 * window (no WM chrome, no focus policy — what a HUD is). Other
 * backends get the honest FDK_ERR_UNSUPPORTED refusal.
 */

#include "hud.h"

#include <fdk/fdk.h>
#include <fdk/fdk_extend.h>
#include <fdk/fdk_text.h>
#include <fdk/fdk_theme.h>
#include <fdk/fdk_window.h>

#include <X11/Xlib.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define HUD_API_VERSION_CHECK FDK_EXT_API_VERSION

#define HUD_W 240
#define HUD_H 200
#define HUD_MENU_W 190
#define HUD_MENU_ROW 30
#define HUD_MENU_ROWS 3

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct fdk_hud {
    fdk_context *ctx;
    fdk_ext_surface_kind *kind;
    fdk_ext_surface *surf;
    fdk_font *font_big;
    fdk_font *font_small;

    float value;   /* what the gauge shows (eases)   */
    float target;  /* what it eases toward           */
    char label[64];
    bool frozen;

    fdk_timer *tick;
    fdk_ext_fd_source *cmd_src;
    int cmd_fd;          /* the host's write end    */
    int pipe_fd;         /* our read end            */
    char pend[128];      /* partial line accumulator*/
    size_t pend_len;

    fdk_ext_surface *popup;
    int popup_hover;

    int user_x, user_y; /* the panel's screen position (options)   */

    fdk_hud_stats st;
} fdk_hud;

/* ---- the palette (a HUD is self-colored; the accent follows the
 *      desktop theme so the panel never looks bolted on) ---- */

static fdk_color hud_col(float r, float g, float b, float a) {
    fdk_color c = {r, g, b, a};
    return c;
}

static fdk_color hud_accent(void) {
    fdk_color c = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    if (c.a == 0.0f) {
        c = hud_col(0.35f, 0.65f, 1.0f, 1.0f);
    }
    return c;
}

/* ---- rendering ---- */

static void hud_draw_text(fdk_surface *s, fdk_font *f,
                          const char *text, int cx, int baseline_y,
                          fdk_color col) {
    fdk_text_metrics m = {0, 0, 0};
    if (fdk_ok(fdk_font_measure_utf8(f, text, strlen(text), &m)) &&
        m.advance_width > 0) {
        fdk_surface_draw_utf8(s, f, text, strlen(text),
                              cx - m.advance_width / 2, baseline_y, col);
    }
}

static void hud_arc_segment(fdk_surface *s, float cx, float cy,
                            float r, float a0, float a1, fdk_color c) {
    /* a 3-px-thick radial tick: two parallel 1-px lines */
    float ca0 = cosf(a0), sa0 = sinf(a0);
    float ca1 = cosf(a1), sa1 = sinf(a1);
    fdk_surface_draw_line(s, (int)(cx + ca0 * (r - 1.0f)),
                          (int)(cy + sa0 * (r - 1.0f)),
                          (int)(cx + ca1 * (r - 1.0f)),
                          (int)(cy + sa1 * (r - 1.0f)), c);
    fdk_surface_draw_line(s, (int)(cx + ca0 * r), (int)(cy + sa0 * r),
                          (int)(cx + ca1 * r), (int)(cy + sa1 * r), c);
    fdk_surface_draw_line(s, (int)(cx + ca0 * (r + 1.0f)),
                          (int)(cy + sa0 * (r + 1.0f)),
                          (int)(cx + ca1 * (r + 1.0f)),
                          (int)(cy + sa1 * (r + 1.0f)), c);
}

static void hud_render(fdk_hud *h) {
    if (h->surf == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(h->surf, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }

    fdk_color bg = hud_col(0.055f, 0.05f, 0.075f, 0.96f);
    fdk_color track = hud_col(0.16f, 0.15f, 0.20f, 1.0f);
    fdk_color accent = hud_accent();
    fdk_color text = hud_col(0.92f, 0.92f, 0.95f, 1.0f);
    fdk_color dim = hud_col(0.55f, 0.55f, 0.62f, 1.0f);

    fdk_surface_fill(s, bg);

    /* the border, 2 px, accent-tinted */
    fdk_rect r2;
    r2.x = 0;               r2.y = 0;
    r2.width = info.width;  r2.height = 2;
    fdk_surface_fill_rect(s, r2, accent);
    r2.y = info.height - 2;
    fdk_surface_fill_rect(s, r2, accent);
    r2.y = 0; r2.width = 2; r2.height = info.height;
    fdk_surface_fill_rect(s, r2, accent);
    r2.x = info.width - 2; r2.width = 2;
    fdk_surface_fill_rect(s, r2, accent);

    /* the gauge: a 270-degree arc from 135 to 405 degrees */
    float cx = (float)info.width / 2.0f;
    float cy = (float)info.height / 2.0f - 12.0f;
    float radius = 52.0f;
    enum { SEGS = 54 };
    int lit = (int)(h->value * (float)SEGS + 0.5f);
    float a0 = (float)M_PI * 0.75f;
    float sweep = (float)M_PI * 1.5f;
    for (int i = 0; i < SEGS; i++) {
        float t0 = a0 + sweep * (float)i / (float)SEGS;
        float t1 = a0 + sweep * (float)(i + 1) / (float)SEGS - 0.012f;
        hud_arc_segment(s, cx, cy, radius, t0, t1,
                        i < lit ? accent : track);
    }

    /* the needle tick at the value edge */
    float na = a0 + sweep * h->value;
    fdk_surface_draw_line(
        s, (int)(cx + cosf(na) * (radius - 12.0f)),
        (int)(cy + sinf(na) * (radius - 12.0f)),
        (int)(cx + cosf(na) * (radius + 9.0f)),
        (int)(cy + sinf(na) * (radius + 9.0f)), text);

    /* the big readout, centered in the arc */
    char readout[32];
    snprintf(readout, sizeof readout, "%.0f%%", (double)(h->value * 100.0f));
    hud_draw_text(s, h->font_big, readout, (int)cx, (int)cy + 10,
                  text);

    /* the caption */
    hud_draw_text(s, h->font_small, h->label, (int)cx,
                  info.height - 18, dim);

    fdk_surface_present(s);
}

/* ---- the popup menu ---- */

static const char *menu_items[HUD_MENU_ROWS] = {
    "Freeze value", "Hide HUD", "Quit demo",
};

static void hud_menu_render(fdk_hud *h) {
    if (h->popup == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(h->popup, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    fdk_color bg = hud_col(0.09f, 0.085f, 0.12f, 1.0f);
    fdk_color hover = hud_accent();
    fdk_color text = hud_col(0.92f, 0.92f, 0.95f, 1.0f);
    fdk_surface_fill(s, bg);
    for (int i = 0; i < HUD_MENU_ROWS; i++) {
        if (i == h->popup_hover) {
            fdk_rect row;
            row.x = 2;
            row.y = i * HUD_MENU_ROW + 1;
            row.width = info.width - 4;
            row.height = HUD_MENU_ROW - 2;
            fdk_surface_fill_rect(s, row, hover);
        }
        fdk_text_metrics m = {0, 0, 0};
        if (fdk_ok(fdk_font_measure_utf8(h->font_small, menu_items[i],
                                         strlen(menu_items[i]), &m)) &&
            m.advance_width > 0) {
            fdk_surface_draw_utf8(
                s, h->font_small, menu_items[i], strlen(menu_items[i]),
                (info.width - m.advance_width) / 2,
                i * HUD_MENU_ROW + HUD_MENU_ROW / 2 + 5, text);
        }
    }
    fdk_surface_present(s);
}

static void hud_menu_close(fdk_hud *h) {
    if (h->popup != NULL) {
        fdk_ext_surface_destroy(h->popup); /* not the add-on's kind */
        h->popup = NULL;
        h->popup_hover = -1;
    }
}

static void hud_menu_pick(fdk_hud *h, int row) {
    if (row < 0 || row >= HUD_MENU_ROWS) {
        return;
    }
    h->st.menu_picks++;
    switch (row) {
    case 0: /* Freeze value: stop easing where it is */
        h->frozen = true;
        h->target = h->value;
        break;
    case 1: /* Hide HUD */
        fdk_ext_surface_hide(h->surf);
        break;
    case 2: /* Quit demo: the host's fdk_run returns */
        fdk_quit(h->ctx);
        break;
    default:
        break;
    }
    hud_menu_close(h);
    hud_render(h);
}

static void hud_menu_event(fdk_ext_surface *surface,
                           const fdk_event_data *e, void *user) {
    (void)surface;
    fdk_hud *h = user;
    switch (e->type) {
    case FDK_EVENT_WINDOW_EXPOSE:
        hud_menu_render(h);
        break;
    case FDK_EVENT_POINTER_MOTION: {
        int row = (int)(e->pointer.position.y / (float)HUD_MENU_ROW);
        if (row < 0 || row >= HUD_MENU_ROWS) {
            row = -1;
        }
        if (row != h->popup_hover) {
            h->popup_hover = row;
            hud_menu_render(h);
        }
        break;
    }
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        if (e->pointer_button.button == 1) {
            int row = (int)(e->pointer_button.position.y /
                            (float)HUD_MENU_ROW);
            hud_menu_pick(h, row);
        }
        break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST: /* outside click / Escape */
        hud_menu_close(h);
        break;
    default:
        break;
    }
}

static void hud_menu_open(fdk_hud *h, int x, int y) {
    if (h->popup != NULL) {
        return;
    }
    fdk_ext_surface *pop = NULL;
    fdk_result r = fdk_ext_popup_create(
        h->surf, (fdk_i32)x, (fdk_i32)y, HUD_MENU_W,
        HUD_MENU_ROW * HUD_MENU_ROWS + 2, &pop);
    if (!fdk_ok(r)) {
        return; /* no grab available: stay up, menu-less */
    }
    h->popup = pop;
    h->popup_hover = -1;
    h->st.menu_opens++;
    fdk_ext_surface_set_event_callback(pop, hud_menu_event, h);
    hud_menu_render(h);
}

/* ---- the panel's own event stream ---- */

static void hud_panel_event(fdk_ext_surface *surface,
                            const fdk_event_data *e, void *user) {
    (void)surface;
    fdk_hud *h = user;
    switch (e->type) {
    case FDK_EVENT_WINDOW_EXPOSE:
        /* EXPOSE is a repaint obligation: no widget tree repaints
         * for us, the add-on re-draws and re-presents itself. */
        h->st.exposes++;
        hud_render(h);
        break;
    case FDK_EVENT_WINDOW_CONFIGURE:
        hud_render(h);
        break;
    case FDK_EVENT_POINTER_MOTION: {
        /* the hand over the arc ring, the arrow elsewhere */
        float dx = e->pointer.position.x - (float)HUD_W / 2.0f;
        float dy = e->pointer.position.y - ((float)HUD_H / 2.0f - 12.0f);
        float dist = sqrtf(dx * dx + dy * dy);
        fdk_ext_surface_set_cursor(
            h->surf,
            dist > 38.0f && dist < 66.0f ? FDK_CURSOR_POINTER
                                         : FDK_CURSOR_DEFAULT);
        break;
    }
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        if (e->pointer_button.button == 3) {
            hud_menu_open(h, (int)e->pointer_button.position.x,
                          (int)e->pointer_button.position.y);
        }
        break;
    default:
        break;
    }
}

/* ---- the kind callbacks (the role) ---- */

static fdk_result hud_kind_create(fdk_ext_surface *surface,
                                  fdk_ext_native_handle *out_native,
                                  void *user) {
    fdk_hud *h = user;
    fdk_context *ctx = fdk_ext_surface_context(surface);

    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd))) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (nd.backend != FDK_EXT_BACKEND_X11) {
        /* the honest refusal: this build speaks the X11 HUD role
         * only; a Wayland build would bind its own protocol here */
        return FDK_ERR_UNSUPPORTED;
    }
    Display *dpy = (Display *)nd.display;
    int scr = DefaultScreen(dpy);

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.override_redirect = True; /* the HUD role: chromeless, on top */
    swa.background_pixel = BlackPixel(dpy, scr);
    swa.border_pixel = BlackPixel(dpy, scr);
    Window w = XCreateWindow(dpy, DefaultRootWindow(dpy), h->user_x,
                             h->user_y, HUD_W, HUD_H, 0, CopyFromParent,
                             InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel |
                                 CWBorderPixel,
                             &swa);
    if (w == None) {
        return FDK_ERR_WINDOW_CREATE;
    }
    /* built UNMAPPED with every property set; FDK maps it at
     * fdk_ext_surface_show and installs its own event mask */
    out_native->backend = FDK_EXT_BACKEND_X11;
    out_native->xid = (fdk_u64)w;
    h->st.kind_creates++;
    return FDK_OK;
}

static void hud_kind_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    fdk_hud *h = user;
    /* the ROLE objects would be torn down here, against a live
     * native surface; this role's objects are the X properties the
     * server discards with the window, so the counter is the proof */
    h->st.kind_destroys++;
}

/* ---- the animation timer ---- */

static void hud_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_hud *h = user;
    if (h->surf == NULL || h->frozen) {
        return;
    }
    float d = h->target - h->value;
    if (d > 0.0005f || d < -0.0005f) {
        h->value += d * 0.18f;
        if ((h->target - h->value > 0.0f) !=
            (d > 0.0f)) { /* overshot */
            h->value = h->target;
        }
        hud_render(h);
    }
}

/* ---- the command pipe (the fd source) ---- */

static void hud_command(fdk_hud *h, const char *line) {
    h->st.commands++;
    float v = 0.0f;
    if (sscanf(line, "value %f", &v) == 1) {
        h->frozen = false;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        h->target = v;
    } else if (strncmp(line, "label ", 6) == 0) {
        snprintf(h->label, sizeof h->label, "%s", line + 6);
        hud_render(h);
    } else if (strcmp(line, "hide") == 0) {
        fdk_ext_surface_hide(h->surf);
    } else if (strcmp(line, "show") == 0) {
        fdk_ext_surface_show(h->surf);
        hud_render(h);
    } else if (strcmp(line, "menu") == 0) {
        hud_menu_open(h, 12, HUD_H - 12);
    } else if (strcmp(line, "quit") == 0) {
        fdk_quit(h->ctx);
    }
}

static void hud_cmd_source(fdk_ext_fd_source *source, uint32_t revents,
                           void *user) {
    (void)source;
    fdk_hud *h = user;
    if (revents & (FDK_EXT_FD_ERR | FDK_EXT_FD_HUP)) {
        return; /* the host closed its end; the panel stays up */
    }
    for (;;) {
        char buf[96];
        ssize_t n = read(h->pipe_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return; /* EPIPE / closed: stop reading */
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                h->pend[h->pend_len] = '\0';
                hud_command(h, h->pend);
                h->pend_len = 0;
            } else if (h->pend_len + 1 < sizeof h->pend) {
                h->pend[h->pend_len++] = buf[i];
            }
        }
        if (n < (ssize_t)sizeof buf) {
            return; /* drained */
        }
    }
}

/* the final counters of the last shutdown — stats_read(NULL)
 * answers from here, since the instance itself is freed */
static fdk_hud_stats hud_last_stats;

/* ---- the exported API ---- */

int fdk_hud_api_version(void) {
    return FDK_HUD_API_VERSION;
}

fdk_result fdk_hud_init(fdk_context *ctx, const fdk_hud_options *o,
                        fdk_hud **out) {
    if (ctx == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* the compile/run-time extension-API pair, per the contract */
    if (HUD_API_VERSION_CHECK < 1 || fdk_ext_api_version() < 1) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_NONE) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    fdk_hud *h = calloc(1, sizeof *h);
    if (h == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    h->ctx = ctx;
    h->popup_hover = -1;
    h->target = 0.0f;
    h->value = 0.0f;
    if (o != NULL) {
        h->target = o->value < 0.0f ? 0.0f : (o->value > 1.0f ? 1.0f
                                                              : o->value);
        h->value = h->target;
        if (o->label != NULL) {
            snprintf(h->label, sizeof h->label, "%s", o->label);
        }
    }
    if (h->label[0] == '\0') {
        snprintf(h->label, sizeof h->label, "fdk-hud");
    }

    /* the fonts: the public text API, the desktop's UI font */
    int px = (int)fdk_ui_font_px();
    h->font_big = fdk_font_load_system_default((fdk_i32)(px * 2));
    h->font_small = fdk_font_load_system_default((fdk_i32)px);
    if (h->font_big == NULL || h->font_small == NULL) {
        fdk_font_destroy(h->font_big == NULL ? NULL : h->font_big);
        fdk_font_destroy(h->font_small == NULL ? NULL : h->font_small);
        free(h);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* the surface kind */
    fdk_ext_surface_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.struct_size = sizeof ops;
    ops.create = hud_kind_create;
    ops.destroy = hud_kind_destroy;
    fdk_result r = fdk_ext_register_kind(ctx, "org.fdk.hud.panel", &ops,
                                         h, &h->kind);
    if (!fdk_ok(r)) {
        fdk_font_destroy(h->font_big);
        fdk_font_destroy(h->font_small);
        free(h);
        return r;
    }

    /* the panel surface (hidden until fdk_hud_show); the create
     * callback reads the options' position, so they land first */
    h->user_x = o ? o->x : 40;
    h->user_y = o ? o->y : 40;
    r = fdk_ext_surface_create(h->kind, &h->surf);
    if (!fdk_ok(r)) {
        fdk_ext_kind_destroy(h->kind);
        fdk_font_destroy(h->font_big);
        fdk_font_destroy(h->font_small);
        free(h);
        return r;
    }
    fdk_ext_surface_set_event_callback(h->surf, hud_panel_event, h);

    /* the animation timer: the public timer API IS the add-on
     * timer surface (no extension twin, by design) */
    h->tick = fdk_timer_add(ctx, 33, true, hud_tick, h);

    /* the command pipe + its fd source */
    int fds[2];
    if (pipe(fds) != 0) {
        fdk_ext_surface_destroy(h->surf);
        fdk_ext_kind_destroy(h->kind);
        fdk_timer_remove(h->tick);
        fdk_font_destroy(h->font_big);
        fdk_font_destroy(h->font_small);
        free(h);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    h->pipe_fd = fds[0];
    h->cmd_fd = fds[1];
    h->cmd_src =
        fdk_ext_loop_add_fd(ctx, fds[0], FDK_EXT_FD_IN, hud_cmd_source, h);

    *out = h;
    return FDK_OK;
}

void fdk_hud_show(fdk_hud *hud) {
    if (hud == NULL || hud->surf == NULL) {
        return;
    }
    fdk_ext_surface_show(hud->surf);
    hud_render(hud);
}

void fdk_hud_hide(fdk_hud *hud) {
    if (hud == NULL || hud->surf == NULL) {
        return;
    }
    hud_menu_close(hud);
    fdk_ext_surface_hide(hud->surf);
}

void fdk_hud_shutdown(fdk_hud *hud) {
    if (hud == NULL) {
        return;
    }
    hud_menu_close(hud);
    if (hud->cmd_src != NULL) {
        fdk_ext_loop_remove_fd(hud->cmd_src); /* our read end's watch */
        hud->cmd_src = NULL;
    }
    if (hud->pipe_fd >= 0) {
        close(hud->pipe_fd); /* ours; the write end is the host's */
        hud->pipe_fd = -1;
    }
    if (hud->tick != NULL) {
        fdk_timer_remove(hud->tick);
        hud->tick = NULL;
    }
    if (hud->surf != NULL) {
        fdk_ext_surface_destroy(hud->surf); /* kind destroy runs here */
        hud->surf = NULL;
    }
    if (hud->kind != NULL) {
        fdk_ext_kind_destroy(hud->kind);
        hud->kind = NULL;
    }
    fdk_font_destroy(hud->font_big);
    fdk_font_destroy(hud->font_small);
    hud->font_big = NULL;
    hud->font_small = NULL;
    hud_last_stats = hud->st; /* the receipt outlives the instance */
    free(hud);
}

void fdk_hud_set_value(fdk_hud *hud, float v01) {
    if (hud == NULL) {
        return;
    }
    hud->frozen = false;
    hud->target = v01 < 0.0f ? 0.0f : (v01 > 1.0f ? 1.0f : v01);
    hud_tick(NULL, hud); /* ease immediately; the timer continues */
}

void fdk_hud_set_label(fdk_hud *hud, const char *text) {
    if (hud == NULL) {
        return;
    }
    snprintf(hud->label, sizeof hud->label, "%s",
             text != NULL ? text : "");
    hud_render(hud);
}

void fdk_hud_show_menu(fdk_hud *hud) {
    if (hud == NULL || hud->surf == NULL) {
        return;
    }
    hud_menu_open(hud, 12, HUD_H - 12);
}

int fdk_hud_command_fd(fdk_hud *hud) {
    return hud != NULL ? hud->cmd_fd : -1;
}

fdk_hud_stats fdk_hud_stats_read(fdk_hud *hud) {
    if (hud == NULL) {
        return hud_last_stats; /* after shutdown: the final counters */
    }
    return hud->st;
}
