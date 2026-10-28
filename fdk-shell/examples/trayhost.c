/* trayhost.c — the fdk-shell StatusNotifier tray host: the reference
 * THIN UI over the desktop model (T-B side 2).
 *
 * The protocol half — the org.kde.StatusNotifierWatcher watcher
 * service, the item registry (both registration dialects), per-item
 * property/icon/menu state, the dbusmenu client, and the name
 * etiquette — lives in the no-drawing MODEL
 * (fdk-shell/src/desktop/tray_model.c, fdk-shell-desktop.h). This
 * example is the rendering half: it creates the model on its bus,
 * snapshots the model's item list, draws the floating tray strip and
 * the menu cascades, and forwards every user interaction (Activate /
 * SecondaryActivate / Scroll / the dbusmenu Event round) through the
 * model's action entry points. Written from the public SNI and
 * dbusmenu specification shapes — no tray host's code was read or
 * copied (the model's provenance header records the same).
 *
 * Layout: a floating strip anchored to a screen edge. Left-click =
 * Activate (or the menu, when ItemIsMenu); middle-click =
 * SecondaryActivate; right-click = the item's dbusmenu as a
 * right-aligned popup cascade. Passive items fade (alpha 0.45, never
 * hidden); NeedsAttention swaps the attention icon and marks the
 * cell with an accent strip. See docs/DECISIONS.md D-007 for the
 * shape decisions.
 *
 * Usage: trayhost [--bar-height N] [--cell N] [--font-size N]
 *                 [--margin N] [--anchor tr|tl|br|bl] [--output N]
 *                 [--replace | --wait] [--quit-after S]
 *
 * Diagnostics: FDK_TRAYHOST_TRACE=1 logs every state transition the
 * UI sees; the model's own transactions trace through FDK's log
 * channel. The rig: make -C fdk-shell test-tray.
 */
#include "fdk-shell/fdk-shell.h"
#include "fdk-shell/fdk-shell-desktop.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the UI metrics (TASK C) -----------------------------------------------
 *
 * Every dimension derives from the process's UI font size
 * (fdk_ui_font_px — theme/settings, the desktop's statement, default
 * 16), scaled off the 14 px baseline the strip was designed against,
 * so cells, menus, and rows grow together. --font-size pins it. */

typedef struct {
    int font_px;
    double k; /* font_px / 14 — the design baseline               */
    int bar_pad;        /* the strip's inner padding (3)           */
    int menu_row_h;     /* the row height floor (28)               */
    int menu_pad_x;     /* the menu's side padding (10)            */
    int menu_pad_y;     /* the menu's top/bottom padding (4)       */
    int menu_sep_h;     /* a separator row (9)                     */
    int menu_arrow_w;   /* the arrow column reserve (18)           */
    int menu_max_w;     /* the menu width cap (300)                */
    int glyph_col;      /* the state-glyph column (26)             */
    int row_gap;        /* line_height + this = row height (10)    */
} ui_metrics;

static ui_metrics g_ui;

static int sc(int base) {
    int v = (int)((double)base * g_ui.k + 0.5);
    return v < 1 ? 1 : v;
}

static void ui_init(int font_override_px) {
    int f = font_override_px > 0 ? font_override_px : fdk_ui_font_px();
    if (f < 8) {
        f = 8;
    }
    if (f > 96) {
        f = 96;
    }
    g_ui.font_px = f;
    g_ui.k = (double)f / 14.0;
    g_ui.bar_pad = sc(3);
    g_ui.menu_row_h = sc(28);
    g_ui.menu_pad_x = sc(10);
    g_ui.menu_pad_y = sc(4);
    g_ui.menu_sep_h = sc(9);
    g_ui.menu_arrow_w = sc(18);
    g_ui.menu_max_w = sc(300);
    g_ui.glyph_col = sc(26);
    g_ui.row_gap = sc(10);
}

/* Bar metrics (logical px). */
#define BAR_RADIUS 6
#define ICON_INSET 1        /* cell border around the drawn icon    */
#define MAX_MENU_DEPTH 8    /* cascade depth budget                 */

/* ---- logging ------------------------------------------------------------ */

static int g_trace; /* FDK_TRAYHOST_TRACE=1 */

