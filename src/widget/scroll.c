#define FDK_LOG_TAG "widgets"

/*
 * scroll.c — ScrollView + its internal Scrollbar (Phase 9)
 *
 * A scrolling CONTAINER: exactly one content child (set_content),
 * positioned at (-scroll_x, -scroll_y) and clipped to the viewport by
 * the paint walk's bounds clip (each widget's subtree paints inside
 * its bounds — the Phase 4 rule ScrollView is named after). Hit
 * testing works the same way: the content sits at negative offsets
 * inside the scrollview, so viewport-local points map onto it
 * directly and points outside decline naturally.
 *
 * Scrollbars are internal child widgets (class "scrollbar", one per
 * axis, created up front and auto-hidden) that OVERLAY the right/bottom edges. They are
 * children rather than paint-hook chrome because they need pointer
 * interaction (thumb drag with the implicit grab, trough paging) —
 * and children get exactly that from the Phase 4 event machinery.
 * Being later siblings of the content, they win hit-tests on their
 * strips. Auto-hide: a bar is visible only when its axis overflows
 * (content extent > viewport extent); hidden bars are input-
 * transparent (Phase 4 visible-flag semantics).
 *
 * Wheel: FDK_WIDGET_SCROLL bubbles up from whatever the pointer is
 * over inside the viewport (labels and plain widgets don't consume
 * it), so scrolling works anywhere over the content — the scrollview
 * handles the first SCROLL that reaches it.
 *
 * Keyboard: the scrollview is focusable ONLY when the application
 * opts in (set_can_focus) — by default focus goes to the content's
 * own focusables, and arrow keys must reach THEM, not the scroller.
 *
 * Themed metric: FDK_TM_SCROLLBAR_WIDTH (6..24, default 12).
 */

#include "widgets_internal.h"
#include "fdk/fdk_animation.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h" /* fdk__window_context (idle clock),
                                        * fdk__window_tree_paint_scale/_surface
                                        * (round 14 strip gate) */
#include "../render/surface_internal.h" /* fdk__surface_self_blit (round 14) */

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* getenv (FDK_SCROLL_NOBLIT) */

#define SCROLL_WHEEL_STEP 48
/* Round 15 (the scroll-feel report): the classic wheel notch moves a
 * viewport-PROPORTIONAL distance — GTK3's rule (a notch = ~1/4 of
 * the visible area, clamped to sane bounds), not a fixed 48 px that
 * feels glacial in a 1000 px list and violent in a 150 px sidebar.
 * Sub-unit deltas (high-resolution wheels, touchpads) keep their
 * fractional meaning: the delta MULTIPLIES the step, so 0.1-notch
 * ticks move a tenth of it — direct, smooth, no row snapping. */
#define SCROLL_NOTCH_MIN 40
#define SCROLL_NOTCH_MAX 160
#define SCROLL_NOTCH_NUM 4 /* one notch = viewport / 4 */
#define SCROLL_KEY_STEP 32
#define SCROLL_MIN_THUMB 24
/* Overlay-mode rhythm (1.0.0): a bar holds ~800 ms after the last
 * activity on its axis, then fades over ~300 ms (quad-out — a
 * departure is softer than an arrival). Activity = any scroll on
 * the axis, the pointer approaching the strip, a hover, or a drag;
 * every touch of it re-arms the clock. */
#define BAR_IDLE_MS 800
#define BAR_FADE_MS 300
/* Smooth-scroll flight (1.0.0): the input-gesture path (wheel,
 * keyboard) eases to the target over this many ms; the public
 * scroll_to/scroll_by and every value-interface path (a11y
 * SET_VALUE, scrollbar thumb drags) stay INSTANT — the programmatic
 * contract is "this position, now", and finger tracking must never
 * lag the finger. Cubic-out: fast start, soft landing — the classic
 * scroll feel. */
#define SCROLL_FLIGHT_MS 140

/* Page step: 90% of the viewport (the overlap gives context). */
static fdk_i32 page_step(fdk_i32 viewport) {
    return (viewport / 10) * 9;
}

typedef struct fdk_scrollview {
    fdk_widget base;
    fdk_widget *content;   /* borrowed; parented to the scrollview */
    fdk_widget *vbar;      /* internal "scrollbar" children         */
    fdk_widget *hbar;
    fdk_i32 scroll_x;      /* >= 0, clamped to content-viewport     */
    fdk_i32 scroll_y;
    /* The in-flight smooth scroll (NULL when idle). Owned by the
     * animation engine's lifetime rules: released by completion,
     * cancel, or the widget teardown sweep without any callback
     * (so this pointer may dangle briefly DURING teardown — nothing
     * reads it after the sweep, which runs before the free). */
    fdk_animation *flight;
    fdk_i32 flight_from_x, flight_from_y;
    fdk_i32 flight_target_x, flight_target_y;
    /* 1.0.0: CLASSIC (layout-owned strips) or OVERLAY (transient
     * thumbs that fade when idle — see fdk_scrollview_set_bar_mode). */
    fdk_scroll_bar_mode mode;
    /* 1.0.0: vertical-only scrolling — the content is arranged at
     * the VIEWPORT's width (never wider), no horizontal bar, and
     * scroll_x is pinned to 0. The recipe the internal List has
     * always used, now public: a tall column in a box layout keeps
     * the box's cross-clamp behavior instead of growing an
     * h-scrollbar. */
    bool vertical_only;
} fdk_scrollview;

typedef struct fdk_scrollbar {
    fdk_widget base;
    fdk_scrollview *owner; /* back-pointer (bars are owned by it)   */
    bool horizontal;
    bool dragging;         /* thumb drag in progress                */
    fdk_f32 grab_offset;   /* pointer-to-thumb-top delta at grab    */
    /* ---- overlay fade state (1.0.0) ----
     *
     * alpha rides the 1.0.0 paint-group engine (set_paint_alpha on
     * the BAR: its subtree blits at the blend). 1.0 at rest = the
     * plain walk, zero cost. `idle` is the one-shot countdown timer
     * (needs a window context — detached trees never arm it, and
     * their bars stay visible: the headless-honesty rule); `fade` is
     * the running alpha flight. */
    fdk_f32 alpha;
    fdk_animation *fade;
    fdk_timer *idle;
} fdk_scrollbar;

static fdk_scrollview *scroll_of(fdk_widget *w) {
    return (fdk_scrollview *)(void *)w;
}
static fdk_scrollbar *bar_of(fdk_widget *w) {
    return (fdk_scrollbar *)(void *)w;
}

extern const fdk_widget_class fdk_scrollview_class_def;
static const fdk_widget_class fdk_scrollbar_class_def;

/* ---- geometry helpers ---- */

static fdk_i32 bar_width(void) {
    return fdk_theme_get_metric(NULL, FDK_TM_SCROLLBAR_WIDTH);
}

/* The overlay bar's own (thinner) metric. */
static fdk_i32 overlay_bar_width(void) {
    return fdk_theme_get_metric(NULL, FDK_TM_SCROLLBAR_OVERLAY_WIDTH);
}

/* The bar thickness in the scrollview's CURRENT mode. */
static fdk_i32 bar_thickness(const fdk_scrollview *sv) {
    return (sv->mode == FDK_SCROLL_BARS_OVERLAY) ? overlay_bar_width()
                                                  : bar_width();
}

/* Content natural size (0x0 when there is no content). */
static void content_extent(fdk_scrollview *sv, fdk_i32 *out_w,
                           fdk_i32 *out_h) {
    *out_w = 0;
    *out_h = 0;
    if (sv->content != NULL) {
        fdk_size nat = { 0, 0 };
        fdk_widget_measure(sv->content, &nat);
        *out_w = nat.width;
        *out_h = nat.height;
    }
}

/* Axis extents for one orientation: viewport size, content size,
 * maximal scroll offset. */
