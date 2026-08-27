/*
 * window_internal.h — internal definition of struct fdk_window
 *
 * Bridges the public fdk_window API (fdk_window.h, fdk_event.h) to a
 * backend's fdk_platform_window via the fdk_platform_ops the owning
 * context selected. See docs/architecture.md's layering diagram —
 * this file is the "FDK Window API" layer.
 */

#ifndef FDK_WINDOW_INTERNAL_H
#define FDK_WINDOW_INTERNAL_H

#include "fdk/fdk_core.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_window.h"

#include "platform/platform_internal.h"
#include "widget/widget_internal.h"

/* Window resize edges/corners (Phase 8 resize handling): the compass
 * vocabulary shared by the window layer's edge hit-testing, the
 * FDK-driven fallback drag, and the backends' interactive-resize
 * starters (EWMH _NET_WM_MOVERESIZE directions / xdg_toplevel resize
 * edges — backends map from these values). */
typedef enum fdk_window_resize_edge {
    FDK_WRES_NONE = 0,
    FDK_WRES_N    = 1,
    FDK_WRES_NE   = 2,
    FDK_WRES_E    = 3,
    FDK_WRES_SE   = 4,
    FDK_WRES_S    = 5,
    FDK_WRES_SW   = 6,
    FDK_WRES_W    = 7,
    FDK_WRES_NW   = 8,
} fdk_window_resize_edge;

/* Pure helpers (unit-tested headless in tests/test_window_logic.c;
 * implemented in window.c): */

/* Which resize zone (if any) contains the window-local point (x,y)
 * in a width x height window with an `border`-wide zone. Corners win
 * over edges; a window narrower/shorter than 2*border degrades to
 * the closer edge, never an invalid corner. */
fdk_window_resize_edge fdk__window_resize_edge_at(fdk_i32 width,
                                                  fdk_i32 height,
                                                  fdk_i32 x, fdk_i32 y,
                                                  fdk_i32 border);

/* Applies an edge drag: `edge` from the press, (dx,dy) = pointer
 * travel since the press, (ox,oy,ow,oh) the geometry at press, and
 * (min_w,min_h)/(max_w,max_h) the clamp bounds (<=0 = no bound).
 * Writes the resulting geometry through the four out pointers —
 * left/top edges move the origin as well as resize. Non-positive
 * dimensions are clamped last. */
void fdk__window_resize_apply(fdk_window_resize_edge edge,
                              fdk_i32 dx, fdk_i32 dy,
                              fdk_i32 ox, fdk_i32 oy,
                              fdk_i32 ow, fdk_i32 oh,
                              fdk_i32 min_w, fdk_i32 min_h,
                              fdk_i32 max_w, fdk_i32 max_h,
                              fdk_i32 *out_x, fdk_i32 *out_y,
                              fdk_i32 *out_w, fdk_i32 *out_h);

/* Double-click detection (title band): a second press is a
 * double-click when it arrives within `interval_ms` milliseconds of
 * the first AND within a few pixels of it (the slop tolerates tiny
 * hand movement between clicks). */
#define FDK_WINDOW_DBLCLICK_MS 400
#define FDK_WINDOW_DBLCLICK_SLOP 5
bool fdk__window_is_double_click(fdk_i64 now_ms, fdk_i64 last_ms,
                                 fdk_i32 dx, fdk_i32 dy);

struct fdk_window {
    fdk_context *ctx;                 /* owning context, not owned by us */
    const fdk_platform_ops *ops;      /* same as ctx's ops, cached for convenience */
    bool is_popup;                    /* Phase 9: grabbed dismissal window */
    fdk_platform_window *pwindow;     /* backend-owned handle */

    fdk_size last_size;

    fdk_event_callback_fn event_callback;
    void *event_callback_user_data;

    /* Lazily created by fdk_window_get_surface() (src/render/surface.c),
     * destroyed by fdk_window_destroy(). NULL until the application
     * first asks to render into this window. */
    struct fdk_surface *surface;

    /* HiDPI paint intermediate (Phase 3 completion): at scale > 1 the
     * widget tree paints into this LOGICAL-sized ARGB surface, which
     * fdk_window_paint then composites onto the physical window
     * surface at the window's scale (see fdk_window_paint). Lazily
     * created, resized on window resize, destroyed with the window;
     * unused (NULL) at scale 1 — that path paints directly. */
    struct fdk_surface *paint_intermediate;