static void tracef(const char *fmt, ...) {
    if (!g_trace) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fputs("trayhost[trace]: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static void sayf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("trayhost: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* ---- options ------------------------------------------------------------ */

typedef enum { ANCHOR_TR = 0, ANCHOR_TL, ANCHOR_BR, ANCHOR_BL } anchor_kind;

typedef struct {
    int bar_height;  /* the strip's height (0 = UI metrics) */
    int cell;        /* one item's cell (0 = UI metrics)    */
    int font_size;   /* px; 0 = the UI metrics              */
    int edge_margin; /* gap from the screen edges   */
    anchor_kind anchor;
    int output_index;    /* -1 = every output            */
    int replace;
    int wait;
    double quit_after_s; /* <=0 = run forever (rigs)     */
} options;

/* ---- the menu cascade (UI state over the model's cached tree) ------------- */

/* One row's snapshot of a menu node (the model's walk output, copied
 * so the rows stay valid while the tree behind them may be replaced —
 * the stale-safe contract's rendering half). */
typedef struct {
    int32_t id;
    char *label;      /* mnemonic-stripped copy, owned            */
    int depth;        /* the walk's depth (the level slicing key)  */
    int is_separator;
    int is_check;
    int is_radio;
    int toggle_state;
    int enabled;
    int has_children;
    int opened;       /* an "opened" Event was sent (teardown)     */
    int y, h;         /* the row's box in its level (at build)     */
} menu_row_info;

/* One cascade level's runtime state (the open menu's shape). */
typedef struct menu_level2 {
    struct menu_level2 *parent; /* NULL for the root level         */
    fdk_shell_popup *popup;
    menu_row_info *rows;    /* built below                        */
    int row_count;   /* rows (separators included)              */
    int width;
    int height;
    int hover;        /* -1 none, else row index               */
} menu_level2;

/* The open cascade's state. */
typedef struct {
    int item_index;  /* the model item the menu is open on       */
    uint32_t item_serial;
    menu_level2 *root;
} menu_state;

static menu_state g_menu;

/* The cascade's levels (index 0 = root) + each popup's creation x
 * (popups carry no query API; the global-position math needs the
 * chain of offsets). */
static menu_level2 *g_levels[MAX_MENU_DEPTH + 1];
static int g_level_count;
static int g_level_x[MAX_MENU_DEPTH + 1];

/* ---- app state ----------------------------------------------------------- */

typedef struct {
    fdk_context *ctx;
    fdk_shell *shell;
    fdk_dbus_connection *bus;
    fdk_shell_tray_host *tray; /* the model (the protocol half)     */
    fdk_font *font;

    options opt;

    fdk_shell_layer *bar;
    int bar_w, bar_h;
    int bar_x, bar_y;   /* the bar's screen origin, when known     */
    int have_bar_pos;   /* X11: computed from the anchoring math   */

    int serving;
    int stopping;
    int exit_code;
    const char *stop_reason;
} trayhost;

static trayhost g_app;

static void bar_relayout(trayhost *app);
static void bar_paint(trayhost *app);
static void bar_layer_event(fdk_shell_layer *layer,
                            const fdk_event_data *ev, void *user);
static void menu_popup_event(fdk_shell_popup *popup,
                              const fdk_event_data *ev, void *user);
static void menu_level_paint(menu_level2 *lvl);
static int menu_global_x(menu_level2 *lvl);
static void menu_close_all(trayhost *app);

/* ---- the model's events -> the UI's relayout ------------------------------ */

static void tray_event(fdk_shell_tray_host *host,
                       fdk_shell_tray_event_type ev, uint32_t serial,
                       void *user) {
    (void)host;
    (void)serial;
    trayhost *app = user;
    switch (ev) {
    case FDK_SHELL_TRAY_EV_ITEMS_CHANGED:
    case FDK_SHELL_TRAY_EV_ITEM_STATE:
        /* The set or one item's visible state: re-derive the strip. */
        bar_relayout(app);
        break;
    case FDK_SHELL_TRAY_EV_MENU_CHANGED:
        /* A refreshed tree arrived for a CLOSED menu (the model
         * discards mid-open refreshes — the stale-safe contract); the
         * next open walks the new tree. Nothing to do now. */
        break;
    case FDK_SHELL_TRAY_EV_HOST_STATE:
        /* The name etiquette's outcome, reported through the event
         * the model defines; serving() is the query. */
        if (fdk_shell_tray_host_serving(app->tray)) {
            app->serving = 1;
            sayf("serving org.kde.StatusNotifierWatcher as %s",
                 fdk_dbus_connection_unique_name(app->bus));
        } else {
            /* Lost, or refused before ever serving (the incumbent
             * watcher path or the RequestName-EXISTS path — a
             * stubborn incumbent beats even --replace). Both are
             * terminal for the reference host; --wait survives (its
             * refusal never comes through this event). */
            int was_serving = app->serving;
            if (was_serving) {
                sayf("another host took the name — handing over");
            } else if (app->opt.replace || app->opt.wait) {
                /* We ASKED (replace/wait) and RequestName said
                 * EXISTS: the incumbent refuses replacement. */
                sayf("org.kde.StatusNotifierWatcher is owned by a "
                     "host that refuses replacement; exiting");
            } else {
                sayf("org.kde.StatusNotifierWatcher is already owned; "
                     "exiting (--replace to take over, --wait to "
                     "queue)");
            }
            app->serving = 0;
            app->stopping = 1;
            app->exit_code = 0;
            app->stop_reason = was_serving
                                   ? "replaced by another host"
                                   : "name exists, replacement refused";
        }
        break;
    default:
        break;
    }
}

/* ---- item snapshots (the model's read APIs, UI-side helpers) --------------- */

/* The item list the bar draws (loaded, un-failed) — model snapshot. */
static int bar_drawable_count(trayhost *app) {
    uint32_t n = fdk_shell_tray_item_count(app->tray);
    int drawable = 0;
    for (uint32_t i = 0; i < n; i++) {
        fdk_shell_tray_item_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        if (!fdk_ok(fdk_shell_tray_item_snapshot(app->tray, i, &info))) {
            continue;
        }
        if (!info.loading && !info.failed) {
            drawable++;
        }
    }
    return drawable;
}

/* The model index of the idx-th drawable item. */
static int bar_item_model_index(trayhost *app, int idx) {
    uint32_t n = fdk_shell_tray_item_count(app->tray);
    int i = 0;
    for (uint32_t m = 0; m < n; m++) {
        fdk_shell_tray_item_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        if (!fdk_ok(fdk_shell_tray_item_snapshot(app->tray, m, &info))) {
            continue;
        }
        if (info.loading || info.failed) {
            continue;
        }
        if (i == idx) {
            return (int)m;
        }
        i++;
    }
    return -1;
}

/* A raw ARGB32 buffer -> an FDK surface (the model hands bytes; the
 * surface is the renderer's own object). */
static fdk_surface *argb_to_surface(const uint8_t *bytes, int w, int h) {
    if (bytes == NULL || w <= 0 || h <= 0) {
        return NULL;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_surface_create_format(w, h, FDK_SURFACE_FORMAT_ARGB8888,
                                          &s))) {
        return NULL;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        fdk_surface_destroy(s);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p;
            memcpy(&p, bytes + (size_t)y * (size_t)w * 4u +
                           (size_t)x * 4u, 4u);
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] = p;
        }
    }
    return s;
}

/* Chooses the icon surface for one item (the best-resolution pixmap
 * for the cell, or the themed name), honoring status. Owned by the
 * caller; NULL when the item has nothing drawable. */
static fdk_surface *item_icon_surface(trayhost *app, int model_index,
                                      fdk_shell_tray_item_status status,
                                      int cell) {
    int target = cell - 2 * ICON_INSET;
    /* Pick the best pixmap: closest-from-above on width, else the
     * largest available (the reference's rule). The ACTIVE list is
     * scanned (attention when the status says so). */
    int attention = status == FDK_SHELL_TRAY_NEEDS_ATTENTION;
    uint32_t best = 0;
    int best_w = -1;
    int found = 0;
    for (uint32_t pi = 0;; pi++) {
        int w = 0, h = 0;
        if (!fdk_ok(fdk_shell_tray_item_pixmap_size(
                app->tray, (uint32_t)model_index, attention, pi, &w,
                &h))) {
            break;
        }
        if (!found) {
            found = 1;
            best = pi;
            best_w = w;
            continue;
        }
        int bi = best_w - target;
        int ci = w - target;
        if (ci >= 0 && (bi < 0 || ci < bi)) {
            best = pi; /* closest from above wins                     */
            best_w = w;
        } else if (bi < 0 && ci > bi) {
            best = pi; /* else the largest available                  */
            best_w = w;
        }
    }
    if (found) {
        size_t need = 0;
        if (!fdk_ok(fdk_shell_tray_item_pixmap_pixels(
                app->tray, (uint32_t)model_index, attention, best, NULL,
                0, &need)) ||
            need == 0 || best_w <= 0) {
            return NULL;
        }
        uint8_t *bytes = malloc(need);
        if (bytes == NULL) {
            return NULL;
        }
        fdk_surface *s = NULL;
        if (fdk_ok(fdk_shell_tray_item_pixmap_pixels(
                app->tray, (uint32_t)model_index, attention, best,
                bytes, need, NULL))) {
            s = argb_to_surface(bytes, best_w,
                                (int)(need / 4u / (size_t)best_w));
        }
        free(bytes);
        if (s != NULL) {
            return s;
        }
    }
    /* The themed-name fallback. */
    fdk_shell_tray_item_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (fdk_ok(fdk_shell_tray_item_snapshot(app->tray,
                                            (uint32_t)model_index,
                                            &info))) {
        char attn[256], name[256];
        size_t alen = sizeof(attn), nlen = sizeof(name);
        info.attention_icon_name = attn;
        info.attention_icon_name_len = alen;
        info.icon_name = name;
        info.icon_name_len = nlen;
        fdk_shell_tray_item_snapshot(app->tray, (uint32_t)model_index,
                                     &info);
        const char *pick = NULL;
        if (status == FDK_SHELL_TRAY_NEEDS_ATTENTION && attn[0] != '\0') {
            pick = attn;
        } else if (name[0] != '\0') {
            pick = name;
        }
        if (pick != NULL) {
            return fdk_icon_theme_surface(pick, (fdk_i32)target);
        }
    }
    return NULL;
}

/* ---- bar rendering --------------------------------------------------------- */