static void axis_extents(fdk_scrollview *sv, bool horizontal,
                         fdk_i32 *out_view, fdk_i32 *out_content,
                         fdk_i32 *out_max) {
    fdk_i32 cw = 0, ch = 0;
    content_extent(sv, &cw, &ch);
    fdk_i32 w = sv->base.bounds.width;
    fdk_i32 h = sv->base.bounds.height;
    /* The space the OTHER axis' bar steals (visible only when that
     * axis overflows — evaluated on the content's natural extents,
     * which is what bar visibility is decided from). Overlay bars
     * steal NOTHING: the viewport is the full bounds, the bars are
     * transient guests over the content (1.0.0). */
    fdk_i32 other_bar = 0;
    if (sv->mode == FDK_SCROLL_BARS_CLASSIC) {
        if (horizontal) {
            other_bar = (ch > h && h > 0) ? bar_width() : 0;
        } else {
            /* Vertical-only never shows an h-bar, so the vertical
             * viewport never pays for one (1.0.0). */
            other_bar = (cw > w && w > 0 && !sv->vertical_only)
                            ? bar_width()
                            : 0;
        }
    }
    if (horizontal) {
        *out_view = (w - other_bar > 0) ? w - other_bar : 0;
        *out_content = sv->vertical_only ? *out_view : cw;
    } else {
        *out_view = (h - other_bar > 0) ? h - other_bar : 0;
        *out_content = ch;
    }
    *out_max = (*out_content > *out_view) ? *out_content - *out_view : 0;
}

/* Clamps both offsets to the current extents. Returns true when a
 * value changed. */
static bool scroll_clamp(fdk_scrollview *sv) {
    fdk_i32 vx = 0, cx = 0, mx = 0;
    fdk_i32 vy = 0, cy = 0, my = 0;
    axis_extents(sv, true, &vx, &cx, &mx);
    axis_extents(sv, false, &vy, &cy, &my);
    bool changed = false;
    if (sv->scroll_x > mx) {
        sv->scroll_x = mx;
        changed = true;
    }
    if (sv->scroll_y > my) {
        sv->scroll_y = my;
        changed = true;
    }
    if (sv->scroll_x < 0) {
        sv->scroll_x = 0;
        changed = true;
    }
    if (sv->scroll_y < 0) {
        sv->scroll_y = 0;
        changed = true;
    }
    return changed;
}

/* The thumb rect at an ARBITRARY offset (round 14: the damage path
 * needs the old-position thumb too). `scroll` is the axis offset in
 * content px; callers pass the live offset for painting and the
 * pre-change offset for the old∪new damage union. */
static void bar_thumb_at(fdk_scrollbar *b, fdk_i32 scroll,
                         fdk_i32 *out_pos, fdk_i32 *out_len) {
    fdk_i32 view = 0, content = 0, max = 0;
    axis_extents(b->owner, b->horizontal, &view, &content, &max);
    fdk_i32 trough = b->horizontal ? b->base.bounds.width
                                   : b->base.bounds.height;
    fdk_i32 thumb = (content > 0)
        ? (trough * view) / content
        : trough;
    if (thumb < SCROLL_MIN_THUMB) {
        thumb = SCROLL_MIN_THUMB;
    }
    if (thumb > trough) {
        thumb = trough;
    }
    fdk_i32 range = trough - thumb;
    fdk_i32 pos = (max > 0) ? (range * scroll) / max : 0;
    if (pos < 0) {
        pos = 0;
    }
    if (pos > range) {
        pos = range;
    }
    *out_pos = pos;
    *out_len = thumb;
}

static void bar_thumb(fdk_scrollbar *b, fdk_i32 *out_pos,
                      fdk_i32 *out_len) {
    bar_thumb_at(b, b->horizontal ? b->owner->scroll_x
                                  : b->owner->scroll_y,
                 out_pos, out_len);
}

/* Round 14 (the strip path): the damage a pure offset change does to
 * a VISIBLE bar — the thumb's old∪new rects, nothing else. The
 * trough (CLASSIC) and the transparent background (OVERLAY) are
 * offset-invariant; a whole-bar invalidate would coarsen the root
 * damage right back to the full viewport, defeating the strips
 * (found live: median ratio 0.52 where the strips themselves are
 * ~7%). */
static void bar_thumb_damage(fdk_widget *bar_w, fdk_i32 old_scroll) {
    fdk_scrollbar *b = bar_of(bar_w);
    fdk_i32 pos_now = 0, len = 0, pos_old = 0, len_old = 0;
    bar_thumb_at(b, b->horizontal ? b->owner->scroll_x
                                  : b->owner->scroll_y,
                 &pos_now, &len);
    bar_thumb_at(b, old_scroll, &pos_old, &len_old);
    fdk_i32 lo = (pos_now < pos_old) ? pos_now : pos_old;
    fdk_i32 hi_now = pos_now + len;
    fdk_i32 hi_old = pos_old + len_old;
    fdk_i32 hi = (hi_now > hi_old) ? hi_now : hi_old;
    if (hi <= lo) {
        return;
    }
    if (b->horizontal) {
        fdk__widget_invalidate_rect(bar_w,
                                    (fdk_rect){lo, 0, hi - lo,
                                               bar_w->bounds.height});
    } else {
        fdk__widget_invalidate_rect(bar_w,
                                    (fdk_rect){0, lo,
                                               bar_w->bounds.width,
                                               hi - lo});
    }
}

/* Local bounding-box union (widget.c's is file-static). */
static fdk_rect scroll_rect_union(fdk_rect a, fdk_rect b) {
    if (a.width <= 0 || a.height <= 0) {
        return b;
    }
    if (b.width <= 0 || b.height <= 0) {
        return a;
    }
    int64_t x0 = (int64_t)a.x < (int64_t)b.x ? (int64_t)a.x : (int64_t)b.x;
    int64_t y0 = (int64_t)a.y < (int64_t)b.y ? (int64_t)a.y : (int64_t)b.y;
    int64_t ax1 = (int64_t)a.x + a.width;
    int64_t bx1 = (int64_t)b.x + b.width;
    int64_t ay1 = (int64_t)a.y + a.height;
    int64_t by1 = (int64_t)b.y + b.height;
    return (fdk_rect){(fdk_i32)x0, (fdk_i32)y0,
                      (fdk_i32)((ax1 > bx1 ? ax1 : bx1) - x0),
                      (fdk_i32)((ay1 > by1 ? ay1 : by1) - y0)};
}

/* ---- delta-strip scroll damage (round 14) ---------------------------
 *
 * THE scroll performance path: a pure offset change (same content,
 * same geometry) repaints only what a blit cannot supply — the
 * exposed strip(s) — instead of the whole viewport. The already-
 * painted rows are MOVED inside the window surface by a same-surface
 * blit (fdk__surface_self_blit), the tree damage is set — exactly,
 * not unioned — to the strips, and the next tree paint renders only
 * those. At a 1000-row list that is a 30-pixel band instead of a
 * full-viewport repaint per wheel tick.
 *
 * Why this is safe only on the direct paint path: the blit operates
 * on the WINDOW surface's pixels, so it requires that the next tree
 * paint renders straight into that surface. fdk_window_paint
 * guarantees that at scale 1 only — at scale > 1 the tree paints
 * into a cleared logical intermediate (HiDPI compositing) whose
 * pixels are NOT the screen's; a blit there would preserve nothing.
 * fdk__window_tree_paint_scale is the gate. (The recorded follow-up:
 * a physical-space blit under scaling. Correct, merely slower, is
 * the round's contract with HiDPI.)
 *
 * When it runs: layout is called AFTER the offsets changed, with the
 * OLD offsets passed in by every caller (scroll_to, flight_apply).
 * The content arrange above already unioned its old+new bounds into
 * the root damage — the strip path REPLACES that bounding box with
 * the exact strips (fdk__widget_set_damage). Anything else that
 * invalidates during the same frame (a hover highlight, a selection
 * change) unions on top and silently widens the repaint: correct by
 * construction, the exactness flag is just bookkeeping. */
static bool scroll_strip_eligible(fdk_widget *w) {
    if (w == NULL || w->parent == NULL) {
        return false; /* detached tree: no window surface to blit */
    }
    /* FDK_SCROLL_NOBLIT=1: the A/B measurement switch for the stress
     * rigs (the strip path off = the pre-round-14 whole-viewport
     * damage). Read once. Never set in production. */
    {
        static int no_blit = -1;
        if (no_blit < 0) {
            const char *v = getenv("FDK_SCROLL_NOBLIT");
            no_blit = (v != NULL && v[0] != '\0') ? 1 : 0;
        }
        if (no_blit) {
            return false;
        }
    }
    /* HiDPI gate (window_internal.h): at scale > 1 the tree paints
     * into a cleared intermediate, so the window surface's pixels
     * are NOT the tree's paint target — a blit there preserves
     * nothing the next paint would keep. */
    if (fdk__window_tree_paint_scale(w) > 1.001f) {
        return false;
    }
    return true;
}

