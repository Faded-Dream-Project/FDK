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
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SCROLL_WHEEL_STEP 48
#define SCROLL_KEY_STEP 32
#define SCROLL_MIN_THUMB 24

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
} fdk_scrollview;

typedef struct fdk_scrollbar {
    fdk_widget base;
    fdk_scrollview *owner; /* back-pointer (bars are owned by it)   */
    bool horizontal;
    bool dragging;         /* thumb drag in progress                */
    fdk_f32 grab_offset;   /* pointer-to-thumb-top delta at grab    */
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
     * which is what bar visibility is decided from). */
    fdk_i32 other_bar = 0;
    if (horizontal) {
        other_bar = (ch > h && h > 0) ? bar_width() : 0;
        *out_view = (w - other_bar > 0) ? w - other_bar : 0;
        *out_content = cw;
    } else {
        other_bar = (cw > w && w > 0) ? bar_width() : 0;
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

/* (Re)arranges content + bars at the scrollview's CURRENT bounds —
 * the measure/arrange hooks, the layout notifier, and every scroll
 * all funnel through here. */
static void scrollview_layout(fdk_widget *w) {
    fdk_scrollview *sv = scroll_of(w);
    fdk_i32 w_ = w->bounds.width;
    fdk_i32 h_ = w->bounds.height;
    if (w_ <= 0 || h_ <= 0) {
        return;
    }

    fdk_i32 cw = 0, ch = 0;
    content_extent(sv, &cw, &ch);
    bool need_v = (ch > h_);
    bool need_h = (cw > w_);
    /* Both-overflow corner: both bars show; the content's viewport
     * loses both strips (the classic L shape). */
    fdk_i32 vw = (w_ - ((need_v) ? bar_width() : 0));
    fdk_i32 vh = (h_ - ((need_h) ? bar_width() : 0));
    if (vw < 0) {
        vw = 0;
    }
    if (vh < 0) {
        vh = 0;
    }

    scroll_clamp(sv);

    /* Content: natural size, offset by the scroll position. */
    if (sv->content != NULL) {
        fdk_rect cb = { -sv->scroll_x, -sv->scroll_y, cw, ch };
        fdk_widget_set_bounds(sv->content, cb);
    }

    /* Bars along the edges; invisible when the axis fits. RAISED to
     * the top of the z-order every layout: adopted content is
     * reparented in (appended last = top-most), and an overlay bar
     * under the content would lose every hit-test on its strip. */
    if (sv->vbar != NULL) {
        fdk_rect vb = { w_ - bar_width(), 0, bar_width(), vh };
        fdk_widget_set_bounds(sv->vbar, vb);
        fdk_widget_set_visible(sv->vbar, need_v && ch > 0);
        fdk_widget_raise(sv->vbar);
    }
    if (sv->hbar != NULL) {
        fdk_rect hb = { 0, h_ - bar_width(), vw, bar_width() };
        fdk_widget_set_bounds(sv->hbar, hb);
        fdk_widget_set_visible(sv->hbar, need_h && cw > 0);
        fdk_widget_raise(sv->hbar);
    }
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

static bool scrollview_handle_event(fdk_widget *w,
                                    const fdk_widget_event *ev) {
    fdk_scrollview *sv = scroll_of(w);
    switch (ev->type) {
    case FDK_WIDGET_SCROLL: {
        fdk_i32 dx = (fdk_i32)(ev->scroll.delta_x * (fdk_f32)SCROLL_WHEEL_STEP);
        fdk_i32 dy = (fdk_i32)(ev->scroll.delta_y * (fdk_f32)SCROLL_WHEEL_STEP);
        fdk_i32 vx = 0, cx = 0, mx = 0;
        fdk_i32 vy = 0, cy = 0, my = 0;
        axis_extents(sv, true, &vx, &cx, &mx);
        axis_extents(sv, false, &vy, &cy, &my);
        fdk_i32 nx = sv->scroll_x - dx;
        fdk_i32 ny = sv->scroll_y - dy;
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
        if (nx != sv->scroll_x || ny != sv->scroll_y) {
            sv->scroll_x = nx;
            sv->scroll_y = ny;
            scrollview_layout(w);
            fdk_widget_invalidate(w);
        }
        return true; /* the scroll is consumed either way */
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
        fdk_i32 nx = sv->scroll_x;
        fdk_i32 ny = sv->scroll_y;
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
        if (nx != sv->scroll_x || ny != sv->scroll_y) {
            sv->scroll_x = nx;
            sv->scroll_y = ny;
            scrollview_layout(w);
            fdk_widget_invalidate(w);
            return true;
        }
        return true; /* consumed even when clamped to the edge */
    }
    default:
        break;
    }
    return false;
}

/* ---- scrollbar ---- */

/* Thumb geometry in the bar's local coordinates. */
static void bar_thumb(fdk_scrollbar *b, fdk_i32 *out_pos,
                      fdk_i32 *out_len) {
    fdk_i32 view = 0, content = 0, max = 0;
    axis_extents(b->owner, b->horizontal, &view, &content, &max);
    fdk_i32 trough = b->horizontal ? b->base.bounds.width
                                   : b->base.bounds.height;
    fdk_i32 scroll = b->horizontal ? b->owner->scroll_x
                                   : b->owner->scroll_y;
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

static void scrollbar_paint(fdk_widget *w, fdk_surface *surface,
                            fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_scrollbar *b = bar_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* Trough: the track token. */
    fdk_surface_fill_rounded_rect(surface, bounds, 2, fdk__pal_track());
    /* Thumb: control background (hover/press accents come for free
     * with the palette's control family). */
    fdk_color thumb_col = fdk__pal_control();
    if ((w->flags & FDK_WF_HOVERED) != 0) {
        thumb_col = fdk__pal_control_hover();
    }
    if (b->dragging) {
        thumb_col = fdk__pal_control_pressed();
    }
    fdk_i32 pos = 0, len = 0;
    bar_thumb(b, &pos, &len);
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
        fdk_surface_fill_rounded_rect(surface, tr, 2, thumb_col);
    }
}

static bool scrollbar_handle_event(fdk_widget *w,
                                   const fdk_widget_event *ev) {
    fdk_scrollbar *b = bar_of(w);
    fdk_scrollview *sv = b->owner;
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
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
            if (b->horizontal) {
                sv->scroll_x = ns;
            } else {
                sv->scroll_y = ns;
            }
            scrollview_layout(&sv->base);
            fdk_widget_invalidate(&sv->base);
        }
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION: {
        if (!b->dragging) {
            return false;
        }
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
                sv->scroll_x = ns;
                scrollview_layout(&sv->base);
                fdk_widget_invalidate(&sv->base);
            }
        } else {
            if (ns != sv->scroll_y) {
                sv->scroll_y = ns;
                scrollview_layout(&sv->base);
                fdk_widget_invalidate(&sv->base);
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_UP:
        if (b->dragging) {
            b->dragging = false;
            fdk_widget_invalidate(w);
        }
        return true;
    default:
        break;
    }
    return false;
}

static const fdk_widget_class fdk_scrollbar_class_def = {
    .size = sizeof(fdk_scrollbar),
    .name = "scrollbar",
    .handle_event = scrollbar_handle_event,
    .paint = scrollbar_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
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
    .destroy = NULL,
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
        fdk_widget_set_visible(bar, false);
    }
    bar = NULL;
    if (fdk_ok(fdk_widget_create(w, &fdk_scrollbar_class_def,
                                 (fdk_rect){0, 0, 0, 0}, &bar))) {
        sv->hbar = bar;
        bar_of(bar)->owner = sv;
        bar_of(bar)->horizontal = true;
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
    fdk_i32 ox = sv->scroll_x, oy = sv->scroll_y;
    sv->scroll_x = x;
    sv->scroll_y = y;
    bool clamped = scroll_clamp(sv);
    scrollview_layout(scrollview);
    fdk_widget_invalidate(scrollview);
    (void)clamped; /* the GET reflects the clamped truth */
    if (sv->scroll_x != ox || sv->scroll_y != oy) {
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
