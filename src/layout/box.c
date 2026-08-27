/*
 * box.c — the Phase 5 box layout container.
 *
 * A box is a widget subclass (src/widget/widget_internal.h pattern)
 * whose measure/arrange hooks implement linear layout. The engine is
 * two passes over the Phase 4 hooks:
 *
 *   measure  (bottom-up): natural size = padding + Σ(child natural +
 *            child margins) + spacing gaps, cross axis = max child
 *            natural cross + padding; homogeneous boxes measure as if
 *            every child were the largest.
 *   arrange  (top-down):  the box's bounds are assigned (by its
 *            parent's layout, the window content glue, or the app),
 *            then each child's slot is computed and assigned through
 *            fdk_widget_arrange — so a child that is itself a
 *            container relayouts recursively.
 *
 * Along-axis distribution: non-expanding children get their natural
 * size; expanding children split the leftover equally (integer
 * division, with accumulated-position arithmetic absorbing rounding
 * so no pixel is lost or double-claimed). Homogeneous mode gives
 * every child an equal share instead. Cross axis: expand fills it;
 * otherwise the child's natural size is placed per its align hint
 * (fill/start/center/end), clamped to the available space.
 *
 * Relayout triggers: the arrange hook (bounds assigned), and
 * fdk_widget_child_layout_changed (child added/removed/hinted — the
 * widget core calls it). All of them run the same pure-geometry
 * pass; no user code runs during layout.
 */

#define FDK_LOG_TAG "layout"

#include "layout/layout_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "widget/widget_internal.h"

#include <string.h>

/* ---- the box subclass ----
 * (struct fdk_box lives in layout_internal.h — container subclasses
 * like the widget catalog's Frame embed it and reuse the packing
 * hooks via fdk_box_class_def.) */

static bool box_class_of(const fdk_widget *w) {
    if (w == NULL || w->klass == NULL) {
        return false;
    }
    /* Box-ness is DELEGATION, not exact class identity: any class
     * that runs the box packing hooks IS a box for layout purposes.
     * The catalog's Frame embeds fdk_box and delegates BOTH hooks —
     * with an identity check here, a Frame's own children (radios
     * added after the frame) never triggered the frame's relayout
     * and never propagated upward; the frame kept its empty natural
     * until some later sibling happened to relayout the parent
     * (found live by the 07 demo: content [ key_frame [ radios ] ]
     * left the radios at 0x0 forever). Subclasses that implement
     * their OWN layout hooks are not boxes and own their relayout
     * policy themselves, exactly as before. */
    return w->klass->measure == fdk_box_measure_hook ||
           w->klass->arrange == fdk_box_arrange_hook;
}

static fdk_box *box_of(fdk_widget *w) {
    return (fdk_box *)w;
}

/* Natural along/cross extent ONE child contributes (its measured
 * size plus its margins). */
static void child_natural(fdk_widget *child, fdk_size *out) {
    fdk_size natural = {0, 0};
    fdk_widget_measure(child, &natural);
    out->width = natural.width + child->margin_left +
                 child->margin_right;
    out->height = natural.height + child->margin_top +
                  child->margin_bottom;
}