/* Applies (or declines) delta-strip damage for the offset change
 * old -> new at the CURRENT bounds, AFTER the content was arranged
 * at the new offsets. Returns true when the strip path was taken
 * (the caller skips its whole-viewport damage). */
static bool scroll_apply_delta_damage(fdk_widget *w, fdk_scrollview *sv,
                                      fdk_i32 old_x, fdk_i32 old_y,
                                      fdk_rect pending) {
    fdk_i32 dx = sv->scroll_x - old_x;
    fdk_i32 dy = sv->scroll_y - old_y;
    if (dx == 0 && dy == 0) {
        return false; /* no movement: caller decides what to damage */
    }
    /* Round 16 (the up-storm residue): fdk__widget_set_damage
     * REPLACES the root damage, and a LATER tick's strip can be
     * SMALLER than an earlier one when the geometry changed
     * mid-flight (the hbar flipping resizes the viewport between
     * ticks). The caller's `pending` snapshot was taken at layout
     * ENTRY — before this tick's own arrange — but a previous
     * tick's strip may have been set (and not yet painted) after
     * that snapshot. Re-read the live root box here and union it
     * in as well, so no still-unpainted strip region is dropped
     * by the replace below. */
    {
        fdk_rect live = {0, 0, 0, 0};
        (void)fdk__widget_pending_damage(w, &live);
        if (live.width > 0 && live.height > 0) {
            pending = scroll_rect_union(pending, live);
        }
    }
    fdk_i32 vw = w->bounds.width;
    fdk_i32 vh = w->bounds.height;
    if (vw <= 0 || vh <= 0) {
        return false;
    }
    fdk_i32 adx = (dx < 0) ? -dx : dx;
    fdk_i32 ady = (dy < 0) ? -dy : dy;
    if (ady >= vh || adx >= vw) {
        return false; /* whole viewport exposed: full damage wins */
    }
    /* The blit/strip footprint excludes a VISIBLE CLASSIC bar's
     * strip: the bar is an opaque widget whose column shows the
     * TROUGH, not content — a full-width blit smears shifted content
     * over it and thumb-only damage leaves the smear (found live by
     * the blit-check: the one lingering diff sat exactly in the v-
     * bar's column). OVERLAY bars float over the content: the full
     * width is correct there (the thumb repaints over the moved
     * pixels via its old∪new rects). */
    fdk_i32 bw = vw;
    fdk_i32 bh = vh;
    if (sv->mode == FDK_SCROLL_BARS_CLASSIC) {
        if (sv->vbar != NULL && fdk_widget_get_visible(sv->vbar)) {
            fdk_i32 w2 = vw - bar_thickness(sv);
            bw = (w2 > 0) ? w2 : 0;
        }
        if (sv->hbar != NULL && fdk_widget_get_visible(sv->hbar)) {
            fdk_i32 h2 = vh - bar_thickness(sv);
            bh = (h2 > 0) ? h2 : 0;
        }
        if (bw <= 0 || bh <= 0) {
            return false;
        }
    }
    if (!scroll_strip_eligible(w)) {
        return false;
    }
    fdk_surface *surface = fdk__window_tree_surface(w);
    if (surface == NULL) {
        return false;
    }
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);

    /* The preserved-pixel move, one axis at a time (a diagonal
     * scroll is two independent blits — the corner square is
     * exposed by BOTH strips, covered twice, harmless).
     *
     * Direction: scrolling DOWN (scroll_y grows, content moves up)
     * means the surface's new row r must show old row r + dy —
     * blit(dst=r, src=r+dy) over the rows that stay visible. */
    if (dy != 0 && fdk__surface_self_blit(surface,
                                          abs.x, abs.y,
                                          abs.x, abs.y + dy,
                                          bw, bh - ady) != FDK_OK) {
        return false;
    }
    if (dx != 0 && fdk__surface_self_blit(surface,
                                          abs.x, abs.y,
                                          abs.x + dx, abs.y,
                                          bw - adx, bh) != FDK_OK) {
        return false;
    }

    /* The strips, in ROOT coordinates, UNIONED with whatever was
     * already pending before this layout (bar wakes, hover). The
     * union is exact enough: one bounding box of two strips + any
     * prior damage — still far narrower than the viewport. */
    fdk_rect strip = { .x = abs.x, .y = abs.y, .width = bw, .height = 0 };
    if (dy != 0) {
        strip.y = (dy > 0) ? abs.y + bh - ady : abs.y;
        strip.height = ady;
    }
    if (dx != 0) {
        fdk_rect hstrip = { .x = (dx > 0) ? abs.x + bw - adx : abs.x,
                            .y = abs.y, .width = adx, .height = bh };
        if (dy != 0) {
            strip = scroll_rect_union(strip, hstrip);
        } else {
            strip = hstrip;
        }
    }
    /* fdk__widget_pending_damage returned a box in the same ROOT
     * frame (it WAS the root's damage box). Round 16: the blit
     * above moved every still-unpainted stale pixel by (-dx, -dy)
     * — in the aggressive regime (two notches between paints) the
     * pending box is stale content from an EARLIER tick of the
     * same frame, and the box that describes it must follow the
     * pixels. The cover is the union of THREE readings: the new
     * strips, the box where it was (a pixel not touched by the
     * blit stays stale in place), and the box where the blit took
     * it — the last one translated by (-dx, -dy) and CLIPPED to
     * the blit footprint: the part that left the viewport is gone
     * for good (it re-enters as a freshly-painted strip when it
     * scrolls back), and an UNCLIPPED translation compounds its
     * own offset every tick — the storm trace showed the box
     * descending to y = -1800 and the damage demand exploding.
     * (Found live: 5 px of dead text glyphs mid-list after a
     * 30 Hz down-storm; a 64 px displaced block after the up one.) */
    if (pending.width > 0 && pending.height > 0) {
        strip = scroll_rect_union(strip, pending);
        if (dx != 0 || dy != 0) {
            fdk_i32 mx0 = pending.x - dx;
            fdk_i32 my0 = pending.y - dy;
            fdk_i32 mx1 = mx0 + pending.width;
            fdk_i32 my1 = my0 + pending.height;
            if (mx0 < abs.x) {
                mx0 = abs.x;
            }
            if (my0 < abs.y) {
                my0 = abs.y;
            }
            if (mx1 > abs.x + bw) {
                mx1 = abs.x + bw;
            }
            if (my1 > abs.y + bh) {
                my1 = abs.y + bh;
            }
            if (mx1 > mx0 && my1 > my0) {
                fdk_rect moved = { .x = mx0, .y = my0,
                                   .width = mx1 - mx0,
                                   .height = my1 - my0 };
                strip = scroll_rect_union(strip, moved);
            }
        }
    }
    fdk__widget_set_damage(w, strip);

    /* The moved axes' visible bars: only the THUMB pixels differ
     * between the old and new offsets (the trough and the overlay
     * background are offset-invariant, and the bar's own bounds
     * never move, so nothing rides the arrange damage). The old∪new
     * thumb rects — a few dozen pixels — not the whole bar widget:
     * a 12 x full-height bar invalidate would coarsen the root
     * damage right back to the viewport and defeat the strips. */
    if (dy != 0 && sv->vbar != NULL &&
        fdk_widget_get_visible(sv->vbar)) {
        bar_thumb_damage(sv->vbar, old_y);
    }
    if (dx != 0 && sv->hbar != NULL &&
        fdk_widget_get_visible(sv->hbar)) {
        bar_thumb_damage(sv->hbar, old_x);
    }
    return true;
}

/* ---- overlay fade machinery (1.0.0) --------------------------------
 *
 * A bar's VISIBILITY is two-ANDed facts: the axis overflows (the
 * structural need, decided by the layout) and the bar is AWAKE —
 * alpha above zero, an idle countdown running, a fade in flight,
 * hovered, or dragged. A fully faded bar is input-transparent (the
 * Phase 4 visible-flag semantics), so it stops stealing presses on
 * its strip until something wakes it. Detached trees have no
 * context to arm a timer with: their bars never fade (the honesty
 * rule), which also keeps the headless suite pixel-stable. */