static fdk_color tk_color(fdk_theme_token token, fdk_color fallback) {
    fdk_color c = fdk_theme_get_color(NULL, token);
    if (c.a <= 0.0f) {
        return fallback;
    }
    return c;
}

/* The screen size for position math (the anchoring rules are ours,
 * so the bar's origin is computable — X11 truth; Wayland has no
 * global space, documented in D-007). */
static void bar_screen_size(trayhost *app, int *w, int *h) {
    *w = 1024;
    *h = 768;
    uint32_t count = 0;
    if (fdk_ok(fdk_shell_output_count(app->shell, &count)) &&
        count > 0) {
        uint32_t idx = 0;
        if (app->opt.output_index >= 0 &&
            (uint32_t)app->opt.output_index < count) {
            idx = (uint32_t)app->opt.output_index;
        }
        fdk_shell_output_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        if (fdk_ok(fdk_shell_output_get_info(app->shell, idx, &info)) &&
            info.width > 0 && info.height > 0) {
            *w = info.width;
            *h = info.height;
        }
    }
}

/* (Re)creates the bar layer when its WIDTH changes (the destroy +
 * recreate idiom — the only documented way to resize a layer). */
static void bar_relayout(trayhost *app) {
    int count = app->tray != NULL ? bar_drawable_count(app) : 0;
    int want_w = 2 * g_ui.bar_pad + count * app->opt.cell;
    if (want_w < 2 * g_ui.bar_pad) {
        want_w = 2 * g_ui.bar_pad; /* the empty strip keeps a toe-hold */
    }
    if (app->bar != NULL && app->bar_w == want_w) {
        bar_paint(app);
        return;
    }
    if (app->bar != NULL) {
        menu_close_all(app); /* the cascade parents to the bar       */
        fdk_shell_layer_destroy(app->bar);
        app->bar = NULL;
    }
    fdk_shell_layer_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.layer = FDK_SHELL_LAYER_TOP;
    switch (app->opt.anchor) {
    case ANCHOR_TR:
        d.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_RIGHT;
        d.margin_top = app->opt.edge_margin;
        d.margin_right = app->opt.edge_margin;
        break;
    case ANCHOR_TL:
        d.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT;
        d.margin_top = app->opt.edge_margin;
        d.margin_left = app->opt.edge_margin;
        break;
    case ANCHOR_BR:
        d.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_RIGHT;
        d.margin_bottom = app->opt.edge_margin;
        d.margin_right = app->opt.edge_margin;
        break;
    case ANCHOR_BL:
    default:
        d.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_LEFT;
        d.margin_bottom = app->opt.edge_margin;
        d.margin_left = app->opt.edge_margin;
        break;
    }
    d.exclusive_zone = 0; /* a floating tray reserves nothing        */
    d.keyboard = FDK_SHELL_KBD_NONE;
    d.width = want_w;
    d.height = app->opt.bar_height;
    d.output_index = app->opt.output_index;
    snprintf(d.namespace_, sizeof(d.namespace_), "trayhost");

    fdk_shell_layer *layer = NULL;
    fdk_result r = fdk_shell_layer_create(app->shell, &d, &layer);
    if (!fdk_ok(r)) {
        tracef("bar layer create failed (%d)", r);
        return;
    }
    app->bar = layer;
    app->bar_w = want_w;
    app->bar_h = app->opt.bar_height;
    fdk_shell_layer_set_event_callback(layer, NULL, NULL); /* set below */
    fdk_shell_layer_set_event_callback(layer, bar_layer_event, app);
    fdk_shell_layer_show(layer);
    bar_paint(app);

    /* The anchoring math mirrors the X11 backend's (top-right:
     * screen - margin - extent) — the Activate coordinates are sent
     * in SCREEN space where that is knowable. */
    int sw = 0, sh = 0;
    bar_screen_size(app, &sw, &sh);
    (void)sh;
    app->have_bar_pos = 0;
    if (app->opt.anchor == ANCHOR_TR) {
        app->bar_x = sw - app->opt.edge_margin - want_w;
        app->bar_y = app->opt.edge_margin;
        app->have_bar_pos = 1;
    } else if (app->opt.anchor == ANCHOR_TL) {
        app->bar_x = app->opt.edge_margin;
        app->bar_y = app->opt.edge_margin;
        app->have_bar_pos = 1;
    }
    tracef("bar relayout: %d items, %dx%d at (%d,%d)", count, want_w,
           app->bar_h, app->have_bar_pos ? app->bar_x : -1,
           app->have_bar_pos ? app->bar_y : -1);
}

/* Paints one icon cell into the bar surface, faded for Passive. */
static void bar_paint_cell(trayhost *app, fdk_surface *s, int model_index,
                           fdk_shell_tray_item_status status, int cell_x) {
    int cell = app->opt.cell;
    fdk_surface *icon =
        item_icon_surface(app, model_index, status, cell);
    if (icon == NULL) {
        return;
    }
    fdk_surface_info ii;
    if (!fdk_ok(fdk_surface_get_info(icon, &ii))) {
        fdk_surface_destroy(icon);
        return;
    }
    int box = cell - 2 * ICON_INSET;
    double scale = 1.0;
    if (ii.width > box || ii.height > box) {
        scale = (double)box / (double)(ii.width > ii.height ? ii.width
                                                            : ii.height);
    }
    int dw = (int)(ii.width * scale + 0.5);
    int dh = (int)(ii.height * scale + 0.5);
    int dx = cell_x + (cell - dw) / 2;
    int dy = (app->bar_h - dh) / 2;

    if (status != FDK_SHELL_TRAY_PASSIVE) {
        fdk_matrix m = { .m00 = (float)scale, .m01 = 0.0f, .m10 = 0.0f,
                         .m11 = (float)scale, .tx = (float)dx,
                         .ty = (float)dy };
        fdk_surface_blit_transformed(s, m, icon);
    } else {
        /* Passive: the icon drawn at alpha 0.45 — faded, NEVER
         * hidden (the honest middle; D-007). The fade is applied by
         * scaling the icon's own alpha channel on a scratch surface
         * (the transform blit has no alpha knob). */
        fdk_surface *scratch = NULL;
        if (fdk_ok(fdk_surface_create_format(
                ii.width, ii.height, FDK_SURFACE_FORMAT_ARGB8888,
                &scratch))) {
            fdk_matrix m = { .m00 = 1.0f, .m01 = 0.0f, .m10 = 0.0f,
                             .m11 = 1.0f, .tx = 0.0f, .ty = 0.0f };
            fdk_surface_blit_transformed(scratch, m, icon);
            fdk_surface_info si;
            if (fdk_ok(fdk_surface_get_info(scratch, &si))) {
                for (int y = 0; y < si.height; y++) {
                    for (int x = 0; x < si.width; x++) {
                        uint32_t p =
                            si.pixels[(size_t)y * (size_t)si.stride +
                                      (size_t)x];
                        uint32_t a = (p >> 24) & 0xFFu;
                        a = (a * 45u) / 100u;
                        si.pixels[(size_t)y * (size_t)si.stride +
                                  (size_t)x] =
                            (p & 0x00FFFFFFu) | (a << 24);
                    }
                }
            }
            fdk_matrix m2 = { .m00 = (float)scale, .m01 = 0.0f,
                              .m10 = 0.0f, .m11 = (float)scale,
                              .tx = (float)dx, .ty = (float)dy };
            fdk_surface_blit_transformed(s, m2, scratch);
            fdk_surface_destroy(scratch);
        }
    }
    fdk_surface_destroy(icon);

    if (status == FDK_SHELL_TRAY_NEEDS_ATTENTION) {
        /* The attention marker: an accent strip under the cell.     */
        fdk_color accent = tk_color(
            FDK_TK_ACCENT,
            (fdk_color){ .r = 0.56f, .g = 0.47f, .b = 0.85f, .a = 1.0f });
        fdk_surface_fill_rect(
            s, (fdk_rect){ .x = cell_x + 3,
                           .y = app->bar_h - 4,
                           .width = app->opt.cell - 2 * g_ui.bar_pad,
                           .height = 3 },
            accent);
    }
}