void fdk_box_measure_hook(fdk_widget *w, fdk_size *out) {
    fdk_box *box = box_of(w);
    fdk_i32 along = box->padding * 2;
    fdk_i32 max_child_along = 0;
    fdk_i32 max_child_cross = 0;
    size_t visible = 0;
    for (size_t i = 0; i < w->child_count; i++) {
        fdk_widget *child = w->children[i];
        if ((child->flags & FDK_WF_VISIBLE) == 0 ||
            (child->flags & FDK_WF_DESTROYING) != 0) {
            continue; /* hidden children take no space */
        }
        fdk_size nat;
        child_natural(child, &nat);
        fdk_i32 child_along = (box->orientation == FDK_HORIZONTAL)
                                  ? nat.width
                                  : nat.height;
        fdk_i32 child_cross = (box->orientation == FDK_HORIZONTAL)
                                  ? nat.height
                                  : nat.width;
        if (child_along > max_child_along) {
            max_child_along = child_along;
        }
        if (child_cross > max_child_cross) {
            max_child_cross = child_cross;
        }
        along += child_along;
        visible++;
    }
    if (visible > 1) {
        along += box->spacing * (fdk_i32)(visible - 1);
    }
    if (box->homogeneous && visible > 0) {
        along = box->padding * 2 +
                max_child_along * (fdk_i32)visible +
                (visible > 1 ? box->spacing * (fdk_i32)(visible - 1)
                             : 0);
    }
    along += box->title_inset; /* Frame's title band, vertical only */
    fdk_i32 cross = box->padding * 2 + max_child_cross;
    if (box->orientation == FDK_HORIZONTAL) {
        *out = (fdk_size){along, cross};
    } else {
        *out = (fdk_size){cross, along};
    }
}

/* Places children within the box's CURRENT bounds. Pure geometry:
 * fdk_widget_arrange → set_bounds → invalidate, no user callbacks. */