/* The bar's awake test (structural need is the caller's). */
static bool bar_awake(const fdk_scrollbar *b) {
    return b->alpha > 0.0f || b->idle != NULL || b->fade != NULL ||
           b->dragging ||
           ((b->base.flags & FDK_WF_HOVERED) != 0);
}

/* Re-derives one bar's visibility from its structural need (the
 * axis overflows and the content exists) and its awake state. */
static void bar_recheck_visible(fdk_widget *bar_w) {
    if (bar_w == NULL) {
        return;
    }
    fdk_scrollbar *b = bar_of(bar_w);
    fdk_i32 view = 0, content = 0, max = 0;
    axis_extents(b->owner, b->horizontal, &view, &content, &max);
    bool need = content > 0 && max > 0;
    bool show = need &&
                (b->owner->mode == FDK_SCROLL_BARS_CLASSIC ||
                 bar_awake(b));
    fdk_widget_set_visible(bar_w, show);
}

static void bar_idle_elapsed(fdk_timer *timer, void *user);

/* Wakes one axis' bar: full opacity NOW (a re-appearance reads as
 * instant — fades are for departures), the idle clock re-armed.
 * Refuses axes with nothing to scroll (no phantom bars). */
static void bar_activity(fdk_scrollview *sv, bool horizontal) {
    if (sv->mode != FDK_SCROLL_BARS_OVERLAY) {
        return;
    }
    fdk_widget *bar_w = horizontal ? sv->hbar : sv->vbar;
    if (bar_w == NULL) {
        return;
    }
    fdk_i32 view = 0, content = 0, max = 0;
    axis_extents(sv, horizontal, &view, &content, &max);
    if (content <= 0 || max <= 0) {
        return; /* nothing to indicate */
    }
    fdk_scrollbar *b = bar_of(bar_w);
    if (b->fade != NULL) {
        fdk_animation_cancel(b->fade);
        b->fade = NULL;
    }
    if (b->alpha < 1.0f) {
        b->alpha = 1.0f;
        fdk__widget_set_paint_alpha(bar_w, 1.0f);
    }
    bool was_visible_awake =
        fdk_widget_get_visible(bar_w) && b->alpha >= 1.0f;
    fdk_widget_set_visible(bar_w, true);
    if (b->idle != NULL) {
        fdk_timer_reset(b->idle, BAR_IDLE_MS);
    } else {
        fdk_context *ctx = fdk__window_context(
            fdk__widget_window_owner(&sv->base));
        if (ctx != NULL) {
            b->idle = fdk_timer_add(ctx, BAR_IDLE_MS, false,
                                    bar_idle_elapsed, bar_w);
        }
        /* No context (detached tree): no clock, no fade — the bar
         * just stays visible. The headless-honesty rule. */
    }
    if (!was_visible_awake) {
        /* The wake itself changed pixels (appearance, alpha) — the
         * whole bar repaints. But an ALREADY-visible full-alpha bar
         * changes nothing here: the thumb movement is the offset's
         * business, and the strip path damages it thumb-precisely
         * (a whole-bar invalidate per wheel would coarsen the root
         * damage back to the viewport — round 14's lesson). */
        fdk_widget_invalidate(bar_w);
    }
}

/* The fade-out flight's tick: alpha slides 1 -> 0 with the eased
 * blend; the paint group does the compositing. */
static void bar_fade_tick(fdk_animation *anim, double e, void *user) {
    (void)anim;
    fdk_widget *bar_w = user;
    fdk_scrollbar *b = bar_of(bar_w);
    b->alpha = (fdk_f32)(1.0 - e);
    if (b->alpha < 0.0f) {
        b->alpha = 0.0f;
    }
    fdk__widget_set_paint_alpha(bar_w, b->alpha);
}

static void bar_fade_done(fdk_animation *anim, bool finished,
                          void *user) {
    fdk_widget *bar_w = user;
    fdk_scrollbar *b = bar_of(bar_w);
    if (b->fade == anim) {
        b->fade = NULL;
    }
    if (finished) {
        /* Fully faded: the bar goes input-transparent until the next
         * activity wakes it. (A cancel means activity beat the fade
         * out — bar_activity already restored the alpha.) */
        b->alpha = 0.0f;
        fdk__widget_set_paint_alpha(bar_w, 0.0f);
        bar_recheck_visible(bar_w);
    }
}

/* The idle countdown expired: fade out UNLESS the pointer is on the
 * bar or a thumb drag is riding (then the clock just re-arms). */
static void bar_idle_elapsed(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_widget *bar_w = user;
    fdk_scrollbar *b = bar_of(bar_w);
    b->idle = NULL;
    if (b->dragging || (bar_w->flags & FDK_WF_HOVERED) != 0 ||
        b->fade != NULL) {
        bar_activity(b->owner, b->horizontal);
        return;
    }
    if (b->fade == NULL) {
        b->fade = fdk_widget_animate(bar_w, BAR_FADE_MS,
                                     FDK_EASE_QUAD_OUT, bar_fade_tick,
                                     bar_fade_done, bar_w);
        /* Animation-engine OOM (or a detached clock): no fade — the
         * bar simply stays. Honest degradation, no phantom vanish. */
    }
}

/* (Re)arranges content + bars at the scrollview's CURRENT bounds —
 * the measure/arrange hooks, the layout notifier, and every scroll
 * all funnel through here. `old_x`/`old_y` are the offsets BEFORE
 * this call's changes (pass the CURRENT sv->scroll_x/y when the
 * caller changed nothing — the delta is zero and nothing special
 * happens); a PURE offset change may be routed through the round-14
 * delta-strip damage (see scroll_apply_delta_damage). Returns true
 * when the strip path consumed the damage story. */
static bool scrollview_layout_ex(fdk_widget *w, fdk_i32 old_x,
                                 fdk_i32 old_y) {
    fdk_scrollview *sv = scroll_of(w);
    fdk_i32 w_ = w->bounds.width;
    fdk_i32 h_ = w->bounds.height;
    if (w_ <= 0 || h_ <= 0) {
        return false;
    }
    /* Round 14: whatever was already pending BEFORE this layout's
     * arrange coarsens the box (a bar wake from the same wheel
     * event, a hover change) must survive the strip replacement —
     * snapshot it now, scroll_apply_delta_damage unions it back. */
    fdk_rect pending = {0, 0, 0, 0};
    bool had_pending = fdk__widget_pending_damage(w, &pending);
    (void)had_pending;

    fdk_i32 cw = 0, ch = 0;
    content_extent(sv, &cw, &ch);
    bool need_v = (ch > h_);
    bool need_h = (cw > w_) && !sv->vertical_only;
    fdk_i32 th = bar_thickness(sv);
    /* Both-overflow corner: CLASSIC loses both strips (the L shape);
    * OVERLAY keeps the full viewport — the vertical bar runs the
    * full height and the horizontal bar shortens by the thickness
    * so the two thumbs never stack on the corner pixel. */
    fdk_i32 vw, vh;
    if (sv->mode == FDK_SCROLL_BARS_CLASSIC) {
        vw = w_ - ((need_v) ? bar_width() : 0);
        vh = h_ - ((need_h) ? bar_width() : 0);
    } else {
        vw = w_;
        vh = h_;
    }
    if (vw < 0) {
        vw = 0;
    }
    if (vh < 0) {
        vh = 0;
    }

    scroll_clamp(sv);

    /* Content: natural size, offset by the scroll position. ARRANGE,
     * not set_bounds: the content may be a container (a box of rows
     * is the obvious one), and only the arrange hook lays its
     * children out — set_bounds moved the box but left every child
     * at its creation bounds, so a box content rendered nothing
     * (found live by example 11's 40-row list: zero rows painted).
     * Vertical-only (1.0.0): the content's WIDTH is the viewport —
     * a column whose natural width exceeds the viewport keeps the
     * box cross-clamp behavior it had before adoption instead of
     * growing a horizontal scroll range. */
    if (sv->content != NULL) {
        fdk_i32 content_w = sv->vertical_only ? vw : cw;
        fdk_rect cb = { -sv->scroll_x, -sv->scroll_y, content_w, ch };
        fdk_widget_arrange(sv->content, cb);
    }

    /* Bars along the edges. RAISED to the top of the z-order every
     * layout: adopted content is reparented in (appended last =
     * top-most), and an overlay bar under the content would lose
     * every hit-test on its strip. In overlay mode a fully faded
     * bar drops out of visibility here (input-transparent until the
     * next activity wakes it). */
    if (sv->vbar != NULL) {
        fdk_rect vb = { w_ - th, 0, th, (sv->mode == FDK_SCROLL_BARS_OVERLAY)
                                           ? h_ : vh };
        fdk_widget_set_bounds(sv->vbar, vb);
        if (sv->mode == FDK_SCROLL_BARS_CLASSIC) {
            fdk_widget_set_visible(sv->vbar, need_v && ch > 0);
        } else {
            bar_recheck_visible(sv->vbar);
        }
        fdk_widget_raise(sv->vbar);
    }
    if (sv->hbar != NULL) {
        fdk_i32 hw = (sv->mode == FDK_SCROLL_BARS_OVERLAY && need_v)
                         ? w_ - th
                         : vw;
        fdk_rect hb = { 0, h_ - th, hw, th };
        fdk_widget_set_bounds(sv->hbar, hb);
        if (sv->mode == FDK_SCROLL_BARS_CLASSIC) {
            fdk_widget_set_visible(sv->hbar, need_h && cw > 0);
        } else {
            bar_recheck_visible(sv->hbar);
        }
        fdk_widget_raise(sv->hbar);
    }
    /* Round 11: bar visibility just changed with the scroll range —
     * keep the input vetoes truthful (visible bar = gesture-owning,
     * hidden bar = input-transparent and no veto). */
    fdk__scrollview_veto_input(w);

    /* Round 14: a pure offset change may take the strip path. This
     * runs LAST — after the arrange's ordinary damage accumulated —
     * because the strip story REPLACES that bounding box. */
    return scroll_apply_delta_damage(w, sv, old_x, old_y, pending);
}