static void bar_paint(trayhost *app) {
    if (app->bar == NULL || app->tray == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_layer_get_surface(app->bar, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    int w = info.width;
    int h = info.height;

    fdk_color bg = tk_color(
        FDK_TK_TOOLTIP_BACKGROUND,
        (fdk_color){ .r = 0.10f, .g = 0.11f, .b = 0.14f, .a = 1.0f });
    fdk_surface_fill_rounded_rect(
        s, (fdk_rect){ .x = 0, .y = 0, .width = w, .height = h },
        BAR_RADIUS, bg);

    int i = 0;
    uint32_t n = fdk_shell_tray_item_count(app->tray);
    for (uint32_t m = 0; m < n; m++) {
        fdk_shell_tray_item_info it;
        memset(&it, 0, sizeof(it));
        it.struct_size = sizeof(it);
        if (!fdk_ok(fdk_shell_tray_item_snapshot(app->tray, m, &it))) {
            continue;
        }
        if (it.loading || it.failed) {
            continue;
        }
        bar_paint_cell(app, s, (int)m, it.status,
                       g_ui.bar_pad + i * app->opt.cell);
        i++;
    }
    (void)fdk_surface_present(s);
}

/* ---- the menu cascade (built from the model's tree walk) ------------------- */

/* The walk collector: one flat array in preorder with depths, from
 * which each level's row list is carved. */
typedef struct {
    menu_row_info *nodes;
    int count;
    int cap;
} walk_collector;

static void walk_collect(const fdk_shell_tray_menu_node_info *node,
                         int depth, void *user) {
    walk_collector *wc = user;
    if (wc->count >= wc->cap) {
        return;
    }
    menu_row_info *r = &wc->nodes[wc->count++];
    r->id = node->id;
    r->label = node->label != NULL ? strdup(node->label) : NULL;
    r->depth = depth;
    r->is_separator = node->is_separator;
    r->is_check = node->is_check;
    r->is_radio = node->is_radio;
    r->toggle_state = node->toggle_state;
    r->enabled = node->enabled;
    r->has_children = node->has_children;
    r->opened = 0;
    r->y = 0;
    r->h = 0;
}

/* Builds the rows for one level from a SOURCE array of already-
 * selected row infos (owned by the caller; the labels are re-copied
 * so the level survives the caller's array). */
static menu_level2 *menu_level_build(trayhost *app,
                                     const menu_row_info *src, int count,
                                     menu_level2 *parent) {
    menu_level2 *lvl = calloc(1, sizeof(*lvl));
    if (lvl == NULL) {
        return NULL;
    }
    lvl->parent = parent;
    lvl->hover = -1;

    fdk_font_metrics fm;
    fdk_font_get_metrics(app->font, &fm);
    int row_h = fm.line_height + g_ui.row_gap;
    if (row_h < g_ui.menu_row_h) {
        row_h = g_ui.menu_row_h;
    }

    lvl->rows = calloc((size_t)(count > 0 ? count : 1), sizeof(*lvl->rows));
    if (lvl->rows == NULL) {
        free(lvl);
        return NULL;
    }
    int width = 140;
    int y = g_ui.menu_pad_y;
    for (int i = 0; i < count; i++) {
        const menu_row_info *nd = &src[i];
        int rh = nd->is_separator ? g_ui.menu_sep_h : row_h;
        lvl->rows[i] = *nd; /* the snapshot copy                     */
        lvl->rows[i].label =
            nd->label != NULL ? strdup(nd->label) : NULL;
        lvl->rows[i].opened = 0;
        lvl->rows[i].y = y;
        lvl->rows[i].h = rh;
        if (!nd->is_separator && nd->label != NULL) {
            fdk_text_metrics tm;
            fdk_font_measure_utf8(app->font, nd->label,
                                  strlen(nd->label), &tm);
            int need = g_ui.menu_pad_x + g_ui.glyph_col +
                       g_ui.menu_pad_x + tm.advance_width +
                       g_ui.menu_arrow_w + g_ui.menu_pad_x;
            if (need > width) {
                width = need;
            }
        }
        y += rh;
    }
    lvl->row_count = count;
    lvl->width = width > g_ui.menu_max_w ? g_ui.menu_max_w : width;
    lvl->height = y + g_ui.menu_pad_y;
    return lvl;
}

/* Frees a level's rows (the labels are owned copies). */
static void menu_level_free(menu_level2 *lvl) {
    if (lvl == NULL) {
        return;
    }
    for (int i = 0; i < lvl->row_count; i++) {
        free(lvl->rows[i].label);
    }
    free(lvl->rows);
    free(lvl);
}

/* Frees the collector's nodes. */
static void walk_collect_free(walk_collector *wc) {
    for (int i = 0; i < wc->count; i++) {
        free(wc->nodes[i].label);
    }
    free(wc->nodes);
    wc->nodes = NULL;
    wc->count = 0;
}

/* The flat walk of the item's cached tree (the model's API); the
 * array is owned by the caller through walk_collect_free. Returns 0
 * on an empty/absent tree. */
static int menu_walk_flat(trayhost *app, int model_index,
                          walk_collector *wc) {
    memset(wc, 0, sizeof(*wc));
    wc->cap = 256;
    wc->nodes = calloc((size_t)wc->cap, sizeof(*wc->nodes));
    if (wc->nodes == NULL) {
        wc->cap = 0;
        return 0;
    }
    /* fdk_shell_tray_menu_walk returns NOT_FOUND for an absent
     * cache — the walk collector simply stays empty (an honest
     * empty menu; the next event brings the tree). */
    (void)fdk_shell_tray_menu_walk(app->tray, (uint32_t)model_index,
                                   walk_collect, wc);
    return wc->count > 0;
}

/* Closes levels from `index` down: ONE popup destroy (the backend's
 * family sweep takes the native children — destroying them again
 * would double-free), then the wrappers free by hand. The "closed"
 * Events for submenus this cascade had opened ride the model. */
static void menu_close_from(trayhost *app, int index) {
    if (g_level_count <= index) {
        return;
    }
    for (int i = index; i < g_level_count; i++) {
        if (g_levels[i] != NULL) {
            if (i == index && g_levels[i]->popup != NULL) {
                fdk_shell_popup_destroy(g_levels[i]->popup);
                g_levels[i]->popup = NULL;
            }
            /* Send "closed" for submenus this level had opened. */
            for (int r = 0; r < g_levels[i]->row_count; r++) {
                menu_row_info *nd = &g_levels[i]->rows[r];
                if (nd != NULL && nd->opened) {
                    nd->opened = 0;
                    fdk_shell_tray_menu_event(app->tray,
                                              (uint32_t)g_menu.item_index,
                                              nd->id, "closed");
                }
            }
            menu_level_free(g_levels[i]);
            g_levels[i] = NULL;
        }
    }
    g_level_count = index;
    if (g_level_count == 0) {
        if (g_menu.item_index >= 0) {
            fdk_shell_tray_menu_close(app->tray,
                                      (uint32_t)g_menu.item_index);
        }
        g_menu.item_index = -1;
        g_menu.item_serial = 0;
        g_menu.root = NULL;
    }
}

static void menu_close_all(trayhost *app) {
    menu_close_from(app, 0);
}

/* The root open: right-click on an item's cell. The rows are the
 * ROOT NODE'S CHILDREN — the walk's depth-1 nodes, FILTERED (the
 * preorder interleaves each depth-1 node with its own subtree).
 * The walk's root node itself (depth 0) is the menu container, not
 * a row. */
static void menu_open_root(trayhost *app, int model_index, int cell_index) {
    if (app->tray == NULL || model_index < 0) {
        return;
    }
    walk_collector wc;
    if (!menu_walk_flat(app, model_index, &wc)) {
        walk_collect_free(&wc);
        return;
    }
    /* The depth-1 filter. */
    int n1 = 0;
    for (int i = 0; i < wc.count; i++) {
        if (wc.nodes[i].depth == 1) {
            n1++;
        }
    }
    menu_row_info *src = NULL;
    if (n1 > 0) {
        src = calloc((size_t)n1, sizeof(*src));
    }
    if (src == NULL) {
        walk_collect_free(&wc);
        return;
    }
    int k = 0;
    for (int i = 0; i < wc.count; i++) {
        if (wc.nodes[i].depth == 1) {
            src[k++] = wc.nodes[i];
            src[k - 1].label =
                wc.nodes[i].label != NULL ? strdup(wc.nodes[i].label)
                                          : NULL;
        }
    }
    walk_collect_free(&wc);
    menu_level2 *lvl = menu_level_build(app, src, n1, NULL);
    for (int i = 0; i < n1; i++) {
        free(src[i].label);
    }
    free(src);
    if (lvl == NULL || lvl->row_count == 0) {
        menu_level_free(lvl);
        return;
    }
    menu_close_all(app);

    /* Tell the model the menu is open (AboutToShow + the open-path
     * refresh; mid-open refreshes stay stale-safe model-side). */
    fdk_shell_tray_menu_open(app->tray, (uint32_t)model_index);

    /* Right-aligned under the item's cell. The popup's x is
     * LAYER-LOCAL, and the menu is WIDER than the bar: x goes
     * NEGATIVE (the menu hangs left of the bar's right edge — the
     * shape every right-edge tray menu has). Clamped only so the
     * global right edge stays on screen (when the bar origin is
     * knowable; Wayland's compositor clamps for us). */
    int cell_x = g_ui.bar_pad + cell_index * app->opt.cell;
    int x = cell_x + app->opt.cell - lvl->width;
    if (app->have_bar_pos) {
        int sw = 0, sh = 0;
        bar_screen_size(app, &sw, &sh);
        (void)sh;
        int max_x = sw - app->opt.edge_margin - app->bar_x - lvl->width;
        if (x > max_x) {
            x = max_x;
        }
    }
    fdk_result r = fdk_shell_popup_create(
        app->bar, x, app->bar_h, lvl->width, lvl->height, &lvl->popup);
    if (!fdk_ok(r)) {
        tracef("menu popup create failed (%d)", r);
        fdk_shell_tray_menu_close(app->tray, (uint32_t)model_index);
        menu_level_free(lvl);
        return;
    }
    fdk_shell_tray_item_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    uint32_t serial = 0;
    if (fdk_ok(fdk_shell_tray_item_snapshot(app->tray,
                                            (uint32_t)model_index,
                                            &info))) {
        serial = info.serial;
    }
    g_menu.item_index = model_index;
    g_menu.item_serial = serial;
    g_menu.root = lvl;
    g_levels[0] = lvl;
    g_level_count = 1;
    g_level_x[0] = x;
    fdk_shell_popup_set_event_callback(lvl->popup, menu_popup_event,
                                       NULL);
    fdk_shell_popup_show(lvl->popup);
    menu_level_paint(lvl);
    tracef("menu open (root %dx%d at cell %d)", lvl->width, lvl->height,
           cell_index);
}

/* Opens a submenu level for one row. The children of the row's node
 * are re-derived by a fresh walk: find the node by id, then take
 * the nodes that follow it at exactly one depth deeper, until the
 * depth falls back to the node's own level. */
static void menu_open_child(trayhost *app, menu_level2 *parent,
                            int row_index) {
    menu_row_info *nd = &parent->rows[row_index];
    if (nd == NULL || !nd->has_children || app->tray == NULL) {
        return;
    }
    /* Close any deeper level first (a sibling submenu was open). */
    for (int i = 0; i < g_level_count; i++) {
        if (g_levels[i] == parent) {
            menu_close_from(app, i + 1);
            break;
        }
    }
    if (g_level_count >= MAX_MENU_DEPTH) {
        return;
    }
    /* Walk fresh, find this node, carve its child run. */
    walk_collector wc;
    if (!menu_walk_flat(app, g_menu.item_index, &wc)) {
        walk_collect_free(&wc);
        return;
    }
    int at = -1;
    for (int i = 0; i < wc.count; i++) {
        if (wc.nodes[i].id == nd->id) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        walk_collect_free(&wc);
        return;
    }
    int node_depth = wc.nodes[at].depth;
    int child_depth = node_depth + 1;
    /* The child run: following nodes deeper than the node, keeping
     * those AT child_depth, until the depth falls back to the
     * node's own level (a next sibling) or shallower. */
    int n2 = 0;
    for (int i = at + 1;
         i < wc.count && wc.nodes[i].depth > node_depth; i++) {
        if (wc.nodes[i].depth == child_depth) {
            n2++;
        }
    }
    menu_row_info *src = NULL;
    if (n2 > 0) {
        src = calloc((size_t)n2, sizeof(*src));
    }
    if (src == NULL) {
        walk_collect_free(&wc);
        return;
    }
    int k = 0;
    for (int i = at + 1;
         i < wc.count && wc.nodes[i].depth > node_depth; i++) {
        if (wc.nodes[i].depth == child_depth) {
            src[k++] = wc.nodes[i];
            src[k - 1].label =
                wc.nodes[i].label != NULL ? strdup(wc.nodes[i].label)
                                          : NULL;
        }
    }
    walk_collect_free(&wc);
    menu_level2 *lvl = menu_level_build(app, src, n2, parent);
    for (int i = 0; i < n2; i++) {
        free(src[i].label);
    }
    free(src);
    if (lvl == NULL || lvl->row_count == 0) {
        menu_level_free(lvl);
        return;
    }
    /* Position: at the parent's right edge, flipped when the child
     * would cross the screen's right margin. */
    int px = 0, py = parent->rows[row_index].y;
    int sw = 0, sh = 0;
    bar_screen_size(app, &sw, &sh);
    (void)sh;
    /* The parent's GLOBAL x: bar_x + (each level's local x chain). */
    int gx = app->have_bar_pos ? app->bar_x : 0;
    gx += menu_global_x(parent);
    if (gx + parent->width + lvl->width + 8 > sw) {
        px = -lvl->width; /* flip LEFT of the parent                  */
        tracef("submenu flip: parent at %d, child %d wide, screen %d",
               gx, lvl->width, sw);
    } else {
        px = parent->width - 2;
    }
    fdk_result r = fdk_shell_popup_create_child(
        parent->popup, px, py, lvl->width, lvl->height, &lvl->popup);
    if (!fdk_ok(r)) {
        tracef("child popup create failed (%d)", r);
        menu_level_free(lvl);
        return;
    }
    g_levels[g_level_count] = lvl;
    g_level_count++;
    fdk_shell_popup_set_event_callback(lvl->popup, menu_popup_event,
                                       NULL);
    fdk_shell_popup_show(lvl->popup);
    menu_level_paint(lvl);
    fdk_shell_tray_menu_event(app->tray, (uint32_t)g_menu.item_index,
                              nd->id, "opened");
    nd->opened = 1;
}

static int menu_global_x(menu_level2 *lvl) {
    int x = 0;
    for (int i = 0; i < g_level_count; i++) {
        x += g_level_x[i];
        if (g_levels[i] == lvl) {
            break;
        }
    }
    return x;
}

/* ---- menu painting --------------------------------------------------------- */

static void menu_level_paint(menu_level2 *lvl) {
    if (lvl == NULL || lvl->popup == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_popup_get_surface(lvl->popup, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    int w = info.width;
    int h = info.height;

    fdk_color bg = tk_color(
        FDK_TK_TOOLTIP_BACKGROUND,
        (fdk_color){ .r = 0.10f, .g = 0.11f, .b = 0.14f, .a = 1.0f });
    fdk_color fg = tk_color(
        FDK_TK_TOOLTIP_TEXT,
        (fdk_color){ .r = 0.95f, .g = 0.95f, .b = 0.95f, .a = 1.0f });
    fdk_color hover = tk_color(
        FDK_TK_CONTROL_BACKGROUND_HOVER,
        (fdk_color){ .r = 0.26f, .g = 0.29f, .b = 0.36f, .a = 1.0f });
    fdk_color accent = tk_color(
        FDK_TK_ACCENT,
        (fdk_color){ .r = 0.56f, .g = 0.47f, .b = 0.85f, .a = 1.0f });

    fdk_surface_fill_rounded_rect(
        s, (fdk_rect){ .x = 0, .y = 0, .width = w, .height = h }, 6, bg);

    fdk_font_metrics fm;
    fdk_font_get_metrics(g_app.font, &fm);

    for (int i = 0; i < lvl->row_count; i++) {
        menu_row_info *row = &lvl->rows[i];
        if (row->is_separator) {
            /* The separator: a centered hairline. */
            fdk_color line = fg;
            line.a = 0.22f;
            fdk_surface_fill_rect(
                s,
                (fdk_rect){ .x = g_ui.menu_pad_x,
                            .y = row->y + row->h / 2,
                            .width = w - 2 * g_ui.menu_pad_x,
                            .height = 1 },
                line);
            continue;
        }
        if (i == lvl->hover) {
            fdk_surface_fill_rect(
                s, (fdk_rect){ .x = 2, .y = row->y, .width = w - 4,
                               .height = row->h },
                hover);
        }
        int ty = row->y + (row->h - fm.line_height) / 2 + fm.ascent;
        int tx = g_ui.menu_pad_x + sc(22);
        fdk_color text = fg;
        if (!row->enabled) {
            text.a = 0.45f;
        }
        /* The state glyph column (checkmark / radio). */
        if (row->is_check && row->toggle_state) {
            fdk_surface_draw_utf8(s, g_app.font, "\xE2\x9C\x93", 3,
                                  g_ui.menu_pad_x, ty, accent);
        } else if (row->is_radio) {
            fdk_surface_draw_utf8(
                s, g_app.font,
                row->toggle_state ? "\xE2\x97\x8F" : "\xE2\x97\x8B", 3,
                g_ui.menu_pad_x, ty, row->toggle_state ? accent : text);
        }
        if (row->label != NULL) {
            fdk_surface_draw_utf8(s, g_app.font, row->label,
                                  strlen(row->label), tx, ty, text);
        }
        /* The submenu arrow at the right edge. */
        if (row->has_children) {
            fdk_color arrow = fg;
            arrow.a = row->enabled ? 0.8f : 0.35f;
            fdk_surface_draw_utf8(s, g_app.font, "\xE2\x96\xB8", 3,
                                  w - g_ui.menu_pad_x - sc(10), ty,
                                  arrow);
        }
    }
    (void)fdk_surface_present(s);
}

/* ---- pointer interaction ----------------------------------------------------- */

/* The row index at popup-local y (or -1). */
static int menu_row_at(menu_level2 *lvl, float y) {
    for (int i = 0; i < lvl->row_count; i++) {
        if (y >= (float)lvl->rows[i].y &&
            y < (float)(lvl->rows[i].y + lvl->rows[i].h)) {
            return i;
        }
    }
    return -1;
}

static void menu_popup_event(fdk_shell_popup *popup,
                             const fdk_event_data *ev, void *user) {
    (void)popup;
    (void)user;
    trayhost *app = &g_app;
    /* Which level does this popup belong to? */
    menu_level2 *lvl = NULL;
    int lvl_index = -1;
    for (int i = 0; i < g_level_count; i++) {
        if (g_levels[i] != NULL && g_levels[i]->popup == popup) {
            lvl = g_levels[i];
            lvl_index = i;
            break;
        }
    }
    if (lvl == NULL) {
        return; /* a stale popup's event after teardown            */
    }
    switch (ev->type) {
    case FDK_EVENT_WINDOW_EXPOSE:
        menu_level_paint(lvl);
        break;
    case FDK_EVENT_POINTER_MOTION:
    case FDK_EVENT_POINTER_ENTER: {
        int row = menu_row_at(lvl, ev->pointer.position.y);
        int clickable =
            row >= 0 && !lvl->rows[row].is_separator &&
            lvl->rows[row].enabled;
        int new_hover = clickable ? row : -1;
        if (new_hover != lvl->hover) {
            lvl->hover = new_hover;
            fdk_shell_popup_set_cursor(
                popup, new_hover >= 0 ? FDK_SHELL_CURSOR_POINTER
                                      : FDK_SHELL_CURSOR_DEFAULT);
            menu_level_paint(lvl);
            if (new_hover >= 0 && lvl->rows[new_hover].has_children) {
                menu_open_child(app, lvl, new_hover);
            }
        }
        break;
    }
    case FDK_EVENT_POINTER_LEAVE:
        if (lvl->hover != -1) {
            lvl->hover = -1;
            menu_level_paint(lvl);
        }
        break;
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        break;
    case FDK_EVENT_POINTER_BUTTON_UP:
        if (ev->pointer_button.button == FDK_POINTER_BUTTON_LEFT &&
            lvl->hover >= 0) {
            menu_row_info *nd = &lvl->rows[lvl->hover];
            if (nd != NULL && nd->enabled && !nd->has_children) {
                fdk_shell_tray_menu_event(
                    app->tray, (uint32_t)g_menu.item_index, nd->id,
                    "clicked");
                menu_close_all(app); /* a choice was made             */
            }
        }
        break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        /* Outside click or Escape. On a CHILD level: close it and
         * its descendants and re-arm the parent's grab (the child's
         * death released the server grab — the hide/show cycle
         * re-takes it, the documented popup idiom). On the ROOT:
         * the whole family goes. */
        if (lvl_index == 0) {
            menu_close_all(app);
        } else {
            menu_level2 *parent = lvl->parent;
            menu_close_from(app, lvl_index);
            if (parent != NULL && parent->popup != NULL) {
                fdk_shell_popup_hide(parent->popup);
                fdk_shell_popup_show(parent->popup);
                menu_level_paint(parent);
            }
        }
        break;
    default:
        break;
    }
}

static void bar_layer_event(fdk_shell_layer *layer,
                            const fdk_event_data *ev, void *user) {
    trayhost *app = user;
    (void)layer;
    switch (ev->type) {
    case FDK_EVENT_WINDOW_CONFIGURE:
        bar_paint(app);
        break;
    case FDK_EVENT_WINDOW_EXPOSE:
        bar_paint(app); /* the ghost-content contract                */
        break;
    case FDK_EVENT_POINTER_MOTION:
    case FDK_EVENT_POINTER_ENTER:
    case FDK_EVENT_POINTER_LEAVE:
        break;
    case FDK_EVENT_POINTER_BUTTON_UP: {
        int cell = app->opt.cell;
        int idx = ((int)ev->pointer_button.position.x - g_ui.bar_pad) /
                  cell;
        int in_cell =
            (int)ev->pointer_button.position.x >= g_ui.bar_pad &&
            idx >= 0 && idx < (app->tray != NULL ? bar_drawable_count(app)
                                                 : 0);
        if (!in_cell) {
            break;
        }
        int model_index = bar_item_model_index(app, idx);
        if (model_index < 0) {
            break;
        }
        /* ItemIsMenu decides the click's meaning. */
        int item_is_menu = 0;
        fdk_shell_tray_item_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        if (fdk_ok(fdk_shell_tray_item_snapshot(app->tray,
                                                (uint32_t)model_index,
                                                &info))) {
            item_is_menu = info.item_is_menu;
        }
        /* The click position in SCREEN coordinates, where the
         * anchoring math makes those knowable (X11; D-007 records
         * the Wayland truth: no global space, the layer-local
         * coordinates carry). */
        int gx = (int)ev->pointer_button.position.x;
        int gy = (int)ev->pointer_button.position.y;
        if (app->have_bar_pos) {
            gx += app->bar_x;
            gy += app->bar_y;
        }
        if (ev->pointer_button.button == FDK_POINTER_BUTTON_LEFT) {
            if (g_level_count > 0 &&
                g_menu.item_index == model_index) {
                menu_close_all(app); /* a left-click toggles          */
                break;
            }
            if (item_is_menu) {
                menu_open_root(app, model_index, idx);
            } else {
                fdk_shell_tray_item_activate(
                    app->tray, (uint32_t)model_index, gx, gy);
            }
        } else if (ev->pointer_button.button ==
                   FDK_POINTER_BUTTON_RIGHT) {
            if (g_level_count > 0 &&
                g_menu.item_index == model_index) {
                menu_close_all(app); /* right-click toggles too       */
            } else {
                menu_open_root(app, model_index, idx);
            }
        } else if (ev->pointer_button.button ==
                   FDK_POINTER_BUTTON_MIDDLE) {
            if (item_is_menu) {
                menu_open_root(app, model_index, idx);
            } else {
                fdk_shell_tray_item_secondary_activate(
                    app->tray, (uint32_t)model_index, gx, gy);
            }
        }
        break;
    }
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        /* The bar's layer was closed by the environment (an output
         * going away): the layer handle is stale; the next relayout
         * recreates it. */
        app->bar = NULL;
        app->bar_w = 0;
        bar_relayout(app);
        break;
    default:
        break;
    }
}

/* ---- bus lifecycle ----------------------------------------------------------- */

static void bus_ready(fdk_dbus_connection *bus, int ok, const char *error,
                      void *user) {
    trayhost *app = user;
    if (!ok) {
        sayf("session bus connection failed (%s)", error ? error : "?");
        app->stopping = 1;
        app->exit_code = 1;
        app->stop_reason = "bus connect failed";
        return;
    }
    tracef("bus ready as %s", fdk_dbus_connection_unique_name(bus));
    /* The MODEL owns the protocol from here (the watcher surface,
     * the name etiquette, the item registry, the dbusmenu client). */
    fdk_shell_tray_host_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.event_cb = tray_event;
    desc.user = app;
    desc.replace = app->opt.replace;
    desc.wait = app->opt.wait;
    fdk_result r =
        fdk_shell_tray_host_create(bus, &desc, &app->tray);
    if (!fdk_ok(r)) {
        sayf("the tray model could not start (%d)", r);
        app->stopping = 1;
        app->exit_code = 1;
        app->stop_reason = "tray model create failed";
    }
}

static void bus_died(fdk_dbus_connection *bus, void *user) {
    (void)bus;
    trayhost *app = user;
    sayf("the session bus died — exiting (a supervisor should "
         "restart trayhost)");
    app->bus = NULL;
    app->tray = NULL; /* the connection's teardown took the model's
                       * protocol state with it                      */
    app->stopping = 1;
    app->exit_code = 1;
    app->stop_reason = "bus died";
}

/* ---- output events -------------------------------------------------------- */

static void output_event(fdk_shell *shell,
                         const fdk_shell_output_event *event, void *user) {
    (void)shell;
    trayhost *app = user;
    tracef("output event type=%d index=%u", (int)event->type,
           event->index);
    if (event->type == FDK_SHELL_OUTPUT_ADDED ||
        event->type == FDK_SHELL_OUTPUT_CHANGED) {
        bar_relayout(app); /* re-derive geometry (and the bar origin) */
    }
}

/* ---- CLI ------------------------------------------------------------------- */

static void usage(FILE *out) {
    fputs(
        "trayhost — the fdk-shell StatusNotifier tray host "
        "(org.kde.StatusNotifierWatcher)\n\n"
        "usage: trayhost [options]\n\n"
        "  --bar-height N  the strip's height in logical px (default 36)\n"
        "  --cell N        one item's cell in logical px (default: scaled\n"
        "                  from the UI font size; 30 at 14 px)\n"
        "  --font-size N    pin the UI font size in px (default: theme/\n"
        "                  settings / the desktop statement / 16)\n"
        "  --margin N      gap from the screen edges (default 4)\n"
        "  --anchor W      tr (default) | tl | br | bl\n"
        "  --output N      show on output N only (default: all)\n"
        "  --replace       take the bus name from a host that allows\n"
        "                  replacement\n"
        "  --wait          queue for the name instead of exiting\n"
        "  --quit-after S  exit after S seconds (rigs/testing)\n"
        "  --help          this text\n"
        "  --version       the host and protocol versions\n\n"
        "Diagnostics: FDK_TRAYHOST_TRACE=1 logs every relayout,\n"
        "menu transaction, and placement decision.\n",
        out);
}

static void parse_args(trayhost *app, int argc, char **argv) {
    app->opt.bar_height = 0; /* 0 = derive from the UI metrics   */
    app->opt.cell = 0;        /* 0 = derive                        */
    app->opt.font_size = 0;   /* 0 = the UI metrics                */
    app->opt.edge_margin = 4;
    app->opt.anchor = ANCHOR_TR;
    app->opt.output_index = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "--version") == 0) {
            printf("trayhost %s (fdk-shell %s, SNI protocol 0)\n",
                   fdk_shell_version(), fdk_shell_version());
            exit(0);
        } else if (strcmp(a, "--replace") == 0) {
            app->opt.replace = 1;
        } else if (strcmp(a, "--wait") == 0) {
            app->opt.wait = 1;
        } else if (strcmp(a, "--bar-height") == 0 && i + 1 < argc) {
            app->opt.bar_height = atoi(argv[++i]);
            if (app->opt.bar_height < 12) {
                app->opt.bar_height = 12;
            }
            if (app->opt.bar_height > 96) {
                app->opt.bar_height = 96;
            }
        } else if (strcmp(a, "--cell") == 0 && i + 1 < argc) {
            app->opt.cell = atoi(argv[++i]);
            if (app->opt.cell < 16) {
                app->opt.cell = 16;
            }
            if (app->opt.cell > 64) {
                app->opt.cell = 64;
            }
        } else if (strcmp(a, "--margin") == 0 && i + 1 < argc) {
            app->opt.edge_margin = atoi(argv[++i]);
            if (app->opt.edge_margin < 0) {
                app->opt.edge_margin = 0;
            }
            if (app->opt.edge_margin > 200) {
                app->opt.edge_margin = 200;
            }
        } else if (strcmp(a, "--anchor") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "tr") == 0) {
                app->opt.anchor = ANCHOR_TR;
            } else if (strcmp(v, "tl") == 0) {
                app->opt.anchor = ANCHOR_TL;
            } else if (strcmp(v, "br") == 0) {
                app->opt.anchor = ANCHOR_BR;
            } else if (strcmp(v, "bl") == 0) {
                app->opt.anchor = ANCHOR_BL;
            } else {
                fprintf(stderr,
                        "trayhost: unknown --anchor '%s' (tr|tl|br|"
                        "bl)\n",
                        v);
                exit(2);
            }
        } else if (strcmp(a, "--output") == 0 && i + 1 < argc) {
            app->opt.output_index = atoi(argv[++i]);
        } else if (strcmp(a, "--quit-after") == 0 && i + 1 < argc) {
            app->opt.quit_after_s = strtod(argv[++i], NULL);
        } else if (strcmp(a, "--font-size") == 0 && i + 1 < argc) {
            app->opt.font_size = atoi(argv[++i]);
            if (app->opt.font_size < 8 || app->opt.font_size > 96) {
                fprintf(stderr, "--font-size: %d out of [8, 96]\n",
                        app->opt.font_size);
                exit(2);
            }
        } else {
            fprintf(stderr, "trayhost: unknown option '%s' (--help)\n",
                    a);
            exit(2);
        }
    }
}