static void box_layout(fdk_widget *w) {
    fdk_box *box = box_of(w);
    if ((w->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_rect bounds = w->bounds;
    fdk_i32 pad = box->padding;
    fdk_i32 inset = (box->orientation == FDK_VERTICAL) ? box->title_inset
                                                       : 0;
    /* Children slots are PARENT-RELATIVE (the core contract), so
     * packing starts at the box's own origin — NOT at the box's
     * parent-relative position (bounds.x/y), which paint_rec would
     * stack on top of the box's absolute origin again. Using
     * bounds.x/y here double-offset every child of any box not
     * sitting at (0, 0) relative to ITS parent. */
    fdk_i32 content_x = pad;
    fdk_i32 content_y = pad + inset;
    fdk_i32 content_w = bounds.width - pad * 2;
    fdk_i32 content_h = bounds.height - pad * 2 - inset;
    if (content_w < 0) {
        content_w = 0;
    }
    if (content_h < 0) {
        content_h = 0;
    }
    bool horiz = box->orientation == FDK_HORIZONTAL;
    fdk_i32 content_along = horiz ? content_w : content_h;
    fdk_i32 content_cross = horiz ? content_h : content_w;

    size_t n = w->child_count;
    if (n == 0) {
        return;
    }
    fdk_size *naturals = fdk_alloc_array(n, sizeof(fdk_size));
    fdk_i32 *sizes = fdk_alloc_array(n, sizeof(fdk_i32));
    if (naturals == NULL || sizes == NULL) {
        fdk_free(naturals);
        fdk_free(sizes);
        FDK_ERROR("box layout: OOM measuring children");
        return; /* children keep their previous bounds */
    }

    /* Pass 1: naturals (with margins) and visible count. Hidden
     * children get the (-1,-1) marker and take no slot. */
    size_t visible = 0;
    fdk_i32 natural_total = 0;
    fdk_i32 max_along = 0;
    for (size_t i = 0; i < n; i++) {
        fdk_widget *child = w->children[i];
        if ((child->flags & FDK_WF_VISIBLE) == 0 ||
            (child->flags & FDK_WF_DESTROYING) != 0) {
            naturals[i] = (fdk_size){-1, -1};
            continue;
        }
        child_natural(child, &naturals[i]);
        fdk_i32 a = horiz ? naturals[i].width : naturals[i].height;
        if (a > max_along) {
            max_along = a;
        }
        visible++;
    }
    if (visible == 0) {
        fdk_free(naturals);
        fdk_free(sizes);
        return;
    }
    fdk_i32 gaps = (visible > 1) ? box->spacing * (fdk_i32)(visible - 1)
                                 : 0;

    /* Pass 2: along-axis base sizes. */
    size_t expanders = 0;
    if (box->homogeneous) {
        natural_total = max_along * (fdk_i32)visible;
        for (size_t i = 0; i < n; i++) {
            if (naturals[i].width < 0) {
                continue;
            }
            sizes[i] = max_along;
            fdk_widget *child = w->children[i];
            if (horiz ? child->expand_h : child->expand_v) {
                expanders++;
            }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            if (naturals[i].width < 0) {
                continue;
            }
            sizes[i] = horiz ? naturals[i].width : naturals[i].height;
            natural_total += sizes[i];
            fdk_widget *child = w->children[i];
            if (horiz ? child->expand_h : child->expand_v) {
                expanders++;
            }
        }
    }
    fdk_i32 leftover = content_along - gaps - natural_total;
    fdk_i32 extra_each = 0;
    if (expanders > 0 && leftover > 0) {
        extra_each = leftover / (fdk_i32)expanders;
    }

    /* Pass 3: position. `pos` accumulates actual slot extents, so
     * integer rounding is absorbed at each boundary — no drift. */
    fdk_i32 pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (naturals[i].width < 0) {
            continue;
        }
        fdk_widget *child = w->children[i];
        fdk_i32 size = sizes[i];
        if (extra_each > 0 &&
            (horiz ? child->expand_h : child->expand_v)) {
            size += extra_each;
        }

        /* Cross axis: expand fills; else natural placed by align,
         * clamped to the available cross space. */
        fdk_i32 cross_natural = horiz ? naturals[i].height
                                      : naturals[i].width;
        fdk_i32 cross_size = cross_natural;
        fdk_i32 cross_pos = 0;
        bool cross_expand = horiz ? child->expand_v : child->expand_h;
        fdk_align align = horiz ? child->align_v : child->align_h;
        if (cross_expand || align == FDK_ALIGN_FILL) {
            cross_size = content_cross;
        } else if (cross_size > content_cross) {
            cross_size = content_cross;
        }
        switch (align) {
            case FDK_ALIGN_END:
                cross_pos = content_cross - cross_size;
                break;
            case FDK_ALIGN_CENTER:
                cross_pos = (content_cross - cross_size) / 2;
                break;
            default: /* START, and FILL's base position */
                cross_pos = 0;
                break;
        }

        fdk_rect slot;
        if (horiz) {
            slot.x = content_x + pos + child->margin_left;
            slot.y = content_y + cross_pos + child->margin_top;
            slot.width = size - child->margin_left - child->margin_right;
            slot.height = cross_size - child->margin_top -
                          child->margin_bottom;
        } else {
            slot.x = content_x + cross_pos + child->margin_left;
            slot.y = content_y + pos + child->margin_top;
            slot.width = cross_size - child->margin_left -
                         child->margin_right;
            slot.height = size - child->margin_top - child->margin_bottom;
        }
        if (slot.width < 0) {
            slot.width = 0;
        }
        if (slot.height < 0) {
            slot.height = 0;
        }
        fdk_widget_arrange(child, slot);

        pos += size + box->spacing;
    }

    fdk_free(naturals);
    fdk_free(sizes);
}

void fdk_box_arrange_hook(fdk_widget *w, fdk_rect assigned) {
    /* The default bounds assignment (core), then relayout children
     * into the new geometry. */
    fdk_widget_set_bounds(w, assigned);
    box_layout(w);
}

static void box_paint(fdk_widget *w, fdk_surface *surface, fdk_rect bounds,
                      fdk_rect clip) {
    /* Subclass chain: run the base background fill first (a box with
     * a background set paints it like any widget), children then
     * paint on top in the normal tree walk. */
    const fdk_widget_class *base = fdk_widget_base_class();
    base->paint(w, surface, bounds, clip);
}

const struct fdk_widget_class fdk_box_class_def = {
    .size = sizeof(fdk_box),
    .name = "box",
    .handle_event = NULL,
    .paint = box_paint,
    .measure = fdk_box_measure_hook,
    .arrange = fdk_box_arrange_hook,
    .destroy = NULL,
};

/* ---- public box API ---- */

fdk_result fdk_box_create(fdk_widget *parent, fdk_orientation orientation,
                          fdk_widget **out_box) {
    if (out_box == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (orientation != FDK_HORIZONTAL && orientation != FDK_VERTICAL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_box_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_box *box = box_of(w);
    box->orientation = orientation;
    box->spacing = 0;
    box->padding = 0;
    box->homogeneous = false;
    /* fdk_widget_create already relayouted the parent once, but that
     * pass measured this box with its subclass fields still zeroed
     * (orientation 0 = horizontal). Re-notify now that the fields are
     * real. */
    fdk_widget_child_layout_changed(w->parent);
    *out_box = w;
    return FDK_OK;
}

static fdk_box *as_box(fdk_widget *box) {
    if (box == NULL || !box_class_of(box)) {
        return NULL;
    }
    return box_of(box);
}

void fdk_box_set_orientation(fdk_widget *box, fdk_orientation orientation) {
    fdk_box *b = as_box(box);
    if (b == NULL || b->orientation == orientation) {
        return;
    }
    b->orientation = orientation;
    box_layout(box);
    /* The box's own natural changed: ancestors that sized it are
     * stale (same propagation as child changes — see the notifier
     * below). */
    fdk_widget_child_layout_changed(box->parent);
}

fdk_orientation fdk_box_get_orientation(const fdk_widget *box) {
    /* Same delegation rule as box_class_of: frames and future box
     * subclasses answer their real orientation. */
    if (box == NULL || !box_class_of(box)) {
        return (fdk_orientation)0;
    }
    const fdk_box *b = (const fdk_box *)box;
    return b->orientation;
}

void fdk_box_set_spacing(fdk_widget *box, fdk_i32 spacing) {
    fdk_box *b = as_box(box);
    if (b == NULL) {
        return;
    }
    if (spacing < 0) {
        spacing = 0;
    }
    if (b->spacing == spacing) {
        return;
    }
    b->spacing = spacing;
    box_layout(box);
    fdk_widget_child_layout_changed(box->parent);
}

void fdk_box_set_padding(fdk_widget *box, fdk_i32 padding) {
    fdk_box *b = as_box(box);
    if (b == NULL) {
        return;
    }
    if (padding < 0) {
        padding = 0;
    }
    if (b->padding == padding) {
        return;
    }
    b->padding = padding;
    box_layout(box);
    fdk_widget_child_layout_changed(box->parent);
}

void fdk_box_set_homogeneous(fdk_widget *box, bool homogeneous) {
    fdk_box *b = as_box(box);
    if (b == NULL || b->homogeneous == homogeneous) {
        return;
    }
    b->homogeneous = homogeneous;
    box_layout(box);
    fdk_widget_child_layout_changed(box->parent);
}

/* ---- widget-core notification hook ---- */

void fdk_widget_child_layout_changed(fdk_widget *parent) {
    if (parent == NULL || !box_class_of(parent)) {
        return; /* non-containers have nothing to relayout */
    }
    box_layout(parent);

    /* Propagate upward: this box's natural size may have changed
     * (children added/removed/resized), which makes every ANCESTOR
     * container's layout stale — an ancestor that sized this box
     * before the change still holds the old natural. Found live by
     * the 07 demo: content [ key_frame [ radios ] ] left key_frame
     * at its empty-frame height forever, because nothing re-ran
     * content's layout after the radios existed.
     *
     * The chain strictly climbs parent links and terminates at the
     * first non-container (or the root); box_layout itself never
     * calls back into this hook, so there is no cycle. Ancestor
     * relayouts re-arrange their children through the arrange hook,
     * which runs box_layout directly (not this notifier). Pure
     * geometry all the way — no user code runs.
     *
     * Cost note: a child change deep in a nested UI now relayouts
     * every container up to the root (each pass re-measures its
     * children). Correct-first, per the project's no-premature-
     * optimization principle; recorded on the roadmap for the
     * layout-perf pass. */
    fdk_widget_child_layout_changed(parent->parent);
}