/* The every-where default: same offsets (no delta), full behavior. */
static void scrollview_layout(fdk_widget *w) {
    fdk_scrollview *sv = scroll_of(w);
    (void)scrollview_layout_ex(w, sv->scroll_x, sv->scroll_y);
}

static void scrollview_measure(fdk_widget *w, fdk_size *out) {
    fdk_scrollview *sv = scroll_of(w);
    fdk_i32 cw = 0, ch = 0;
    content_extent(sv, &cw, &ch);
    out->width = cw;
    out->height = ch;
    if (out->width < 24) {
        out->width = 24;
    }
    if (out->height < 24) {
        out->height = 24;
    }
}

static void scrollview_arrange(fdk_widget *w, fdk_rect assigned) {
    /* Base behavior (set_bounds + damage), then internal layout. */
    fdk_widget_set_bounds(w, assigned);
    scrollview_layout(w);
}

/* ---- the smooth-scroll flight (1.0.0) -----------------------------------
 * Input gestures ease to their target; everything programmatic
 * snaps. The flight interpolates BOTH axes from the live position
 * (a wheel tick mid-flight retargets from where the eye actually
 * is — accumulated gestures keep their momentum) and re-clamps
 * every frame against the CURRENT extents, because the content can
 * resize mid-flight (rows removed, filter applied) and yesterday's
 * target may exceed today's reach. */

static void flight_apply(fdk_widget *w, double e) {
    fdk_scrollview *sv = scroll_of(w);
    fdk_i32 vx = 0, cx = 0, mx = 0;
    fdk_i32 vy = 0, cy = 0, my = 0;
    axis_extents(sv, true, &vx, &cx, &mx);
    axis_extents(sv, false, &vy, &cy, &my);
    fdk_i32 nx = sv->flight_from_x + (fdk_i32)(
        (double)(sv->flight_target_x - sv->flight_from_x) * e);
    fdk_i32 ny = sv->flight_from_y + (fdk_i32)(
        (double)(sv->flight_target_y - sv->flight_from_y) * e);
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > mx) nx = mx;
    if (ny > my) ny = my;
    if (nx != sv->scroll_x || ny != sv->scroll_y) {
        fdk_i32 ox = sv->scroll_x, oy = sv->scroll_y;
        sv->scroll_x = nx;
        sv->scroll_y = ny;
        /* Round 14: the flight is self-damaging — when the strip
         * path is taken the layout has already set the exact damage
         * (strips + the blit's moved region). When it is declined
         * (HiDPI, detached, blit failure, full-page jump) the
         * fallback is the whole-widget invalidate this path always
         * had. The animation pump's blanket invalidate is skipped
         * for this flight (fdk__animation_set_self_damage). */
        if (!scrollview_layout_ex(w, ox, oy)) {
            fdk_widget_invalidate(w);
        }
    }
}

static void flight_tick(fdk_animation *anim, double e, void *user) {
    (void)anim;
    flight_apply((fdk_widget *)user, e);
}

static void flight_done(fdk_animation *anim, bool finished, void *user) {
    (void)finished; /* cancel-vs-complete only matters to callers */
    fdk_scrollview *sv = scroll_of((fdk_widget *)user);
    if (sv->flight == anim) {
        sv->flight = NULL;
    }
    /* Natural completion leaves the offset at the eased target (the
     * last tick ran at e = 1 exactly); a cancel leaves it wherever
     * the caller wanted — programmatic scroll_to sets it right
     * after, retargeting sets it from the live value. */
}

/* Eases the offset to (tx, ty) over SCROLL_FLIGHT_MS. The public
 * entry for the input-gesture paths. */
static void scroll_flight_to(fdk_widget *w, fdk_i32 tx, fdk_i32 ty) {
    fdk_scrollview *sv = scroll_of(w);
    if (sv->flight != NULL) {
        /* Retarget: cancel fires flight_done (clears the pointer,
         * the offset stays at the live interpolated value), then
         * the new flight starts from THAT — where the eye is. */
        fdk_animation_cancel(sv->flight);
        sv->flight = NULL;
    }
    if (tx == sv->scroll_x && ty == sv->scroll_y) {
        return; /* nothing to move (also the at-edge case) */
    }
    sv->flight_from_x = sv->scroll_x;
    sv->flight_from_y = sv->scroll_y;
    sv->flight_target_x = tx;
    sv->flight_target_y = ty;
    sv->flight = fdk_widget_animate(w, SCROLL_FLIGHT_MS,
                                    FDK_EASE_CUBIC_OUT, flight_tick,
                                    flight_done, w);
    /* Round 14: flight_tick owns its damage precisely (strips when
     * eligible, whole-widget fallback otherwise) — see the pump's
     * self_damage skip. */
    fdk__animation_set_self_damage(sv->flight);
}