static int g_signal_stop;

static void on_signal(int sig) {
    (void)sig;
    g_signal_stop = 1;
}

/* ---- main ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app));
    g_menu.item_index = -1;
    g_trace = getenv("FDK_TRAYHOST_TRACE") != NULL;
    parse_args(&g_app, argc, argv);

    /* The add-on's version gate (the documented pattern). */
    if (fdk_get_version() < FDK_VERSION_ENCODE(1, 1, 0) ||
        fdk_ext_api_version() < 1) {
        fprintf(stderr, "trayhost: FDK 1.1.0 (extension API v1) "
                        "required\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr,
                "trayhost: no display — a tray host renders "
                "(WAYLAND_DISPLAY or DISPLAY required)\n");
        return 1;
    }
    g_app.ctx = ctx;

    fdk_result r = fdk_shell_init(ctx, &g_app.shell);
    if (r == FDK_ERR_UNSUPPORTED) {
        fprintf(stderr,
                "trayhost: this backend offers no layer surfaces "
                "(Wayland compositor without wlr-layer-shell?)\n");
        fdk_shutdown(ctx);
        return 1;
    }
    if (!fdk_ok(r)) {
        fprintf(stderr, "trayhost: fdk_shell_init failed (%d)\n", r);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_shell_set_output_event_callback(g_app.shell, output_event, &g_app);

    ui_init(g_app.opt.font_size);
    if (g_app.opt.bar_height <= 0) {
        g_app.opt.bar_height = sc(36);
    }
    if (g_app.opt.cell <= 0) {
        g_app.opt.cell = sc(30);
    }
    tracef("ui: font %dpx (k=%.2f) bar_h=%d cell=%d row>=%d",
           g_ui.font_px, g_ui.k, g_app.opt.bar_height, g_app.opt.cell,
           g_ui.menu_row_h);
    g_app.font = fdk_font_load_system_default(g_ui.font_px);
    if (g_app.font == NULL) {
        sayf("no system font found — menus render without text "
             "(install a font, e.g. fonts-dejavu-core)");
    }

    /* The empty bar exists from the start (a toe-hold the rig can
     * see even with zero items). */
    bar_relayout(&g_app);

    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, bus_ready, &g_app);
    if (bus == NULL) {
        sayf("no session bus reachable (DBUS_SESSION_BUS_ADDRESS?) — "
             "exiting");
        if (g_app.font != NULL) {
            fdk_font_destroy(g_app.font);
        }
        fdk_shell_shutdown(g_app.shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_app.bus = bus;
    fdk_dbus_connection_set_death_handler(bus, bus_died, &g_app);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct timespec start;
    int serving_clock = 0;

    while (!g_app.stopping && !g_signal_stop) {
        fdk_result pr = fdk_pump_events(ctx, 100);
        if (pr < 0) {
            sayf("the display connection died (%d) — exiting", (int)pr);
            g_app.exit_code = 1;
            break;
        }
        if (g_app.opt.quit_after_s > 0.0) {
            if (g_app.serving && !serving_clock) {
                serving_clock = 1;
                clock_gettime(CLOCK_MONOTONIC, &start);
                tracef("quit-after clock starts (serving)");
            } else if (!serving_clock) {
                clock_gettime(CLOCK_MONOTONIC, &start);
            }
            if (serving_clock) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed =
                    (double)(now.tv_sec - start.tv_sec) +
                    (double)(now.tv_nsec - start.tv_nsec) / 1e9;
                if (elapsed >= g_app.opt.quit_after_s) {
                    break;
                }
            }
        }
    }
    if (g_signal_stop) {
        g_app.exit_code = 0;
    }

    /* Teardown: the cascade, the model, the bar, the bus, the shell,
     * FDK — ASan-clean by construction (the rig asserts it). */
    menu_close_all(&g_app);
    if (g_app.tray != NULL) {
        fdk_shell_tray_host_destroy(g_app.tray);
        g_app.tray = NULL;
    }
    if (g_app.bar != NULL) {
        fdk_shell_layer_destroy(g_app.bar);
        g_app.bar = NULL;
    }
    if (g_app.bus != NULL) {
        fdk_dbus_connection_close(g_app.bus);
        g_app.bus = NULL;
    }
    if (g_app.font != NULL) {
        fdk_font_destroy(g_app.font);
    }
    fdk_shell_shutdown(g_app.shell);
    fdk_shutdown(ctx);
    if (g_app.stop_reason != NULL && g_app.exit_code != 0) {
        sayf("%s", g_app.stop_reason);
    }
    return g_app.exit_code;
}