    /* Lazily created by fdk_window_get_root() (src/widget/widget.c's
     * window glue below), destroyed with the window. NULL until the
     * application builds a widget tree on this window. While set,
     * fdk_window_dispatch_event routes pointer/key events through the
     * tree before the application callback, and fdk_window_paint()
     * repaints+ presents the tree. */
    struct fdk_widget *root;

    /* The window's content widget (fdk_window_set_content, Phase 5
     * layout): weak reference — must be a descendant of root while
     * set. Auto-arranged to the root's full bounds on every
     * configure. Validated (still in the tree?) at each use, so a
     * destroyed content just clears the association. */
    struct fdk_widget *content;

    /* ---- Phase 8: FDK-drawn decorations ----
     *
     * deco_bar is a plain widget with a themed paint class (see
     * window.c) carrying the title Label and the close Button; it is
     * created by fdk_window_set_decorated(true) and destroyed by
     * set_decorated(false) / window destruction. The content widget
     * is arranged below the band while it exists. */
    bool decorated;
    struct fdk_widget *deco_bar;
    struct fdk_widget *deco_title;
    struct fdk_widget *deco_close;
    fdk_font *deco_font;        /* effective (borrowed or owned)  */
    fdk_font *deco_font_owned;  /* the system default we loaded   */
    char *title;                /* owned copy (backend + deco sync) */

    /* Title-band drag state (bar-local coordinates; see the drag
     * handler in window.c for why the snap formulation converges). */
    bool dragging;
    fdk_pointf drag_anchor;
    fdk_i32 drag_origin_x, drag_origin_y;

    /* ---- Phase 8 completion: window management ---- */

    /* Cached window state, updated from FDK_EVENT_WINDOW_STATE (the
     * backends own the truth — EWMH properties, xdg configures —
     * and report changes; fdk_window_is_maximized/is_minimized read
     * these caches, mirroring how last_size mirrors CONFIGURE). */
    bool maximized;
    bool minimized;

    /* Size limits last handed to fdk_window_set_size_limits — the
     * hints go to the platform, but FDK's own resize-edge drag ALSO
     * clamps to them (on a bare X server there is no WM to enforce
     * anything). <=0 means "no constraint in that dimension". */
    fdk_size min_size;
    fdk_size max_size;

    /* FDK-drawn resize edges: when set, a border-wide frame around
     * the window captures edge/corner drags (see
     * fdk_window_set_resizable). */
    bool resizable;
    bool resizable_explicit; /* set by the app, not by decorating */

    /* Active FDK-driven resize drag (the WM/compositor-driven path
     * needs none of this — begin_resize hands the whole drag over).
     * resize_edge is FDK_WRES_NONE while idle. */
    fdk_window_resize_edge resize_edge;
    fdk_i32 resize_press_x, resize_press_y;   /* window-local press */
    fdk_i32 resize_orig_x, resize_orig_y;     /* geometry at press  */
    fdk_i32 resize_orig_w, resize_orig_h;

    /* Title-band double-click tracking (monotonic ms + window-local
     * press position of the last band click). */
    fdk_i64 last_band_click_ms;
    fdk_i32 last_band_click_x, last_band_click_y;

    /* Hit rects of the band's window-management buttons (minimize /
     * maximize-restore / close), window-local, refreshed by
     * window_arrange_deco(). Which subset exists depends on the
     * backend's vtable; zero-sized when absent. Drawn as vector
     * glyphs in the bar's paint hook (font-independent — no system
     * font means no title text, never missing window buttons). */
    fdk_rect deco_btn_min;
    fdk_rect deco_btn_max;
    fdk_rect deco_btn_close;

    /* Which band button (if any) is currently pressed/hovered — 1:1
     * with the rects above, for hover/pressed themed fills. */
    int deco_hover;   /* 0 none, 1 min, 2 max, 3 close */
    int deco_pressed; /* 0 none, 1 min, 2 max, 3 close */
};

/* Called by the context's platform dispatch callback (see
 * src/core/context.c) once it has resolved a raw fdk_platform_window*
 * back to the fdk_window* that owns it. Not part of the public API. */
void fdk_window_dispatch_event(fdk_window *window, const fdk_event_data *event);

/* Resolves the opaque root-owner pointer the widget layer hands out
 * (fdk__widget_window_owner, set on window-owned roots) back to the
 * owning context — the Phase 9 back-edge behind widget-side clipboard
 * access (Entry's Ctrl+X/C/V). NULL for a non-fdk_window owner (never
 * happens today; defensive contract). */
fdk_context *fdk__window_context(void *window_owner);

#endif /* FDK_WINDOW_INTERNAL_H */