static bool scrollview_handle_event(fdk_widget *w,
                                    const fdk_widget_event *ev) {
    fdk_scrollview *sv = scroll_of(w);
    switch (ev->type) {
    case FDK_WIDGET_SCROLL: {
        fdk_i32 vx = 0, cx = 0, mx = 0;
        fdk_i32 vy = 0, cy = 0, my = 0;
        axis_extents(sv, true, &vx, &cx, &mx);
        axis_extents(sv, false, &vy, &cy, &my);
        /* Per-axis notch: the axis' own viewport, quartered and
         * clamped (a 225 px sidebar pane notch = 56 px; a 1000 px
         * list notch = 160 px at the cap). */
        fdk_i32 step_y = SCROLL_WHEEL_STEP;
        if (vy > 0) {
            step_y = vy / SCROLL_NOTCH_NUM;
            if (step_y < SCROLL_NOTCH_MIN) {
                step_y = SCROLL_NOTCH_MIN;
            }
            if (step_y > SCROLL_NOTCH_MAX) {
                step_y = SCROLL_NOTCH_MAX;
            }
        }
        fdk_i32 step_x = SCROLL_WHEEL_STEP;
        if (vx > 0) {
            step_x = vx / SCROLL_NOTCH_NUM;
            if (step_x < SCROLL_NOTCH_MIN) {
                step_x = SCROLL_NOTCH_MIN;
            }
            if (step_x > SCROLL_NOTCH_MAX) {
                step_x = SCROLL_NOTCH_MAX;
            }
        }
        fdk_i32 dx = (fdk_i32)(ev->scroll.delta_x * (fdk_f32)step_x);
        fdk_i32 dy = (fdk_i32)(ev->scroll.delta_y * (fdk_f32)step_y);
        /* Overlay bars: the scrolled axes wake. */
        if (dx != 0) {
            bar_activity(sv, true);
        }
        if (dy != 0) {
            bar_activity(sv, false);
        }
        /* Gesture accumulation: a notch while a flight is airborne
         * adds to the PENDING target, not to the stale live offset
         * (six fast notches = one 6-notch glide, the classic wheel
         * feel; computing from the live offset would restart the
         * same 1-notch flight six times). The flight itself still
         * starts from where the eye is — scroll_flight_to's from is
         * the live position. */
        fdk_i32 base_x = sv->scroll_x;
        fdk_i32 base_y = sv->scroll_y;
        if (sv->flight != NULL) {
            base_x = sv->flight_target_x;
            base_y = sv->flight_target_y;
        }
        fdk_i32 nx = base_x - dx;
        fdk_i32 ny = base_y - dy;
        if (nx < 0) {
            nx = 0;
        }
        if (ny < 0) {
            ny = 0;
        }
        if (nx > mx) {
            nx = mx;
        }
        if (ny > my) {
            ny = my;
        }
        scroll_flight_to(w, nx, ny);
        return true; /* the scroll is consumed either way */
    }
    case FDK_WIDGET_POINTER_MOTION: {
        /* Overlay approach detection: the pointer nearing an edge
         * strip wakes that axis' bar BEFORE any scroll happens — a
         * faded bar is input-transparent, so this MOTION (which
         * bubbles from the content under the strip) is the only
         * revival signal a resting overlay bar has. */
        if (sv->mode == FDK_SCROLL_BARS_OVERLAY) {
            fdk_i32 th = overlay_bar_width();
            if (ev->position.x > (fdk_f32)(w->bounds.width - th)) {
                bar_activity(sv, false); /* the right edge: vertical */
            }
            if (ev->position.y > (fdk_f32)(w->bounds.height - th)) {
                bar_activity(sv, true); /* the bottom edge: horizontal */
            }
        }
        return false; /* motion keeps bubbling */
    }
    case FDK_WIDGET_KEY_DOWN: {
        if ((w->flags & FDK_WF_FOCUSED) == 0) {
            return false; /* arrows belong to the content's focusables */
        }
        fdk_i32 vx = 0, cx = 0, mx = 0;
        fdk_i32 vy = 0, cy = 0, my = 0;
        axis_extents(sv, true, &vx, &cx, &mx);
        axis_extents(sv, false, &vy, &cy, &my);
        fdk_i32 page_y = page_step(vy);
        /* Same accumulation rule as the wheel: keys held down (or
         * pressed fast) ride the pending target. */
        fdk_i32 nx = sv->scroll_x;
        fdk_i32 ny = sv->scroll_y;
        if (sv->flight != NULL) {
            nx = sv->flight_target_x;
            ny = sv->flight_target_y;
        }
        switch (ev->key.scancode) {
        case FDK_KEY_LEFT: nx -= SCROLL_KEY_STEP; break;
        case FDK_KEY_RIGHT: nx += SCROLL_KEY_STEP; break;
        case FDK_KEY_UP: ny -= SCROLL_KEY_STEP; break;
        case FDK_KEY_DOWN: ny += SCROLL_KEY_STEP; break;
        case FDK_KEY_PAGE_UP: ny -= page_y; break;
        case FDK_KEY_PAGE_DOWN: ny += page_y; break;
        case FDK_KEY_HOME: ny = 0; break;
        case FDK_KEY_END: ny = my; break;
        default: return false;
        }
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > mx) nx = mx;
        if (ny > my) ny = my;
        /* Overlay bars: the key-scrolled axes wake (both bars when
         * Home/End fire — the position proof the bar exists for). */
        bar_activity(sv, ny != sv->scroll_y);
        bar_activity(sv, nx != sv->scroll_x);
        scroll_flight_to(w, nx, ny);
        return true; /* consumed even when clamped to the edge */
    }
    default:
        break;
    }
    return false;
}

/* ---- scrollbar ---- */

static void scrollbar_paint(fdk_widget *w, fdk_surface *surface,
                            fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_scrollbar *b = bar_of(w);
    fdk_scrollview *sv = b->owner;
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_i32 pos = 0, len = 0;
    bar_thumb(b, &pos, &len);
    if (sv->mode == FDK_SCROLL_BARS_OVERLAY) {
        /* Overlay: the THUMB only — no trough, the content is the
         * background the bar rides on. A hairline inset keeps the
         * rounded ends clear of the viewport edge. */
        fdk_color thumb_col = fdk__pal_control();
        if ((w->flags & FDK_WF_HOVERED) != 0) {
            thumb_col = fdk__pal_control_hover();
        }
        if (b->dragging) {
            thumb_col = fdk__pal_control_pressed();
        }
        fdk_i32 inner = bounds.width - 2;
        if (inner < 1) {
            inner = 1;
        }
        fdk_rect tr;
        if (b->horizontal) {
            tr = (fdk_rect){bounds.x + pos, bounds.y + 1, len, inner};
        } else {
            tr = (fdk_rect){bounds.x + 1, bounds.y + pos, inner, len};
        }
        if (tr.width > 0 && tr.height > 0) {
            fdk_surface_fill_rounded_rect_aa(surface, tr, 3, thumb_col);
        }
        return;
    }
    /* Trough: the track token. */
    fdk_surface_fill_rounded_rect_aa(surface, bounds, 2, fdk__pal_track());
    /* Thumb: control background (hover/press accents come for free
     * with the palette's control family). */
    fdk_color thumb_col = fdk__pal_control();
    if ((w->flags & FDK_WF_HOVERED) != 0) {
        thumb_col = fdk__pal_control_hover();
    }
    if (b->dragging) {
        thumb_col = fdk__pal_control_pressed();
    }
    fdk_i32 pad = 2;
    fdk_i32 inner = bar_width() - pad * 2;
    if (inner < 1) {
        inner = 1;
    }
    fdk_rect tr;
    if (b->horizontal) {
        tr = (fdk_rect){bounds.x + pos, bounds.y + pad, len, inner};
    } else {
        tr = (fdk_rect){bounds.x + pad, bounds.y + pos, inner, len};
    }
    if (tr.width > 0 && tr.height > 0) {
        fdk_surface_fill_rounded_rect_aa(surface, tr, 2, thumb_col);
    }
}

static bool scrollbar_handle_event(fdk_widget *w,
                                   const fdk_widget_event *ev) {
    fdk_scrollbar *b = bar_of(w);
    fdk_scrollview *sv = b->owner;
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        /* Any press on the bar is activity: the overlay bar holds
         * while the interaction rides. */
        bar_activity(sv, b->horizontal);
        fdk_i32 view = 0, content = 0, max = 0;
        axis_extents(sv, b->horizontal, &view, &content, &max);
        fdk_i32 scroll = b->horizontal ? sv->scroll_x : sv->scroll_y;
        fdk_i32 pos = 0, len = 0;
        bar_thumb(b, &pos, &len);
        fdk_i32 local = b->horizontal ? (fdk_i32)ev->pointer.position.x
                                      : (fdk_i32)ev->pointer.position.y;
        if (local >= pos && local < pos + len) {
            /* On the thumb: drag (the implicit grab keeps MOTION). */
            b->dragging = true;
            b->grab_offset = (fdk_f32)(local - pos);
            fdk_widget_invalidate(w);
        } else {
            /* Trough: page toward the click. */
            fdk_i32 page = page_step(view);
            fdk_i32 delta = (local < pos) ? -page : page;
            fdk_i32 ns = scroll + delta;
            if (ns < 0) {
                ns = 0;
            }
            if (ns > max) {
                ns = max;
            }
            /* Round 14: thumb drags ride the strip path — a 60 Hz
             * drag must not repaint the whole viewport per motion. */
            if (b->horizontal) {
                fdk_i32 ox = sv->scroll_x;
                sv->scroll_x = ns;
                if (!scrollview_layout_ex(&sv->base, ox, sv->scroll_y)) {
                    fdk_widget_invalidate(&sv->base);
                }
            } else {
                fdk_i32 oy = sv->scroll_y;
                sv->scroll_y = ns;
                if (!scrollview_layout_ex(&sv->base, sv->scroll_x, oy)) {
                    fdk_widget_invalidate(&sv->base);
                }
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION: {
        if (!b->dragging) {
            /* A hover on the strip is activity too (the pointer
             * arrived — hold the bar). */
            bar_activity(sv, b->horizontal);
            return false;
        }
        bar_activity(sv, b->horizontal); /* keep it awake while dragging */
        fdk_i32 view = 0, content = 0, max = 0;
        axis_extents(sv, b->horizontal, &view, &content, &max);
        fdk_i32 trough = b->horizontal ? w->bounds.width : w->bounds.height;
        fdk_i32 pos = 0, len = 0;
        bar_thumb(b, &pos, &len);
        fdk_i32 range = trough - len;
        if (range <= 0 || max <= 0) {
            return true;
        }
        fdk_i32 local = b->horizontal ? (fdk_i32)ev->position.x
                                      : (fdk_i32)ev->position.y;
        fdk_i32 want = local - (fdk_i32)b->grab_offset;
        if (want < 0) {
            want = 0;
        }
        if (want > range) {
            want = range;
        }
        fdk_i32 ns = (want * max) / range;
        if (b->horizontal) {
            if (ns != sv->scroll_x) {
                fdk_i32 ox = sv->scroll_x;
                sv->scroll_x = ns;
                if (!scrollview_layout_ex(&sv->base, ox, sv->scroll_y)) {
                    fdk_widget_invalidate(&sv->base);
                }
            }
        } else {
            if (ns != sv->scroll_y) {
                fdk_i32 oy = sv->scroll_y;
                sv->scroll_y = ns;
                if (!scrollview_layout_ex(&sv->base, sv->scroll_x, oy)) {
                    fdk_widget_invalidate(&sv->base);
                }
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_UP:
        if (b->dragging) {
            b->dragging = false;
            bar_activity(sv, b->horizontal); /* re-arm the hold */
            fdk_widget_invalidate(w);
        }
        return true;
    default:
        break;
    }
    return false;
}

/* Teardown: the idle timer must not outlive the bar it points at
 * (the animation engine's destroy sweep already reaps the fade). */
static void scrollbar_destroy(fdk_widget *w) {
    fdk_scrollbar *b = bar_of(w);
    if (b->idle != NULL) {
        fdk_timer_remove(b->idle);
        b->idle = NULL;
    }
}

static const fdk_widget_class fdk_scrollbar_class_def = {
    .size = sizeof(fdk_scrollbar),
    .name = "scrollbar",
    .handle_event = scrollbar_handle_event,
    .paint = scrollbar_paint,
    .measure = NULL,
    .arrange = NULL,
    .instance_finalize = scrollbar_destroy,
};

/* ---- a11y ---- */

/* The scroll position is the value interface (0..scroll_max), with
 * SET_VALUE mapping to scroll_to in CONTENT coordinates — the same
 * numbers scroll_to takes. The internal bars expose the same value
 * as their owning scrollview (they ARE its scrolling). */
static void scrollview_a11y_describe(const fdk_widget *w,
                                     fdk_a11y_info *out) {
    const fdk_scrollview *sv = (const fdk_scrollview *)(const void *)w;
    fdk_i32 cw = 0, ch = 0, vw = 0, vh = 0;
    /* Measuring walks measure hooks whose signature is non-const
     * (hooks may cache) — the describe contract ("no mutation") is
     * honored by construction; the cast just bridges the const
     * system's inability to say so. */
    fdk_widget *mut = (fdk_widget *)(void *)(uintptr_t)w;
    content_extent(scroll_of(mut), &cw, &ch);
    fdk__scrollview_viewport(mut, &vw, &vh);
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = (double)((ch > vh) ? ch - vh : 0);
    out->value_current = (double)sv->scroll_y;
    char buf[48];
    (void)snprintf(buf, sizeof(buf), "%d, %d", (int)sv->scroll_x,
                   (int)sv->scroll_y);
    out->value_text = fdk__strdup(buf);
    (void)cw;
}

static fdk_a11y_action_set scrollview_a11y_actions(const fdk_widget *w) {
    (void)w;
    return FDK_A11Y_ACTION_SET_VALUE;
}

static bool scrollview_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                    double value) {
    if (action != FDK_A11Y_ACTION_SET_VALUE) {
        return false;
    }
    fdk_scrollview *sv = scroll_of(w);
    return fdk_ok(fdk_scrollview_scroll_to(w, sv->scroll_x,
                                           (fdk_i32)(value + 0.5)));
}

static const fdk_a11y_class scrollview_a11y = {
    .role = FDK_A11Y_ROLE_SCROLL_AREA,
    .describe = scrollview_a11y_describe,
    .actions = scrollview_a11y_actions,
    .perform = scrollview_a11y_perform,
};

const fdk_widget_class fdk_scrollview_class_def = {
    .size = sizeof(fdk_scrollview),
    .name = "scrollview",
    .handle_event = scrollview_handle_event,
    .paint = NULL, /* base paint (background fill) + children */
    .measure = scrollview_measure,
    .arrange = scrollview_arrange,
    .instance_finalize = NULL,
    .a11y = &scrollview_a11y,
};

/* ---- public API ---- */

fdk_result fdk_scrollview_create(fdk_widget *parent,
                                 fdk_widget **out_scrollview) {
    if (out_scrollview == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_scrollview_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_scrollview *sv = scroll_of(w);
    /* Bars exist from the start, hidden until their axis overflows —
     * creating them inside scrollview_layout would re-enter the
     * layout notifier while sv->vbar is still unassigned. */
    fdk_widget *bar = NULL;
    if (fdk_ok(fdk_widget_create(w, &fdk_scrollbar_class_def,
                                 (fdk_rect){0, 0, 0, 0}, &bar))) {
        sv->vbar = bar;
        bar_of(bar)->owner = sv;
        bar_of(bar)->horizontal = false;
        bar_of(bar)->alpha = 1.0f; /* awake until a clock says otherwise */
        fdk_widget_set_visible(bar, false);
    }
    bar = NULL;
    if (fdk_ok(fdk_widget_create(w, &fdk_scrollbar_class_def,
                                 (fdk_rect){0, 0, 0, 0}, &bar))) {
        sv->hbar = bar;
        bar_of(bar)->owner = sv;
        bar_of(bar)->horizontal = true;
        bar_of(bar)->alpha = 1.0f;
        fdk_widget_set_visible(bar, false);
    }
    fdk_widget_child_layout_changed(w->parent);
    *out_scrollview = w;
    return FDK_OK;
}

fdk_result fdk_scrollview_set_content(fdk_widget *scrollview,
                                      fdk_widget *content) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_scrollview *sv = scroll_of(scrollview);
    if (content == NULL) {
        if (sv->content != NULL) {
            fdk_widget *old = sv->content;
            sv->content = NULL;
            fdk_widget_destroy(old);
            scrollview_layout(scrollview);
            fdk_widget_invalidate(scrollview);
        }
        return FDK_OK;
    }
    if (content->klass == &fdk_scrollbar_class_def) {
        return FDK_ERR_INVALID_ARGUMENT; /* can't adopt a scrollbar */
    }
    if (sv->content == content) {
        return FDK_OK; /* idempotent */
    }
    /* Replace: destroy the old content (simplest ownership model —
     * documented in fdk_widgets.h: the scrollview owns its content,
     * like every FDK container parent owns children). */
    if (sv->content != NULL) {
        fdk_widget *old = sv->content;
        sv->content = NULL;
        fdk_widget_destroy(old);
    }
    sv->content = content;
    /* Reparent into the scrollview (from wherever it lives now);
     * reparent refuses roots, which is what a scrollable content
     * must be. */
    fdk_result r = fdk_widget_reparent(content, scrollview);
    if (!fdk_ok(r)) {
        sv->content = NULL;
        return r;
    }
    sv->scroll_x = 0;
    sv->scroll_y = 0;
    scrollview_layout(scrollview);
    fdk_widget_invalidate(scrollview);
    fdk_widget_child_layout_changed(scrollview->parent);
    return FDK_OK;
}

fdk_result fdk_scrollview_scroll_to(fdk_widget *scrollview, fdk_i32 x,
                                    fdk_i32 y) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_scrollview *sv = scroll_of(scrollview);
    /* The programmatic truth-setting path: snaps instantly and
     * cancels any in-flight gesture animation (flight_done clears
     * the pointer; the offset is overwritten right below). */
    if (sv->flight != NULL) {
        fdk_animation_cancel(sv->flight);
        sv->flight = NULL;
    }
    fdk_i32 ox = sv->scroll_x, oy = sv->scroll_y;
    sv->scroll_x = x;
    sv->scroll_y = y;
    bool clamped = scroll_clamp(sv);
    /* Round 14: instant scrolls take the strip path too (thumb drags
     * ride this path — a 60 Hz drag must not repaint the viewport). */
    if (!scrollview_layout_ex(scrollview, ox, oy)) {
        fdk_widget_invalidate(scrollview);
    }
    (void)clamped; /* the GET reflects the clamped truth */
    if (sv->scroll_x != ox || sv->scroll_y != oy) {
        /* Overlay bars flash on programmatic scrolls too — the bar
         * is the position PROOF, and a moved position is activity. */
        bar_activity(sv, sv->scroll_x != ox);
        bar_activity(sv, sv->scroll_y != oy);
        /* A11y: the scroll position (the value interface) moved. */
        fdk__a11y_notify(scrollview, FDK_A11Y_VALUE_CHANGED, 0);
    }
    return FDK_OK;
}

void fdk_scrollview_scroll_by(fdk_widget *scrollview, fdk_i32 dx,
                              fdk_i32 dy) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def) {
        return;
    }
    fdk_scrollview *sv = scroll_of(scrollview);
    (void)fdk_scrollview_scroll_to(scrollview, sv->scroll_x + dx,
                                   sv->scroll_y + dy);
}

fdk_result fdk_scrollview_get_scroll_offset(fdk_widget *scrollview,
                                            fdk_i32 *out_x,
                                            fdk_i32 *out_y) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def ||
        out_x == NULL || out_y == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_scrollview *sv = scroll_of(scrollview);
    *out_x = sv->scroll_x;
    *out_y = sv->scroll_y;
    return FDK_OK;
}

/* Internal: relayout hook for the layout notifier (box.c). Content
 * changed (child added / natural size changed) — re-run the internal
 * arrangement at the CURRENT bounds. */
void fdk__scrollview_layout_changed(fdk_widget *w) {
    scrollview_layout(w);
}

/* ---- bar-mode public API (1.0.0) ---- */

/* Settles one bar's overlay state after a mode switch: fades and
 * clocks gone, alpha restored. */
static void bar_reset_overlay_state(fdk_widget *bar_w, bool overlay) {
    if (bar_w == NULL) {
        return;
    }
    fdk_scrollbar *b = bar_of(bar_w);
    if (b->fade != NULL) {
        fdk_animation_cancel(b->fade);
        b->fade = NULL;
    }
    if (b->idle != NULL) {
        fdk_timer_remove(b->idle);
        b->idle = NULL;
    }
    b->alpha = 1.0f;
    fdk__widget_set_paint_alpha(bar_w, 1.0f);
    if (overlay) {
        /* Entering overlay: the bars make their debut and hold —
         * bar_activity arms the clock (a no-op in detached trees,
         * where they simply stay visible). */
        bar_activity(b->owner, b->horizontal);
    }
}

fdk_result fdk_scrollview_set_bar_mode(fdk_widget *scrollview,
                                       fdk_scroll_bar_mode mode) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def ||
        (mode != FDK_SCROLL_BARS_CLASSIC &&
         mode != FDK_SCROLL_BARS_OVERLAY)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_scrollview *sv = scroll_of(scrollview);
    if (sv->mode == mode) {
        return FDK_OK; /* idempotent */
    }
    sv->mode = mode;
    bar_reset_overlay_state(sv->vbar, mode == FDK_SCROLL_BARS_OVERLAY);
    bar_reset_overlay_state(sv->hbar, mode == FDK_SCROLL_BARS_OVERLAY);
    /* The viewport just changed size (classic reserved strips, or
     * stopped doing so): re-arrange and re-clamp at the new truth. */
    scrollview_layout(scrollview);
    fdk_widget_invalidate(scrollview);
    return FDK_OK;
}

/* Internal (list columns' anchor math, 1.0.0): the viewport width
 * the scrollview actually reserves for content in its CURRENT bar
 * mode — full width under OVERLAY bars, minus the v-bar's strip
 * under CLASSIC ones (when a v-bar is showing). */
fdk_i32 fdk__scrollview_viewport_width(fdk_widget *scrollview) {
    if (scrollview == NULL) {
        return 0;
    }
    fdk_scrollview *sv = scroll_of(scrollview);
    fdk_i32 w = scrollview->bounds.width;
    if (sv->mode != FDK_SCROLL_BARS_OVERLAY) {
        /* CLASSIC: the v-bar reserves a right strip while it is
         * showing (scrollview_layout's need_v). The list re-runs
         * its column anchor at its own layout points, so reading
         * the CURRENT arrangement is exactly right. */
        if (sv->vbar != NULL &&
            (sv->vbar->flags & FDK_WF_VISIBLE) != 0) {
            fdk_i32 bw = bar_width();
            if (bw > 0 && bw < w) {
                w -= bw;
            }
        }
    }
    return w;
}

void fdk__scrollview_veto_input(fdk_widget *scrollview) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def) {
        return;
    }
    /* The scrollview's own surface and its visible bars own their
     * presses (scrolling, thumb drags); the drag-source controller
     * must not fire from inside them (round 11: the controller
     * previously armed on ANY press whose hit chain reached a
     * source — a sweep begun in the gutter beside the rows armed
     * the file manager's drag and the DnD grab killed the band).
     * Bar visibility changes with the scroll range, so callers
     * re-run this whenever layout may have flipped a bar (the
     * scrollview does it for itself at the end of every
     * scrollview_layout). */
    fdk_widget_set_drag_veto(scrollview, true);
    fdk_scrollview *sv = scroll_of(scrollview);
    fdk_widget *bars[2] = { sv->vbar, sv->hbar };
    for (size_t i = 0; i < 2; i++) {
        if (bars[i] != NULL) {
            fdk_widget_set_drag_veto(
                bars[i], (bars[i]->flags & FDK_WF_VISIBLE) != 0);
        }
    }
}

fdk_scroll_bar_mode fdk_scrollview_get_bar_mode(fdk_widget *scrollview) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def) {
        return FDK_SCROLL_BARS_CLASSIC;
    }
    return scroll_of(scrollview)->mode;
}

fdk_result fdk_scrollview_set_vertical_only(fdk_widget *scrollview,
                                            bool vertical_only) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_scrollview *sv = scroll_of(scrollview);
    if (sv->vertical_only == vertical_only) {
        return FDK_OK; /* idempotent */
    }
    sv->vertical_only = vertical_only;
    if (vertical_only) {
        /* Any live horizontal offset dies here; the clamp keeps it
         * at 0 from now on (the axis reports no scroll range). */
        sv->scroll_x = 0;
    }
    /* The content's arranged width and the bar plan both change:
     * re-arrange and re-clamp at the new truth, exactly like a bar
     * mode switch. */
    scrollview_layout(scrollview);
    fdk_widget_invalidate(scrollview);
    return FDK_OK;
}

bool fdk_scrollview_get_vertical_only(fdk_widget *scrollview) {
    if (scrollview == NULL ||
        scrollview->klass != &fdk_scrollview_class_def) {
        return false;
    }
    return scroll_of(scrollview)->vertical_only;
}

/* Internal: the scroll area's viewport size (bounds minus visible
 * bars) — what "a page" means to keyboard navigation in the widget
 * families built on ScrollView (List, Tree). */
void fdk__scrollview_viewport(fdk_widget *w, fdk_i32 *out_w,
                              fdk_i32 *out_h) {
    if (w == NULL || w->klass != &fdk_scrollview_class_def) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    fdk_scrollview *sv = scroll_of(w);
    fdk_i32 vx = 0, cx = 0, mx = 0;
    fdk_i32 vy = 0, cy = 0, my = 0;
    axis_extents(sv, true, &vx, &cx, &mx);
    axis_extents(sv, false, &vy, &cy, &my);
    *out_w = vx;
    *out_h = vy;
}
