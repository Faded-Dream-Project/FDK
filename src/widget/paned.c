/*
 * paned.c — the two-pane splitter (1.4.1).
 *
 * GTK's GtkPaned / Qt's QSplitter: the FIRST child is pane 1, the
 * SECOND is pane 2, and the class's max_children = 2 makes a third
 * a loud INVALID_ARGUMENT instead of a silent layout orphan. The
 * divider is chrome (drawn by the paned's own paint, in the gap the
 * pane slots leave), a drag band (press inside the band arms the
 * implicit grab; motion retargets the divider; release parks it),
 * and a keyboard citizen (the paned is focusable; arrows step,
 * Home/End jump — the divider IS the focus visual's owner).
 *
 * Position semantics (fdk_widgets.h documents the full contract):
 * UNSET splits each pane to its child's natural size with the
 * leftover shared evenly; SET pins the divider's offset from the
 * pane-1 edge, clamped so the divider stays visible. The stored
 * value is raw; the APPLIED value (what get_position reports) is
 * the raw value clamped against the current extent at arrange
 * time — a paned that shrinks below its pin drags the divider
 * along instead of pushing it outside.
 *
 * Slots are PARENT-RELATIVE (the core contract, same as box
 * packing) and applied through fdk_widget_arrange so a pane that
 * is itself a container relayouts its own subtree.
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include <stdint.h>
#include <stdio.h>

/* Divider geometry: the visual band is FDK_PANED_DIVIDER (6) px
 * wide; the visible line is 1 px centered in it. The keyboard step
 * (GTK uses 8 px per press — one "nudge"). */
#define PANED_STEP 8

/* The applied divider offset for the CURRENT extent: raw pinned
 * value clamped into [0, extent - divider]; the auto split when
 * unset. `extent` is the paned's along-axis content length. */
static fdk_i32 paned_applied_position(const fdk_paned *p, fdk_i32 extent) {
    fdk_i32 room = extent - FDK_PANED_DIVIDER;
    if (room < 0) {
        room = 0;
    }
    if (!p->position_set) {
        /* Auto: pane 1 takes its child's natural size (clamped),
         * pane 2 the rest. When both children's naturals fit, the
         * leftover lands entirely in pane 2 — the doc's "split
         * evenly" applies the moment both fit AND overflow the
         * extent is impossible; when they DO fit, sharing the
         * leftover evenly would grow pane 1 past the natural the
         * app shrink-wrapped around. GTK's unset position gives
         * pane 1 its requisition and pane 2 everything else; that
         * is the behavior apps depend on, and it is what this
         * implements. */
        fdk_widget *c1 = (p->base.child_count > 0)
            ? p->base.children[0]
            : NULL;
        if (c1 == NULL || (c1->flags & FDK_WF_VISIBLE) == 0) {
            return 0;
        }
        fdk_size nat;
        fdk_widget_measure(c1, &nat);
        fdk_i32 want = (p->orientation == FDK_HORIZONTAL) ? nat.width
                                                          : nat.height;
        if (want > room) {
            want = room;
        }
        if (want < 0) {
            want = 0;
        }
        return want;
    }
    if (p->position > room) {
        return room;
    }
    if (p->position < 0) {
        return 0;
    }
    return p->position;
}

static void paned_measure(fdk_widget *w, fdk_size *out) {
    fdk_paned *p = paned_of(w);
    fdk_i32 along = FDK_PANED_DIVIDER;
    fdk_i32 cross = 0;
    for (size_t i = 0; i < w->child_count && i < 2; i++) {
        fdk_widget *child = w->children[i];
        if ((child->flags & FDK_WF_VISIBLE) == 0) {
            continue;
        }
        fdk_size nat;
        fdk_widget_measure(child, &nat);
        if (p->orientation == FDK_HORIZONTAL) {
            along += nat.width;
            if (nat.height > cross) {
                cross = nat.height;
            }
        } else {
            along += nat.height;
            if (nat.width > cross) {
                cross = nat.width;
            }
        }
    }
    if (along < 0) {
        along = INT32_MAX / 2; /* hostile metrics: saturate, don't wrap */
    }
    if (cross < 0) {
        cross = INT32_MAX / 2;
    }
    if (p->orientation == FDK_HORIZONTAL) {
        out->width = along;
        out->height = cross;
    } else {
        out->width = cross;
        out->height = along;
    }
}

static void paned_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_paned *p = paned_of(w);
    fdk_widget_set_bounds(w, assigned);

    if (w->child_count == 0) {
        return;
    }
    bool horiz = p->orientation == FDK_HORIZONTAL;
    fdk_i32 extent = horiz ? w->bounds.width : w->bounds.height;
    fdk_i32 cross = horiz ? w->bounds.height : w->bounds.width;
    fdk_i32 pos = paned_applied_position(p, extent);

    /* One child: it IS the paned (no divider to reserve against).
     * The band check in the event hooks still routes presses
     * harmlessly (the divider sits at the far edge). */
    if (w->child_count == 1) {
        fdk_widget *c1 = w->children[0];
        if ((c1->flags & FDK_WF_VISIBLE) != 0) {
            fdk_rect slot = {0, 0, horiz ? extent : cross,
                             horiz ? cross : extent};
            fdk_widget_arrange(c1, slot);
        }
        return;
    }

    fdk_i32 pane1 = pos;
    fdk_i32 pane2 = extent - FDK_PANED_DIVIDER - pos;
    if (pane2 < 0) {
        pane2 = 0;
    }
    fdk_widget *c1 = w->children[0];
    if ((c1->flags & FDK_WF_VISIBLE) != 0) {
        fdk_rect slot1 = horiz ? (fdk_rect){0, 0, pane1, cross}
                               : (fdk_rect){0, 0, cross, pane1};
        fdk_widget_arrange(c1, slot1);
    }
    fdk_widget *c2 = w->children[1];
    if ((c2->flags & FDK_WF_VISIBLE) != 0) {
        fdk_rect slot2 = horiz
            ? (fdk_rect){pos + FDK_PANED_DIVIDER, 0, pane2, cross}
            : (fdk_rect){0, pos + FDK_PANED_DIVIDER, cross, pane2};
        fdk_widget_arrange(c2, slot2);
    }
}

/* Divider band geometry in paned-LOCAL px (the band includes its
 * own extent; the visual is drawn inside it). False when the paned
 * has a single child (no divider exists). */
static bool paned_divider_band(fdk_paned *p, fdk_i32 *out_start) {
    if (p->base.child_count < 2) {
        return false;
    }
    bool horiz = p->orientation == FDK_HORIZONTAL;
    fdk_i32 extent = horiz ? p->base.bounds.width : p->base.bounds.height;
    *out_start = paned_applied_position(p, extent);
    return true;
}

static void paned_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_paned *p = paned_of(w);
    if (bounds.width <= 0 || bounds.height <= 0 || w->child_count < 2) {
        return;
    }
    fdk_i32 start = 0;
    if (!paned_divider_band(p, &start)) {
        return;
    }
    bool horiz = p->orientation == FDK_HORIZONTAL;
    const bool disabled = (w->flags & FDK_WF_ENABLED) == 0;
    bool hot = (p->div_hovering || p->dragging) && !disabled;

    if (horiz) {
        fdk_i32 x = bounds.x + start;
        /* The hot pill: a rounded accent bar filling the band's
         * height minus a pixel of air each side — the modern
         * splitter affordance (Adwaita's wide separator hover). */
        if (hot) {
            fdk_rect pill = {x + 1, bounds.y + 1,
                             FDK_PANED_DIVIDER - 2, bounds.height - 2};
            if (pill.width > 0 && pill.height > 0) {
                fdk_color a = fdk__pal_accent();
                fdk_color fill = {a.r, a.g, a.b, a.a * 0.45f};
                fdk_surface_fill_rounded_rect(surface, pill,
                                              FDK_PANED_DIVIDER / 2, fill);
            }
        }
        /* The resting line: 1 px, border color, centered. */
        fdk_i32 cx = x + FDK_PANED_DIVIDER / 2;
        fdk_color line = hot ? fdk__pal_accent() : fdk__pal_border();
        if (disabled) {
            line = fdk__pal_control_disabled();
        }
        fdk_surface_fill_rect(surface,
                              (fdk_rect){cx, bounds.y, 1, bounds.height},
                              line);
        /* Grip: three dots down the center — the "draggable" read. */
        fdk_color dot = disabled ? fdk__pal_control_disabled()
                                 : fdk__pal_track();
        fdk_i32 cy = bounds.y + bounds.height / 2;
        fdk_i32 dx = cx - 2;
        for (int i = 0; i < 3; i++) {
            fdk_surface_fill_rect(surface,
                                  (fdk_rect){dx + i * 2, cy, 1, 1}, dot);
        }
    } else {
        fdk_i32 y = bounds.y + start;
        if (hot) {
            fdk_rect pill = {bounds.x + 1, y + 1,
                             bounds.width - 2, FDK_PANED_DIVIDER - 2};
            if (pill.width > 0 && pill.height > 0) {
                fdk_color a = fdk__pal_accent();
                fdk_color fill = {a.r, a.g, a.b, a.a * 0.45f};
                fdk_surface_fill_rounded_rect(surface, pill,
                                              FDK_PANED_DIVIDER / 2, fill);
            }
        }
        fdk_i32 cy = y + FDK_PANED_DIVIDER / 2;
        fdk_color line = hot ? fdk__pal_accent() : fdk__pal_border();
        if (disabled) {
            line = fdk__pal_control_disabled();
        }
        fdk_surface_fill_rect(surface,
                              (fdk_rect){bounds.x, cy, bounds.width, 1},
                              line);
        fdk_color dot = disabled ? fdk__pal_control_disabled()
                                 : fdk__pal_track();
        fdk_i32 cx = bounds.x + bounds.width / 2;
        fdk_i32 dy = cy - 2;
        for (int i = 0; i < 3; i++) {
            fdk_surface_fill_rect(surface,
                                  (fdk_rect){cx, dy + i * 2, 1, 1}, dot);
        }
    }
}

/* Retargets the divider from a pointer position (paned-local along
 * coordinate) honoring the grab offset, then re-applies the layout
 * at the CURRENT bounds (a drag must not wait for the parent's next
 * arrange — the divider follows the pointer frame by frame). */
static void paned_drag_to(fdk_paned *p, fdk_f32 along) {
    fdk_i32 want = (fdk_i32)along - p->grab_offset;
    bool horiz = p->orientation == FDK_HORIZONTAL;
    fdk_i32 extent = horiz ? p->base.bounds.width : p->base.bounds.height;
    fdk_i32 room = extent - FDK_PANED_DIVIDER;
    if (room < 0) {
        room = 0;
    }
    if (want > room) {
        want = room;
    }
    if (want < 0) {
        want = 0;
    }
    if (p->position_set && p->position == want) {
        return; /* nothing moved */
    }
    p->position = want;
    p->position_set = true;
    /* Re-arrange at current bounds (the arrange hook's own path —
     * bounds unchanged, children re-slotted). */
    fdk_rect cur = p->base.bounds;
    paned_arrange(&p->base, cur);
    fdk_widget_invalidate(&p->base);
    /* A11y: the divider position is the value interface. */
    fdk__a11y_notify(&p->base, FDK_A11Y_VALUE_CHANGED, 0);
}

static bool paned_handle_event(fdk_widget *w,
                               const fdk_widget_event *ev) {
    fdk_paned *p = paned_of(w);
    bool horiz = p->orientation == FDK_HORIZONTAL;
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        fdk_i32 start = 0;
        if (!paned_divider_band(p, &start)) {
            return false;
        }
        fdk_f32 along = horiz ? ev->pointer.position.x
                              : ev->pointer.position.y;
        if (along >= (fdk_f32)start &&
            along < (fdk_f32)(start + FDK_PANED_DIVIDER)) {
            p->dragging = true;
            p->grab_offset = (fdk_i32)along - start;
            fdk_widget_invalidate(w);
            return true; /* the grab is ours */
        }
        return false;
    }
    case FDK_WIDGET_POINTER_MOTION: {
        if (p->dragging) {
            fdk_f32 along = horiz ? ev->position.x : ev->position.y;
            paned_drag_to(p, along);
            return true;
        }
        /* Band hover tracking rides MOTION — the core's ENTER/LEAVE
         * transitions carry no position (the contract: LEAVE/ENTER
         * are synthesized without coordinates; every ENTER is
         * immediately followed by the MOTION that caused it, which
         * DOES carry them). */
        if (w->child_count >= 2 && (w->flags & FDK_WF_ENABLED) != 0) {
            fdk_i32 start = 0;
            (void)paned_divider_band(p, &start);
            fdk_f32 along = horiz ? ev->position.x : ev->position.y;
            bool in_band = along >= (fdk_f32)start &&
                           along < (fdk_f32)(start + FDK_PANED_DIVIDER);
            if (in_band != p->div_hovering) {
                p->div_hovering = in_band;
                fdk_widget_invalidate(w);
            }
        }
        return false;
    }
    case FDK_WIDGET_POINTER_UP:
        if (p->dragging) {
            p->dragging = false;
            fdk_widget_invalidate(w);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_ENTER:
        /* No position on synthesized ENTERs — the MOTION that
         * follows owns the band tracking (see above). Nothing to
         * do; containers still see hover through the flag. */
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        /* Off the paned entirely: the band cannot be hovered. */
        if (p->div_hovering) {
            p->div_hovering = false;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_KEY_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        fdk_scancode sc = ev->key.scancode;
        /* Toward pane 1 / pane 2 — arrows read along the axis. */
        bool toward1 = (sc == FDK_KEY_LEFT) || (sc == FDK_KEY_UP);
        bool toward2 = (sc == FDK_KEY_RIGHT) || (sc == FDK_KEY_DOWN);
        fdk_i32 extent = horiz ? w->bounds.width : w->bounds.height;
        fdk_i32 room = extent - FDK_PANED_DIVIDER;
        if (room < 0) {
            room = 0;
        }
        fdk_i32 cur = paned_applied_position(p, extent);
        fdk_i32 want = cur;
        if (toward1 && (horiz ? sc == FDK_KEY_LEFT : sc == FDK_KEY_UP)) {
            want = cur - PANED_STEP;
        } else if (toward2 &&
                   (horiz ? sc == FDK_KEY_RIGHT : sc == FDK_KEY_DOWN)) {
            want = cur + PANED_STEP;
        } else if (sc == FDK_KEY_HOME) {
            want = 0;
        } else if (sc == FDK_KEY_END) {
            want = room;
        } else {
            return false;
        }
        if (want < 0) {
            want = 0;
        }
        if (want > room) {
            want = room;
        }
        if (p->position_set && want == p->position) {
            return true; /* consumed, but nothing to do */
        }
        p->position = want;
        p->position_set = true;
        fdk_rect cur_rect = w->bounds;
        paned_arrange(w, cur_rect);
        fdk_widget_invalidate(w);
        fdk__a11y_notify(w, FDK_A11Y_VALUE_CHANGED, 0);
        return true;
    }
    default:
        return false;
    }
}

/* The layout notifier's hook (box.c): the paned's subtree changed
 * (child added / child's natural size changed / position state
 * changed) — re-run the arrangement at the CURRENT bounds (the
 * applied position re-measures the children, so a pane whose
 * natural size changed re-splits without waiting for the parent). */
void fdk__paned_layout_changed(fdk_widget *w) {
    if (w == NULL || w->klass != &fdk_paned_class_def) {
        return;
    }
    if ((w->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_rect cur = w->bounds;
    paned_arrange(w, cur);
}

/* ---- a11y ---- */

static void paned_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_paned *p = (const fdk_paned *)(const void *)w;
    if (w->child_count < 2) {
        return; /* no divider exists yet */
    }
    bool horiz = p->orientation == FDK_HORIZONTAL;
    fdk_i32 extent = horiz ? w->bounds.width : w->bounds.height;
    fdk_i32 room = extent - FDK_PANED_DIVIDER;
    if (room < 0) {
        room = 0;
    }
    fdk_i32 pos = paned_applied_position(p, extent);
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = (double)room;
    out->value_current = (double)pos;
    out->value_text = fdk__a11y_valuef("%d px", (int)pos);
}

static const fdk_a11y_class paned_a11y = {
    .role = FDK_A11Y_ROLE_SPLIT_PANE,
    .describe = paned_a11y_describe,
    .actions = NULL, /* arrows drive it; no AT "activate" verb fits */
    .perform = NULL,
};

const fdk_widget_class fdk_paned_class_def = {
    .size = sizeof(fdk_paned),
    .name = "paned",
    .handle_event = paned_handle_event,
    .paint = paned_paint,
    .measure = paned_measure,
    .arrange = paned_arrange,
    .destroy = NULL,
    .a11y = &paned_a11y,
    .max_children = 2, /* the panes ARE the layout */
};

fdk_result fdk_paned_create(fdk_widget *parent,
                            fdk_orientation orientation,
                            fdk_widget **out_paned) {
    if (out_paned == NULL || (orientation != FDK_HORIZONTAL &&
                              orientation != FDK_VERTICAL)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_paned_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_paned *p = paned_of(w);
    p->orientation = orientation;
    p->position_set = false;
    p->position = 0;
    p->dragging = false;
    p->grab_offset = 0;
    p->div_hovering = false;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_paned = w;
    return FDK_OK;
}

fdk_result fdk_paned_set_position(fdk_widget *paned, fdk_i32 position) {
    if (paned == NULL || paned->klass != &fdk_paned_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (position < 0) {
        return FDK_ERR_INVALID_ARGUMENT; /* unset_position is "auto" */
    }
    fdk_paned *p = paned_of(paned);
    if (p->position_set && p->position == position) {
        return FDK_OK;
    }
    p->position = position;
    p->position_set = true;
    /* Apply at the current bounds immediately (the drag path's
     * discipline: programmatic moves do not wait for the parent). */
    fdk_rect cur = paned->bounds;
    paned_arrange(paned, cur);
    fdk_widget_invalidate(paned);
    fdk__a11y_notify(paned, FDK_A11Y_VALUE_CHANGED, 0);
    return FDK_OK;
}

fdk_i32 fdk_paned_get_position(fdk_widget *paned) {
    if (paned == NULL || paned->klass != &fdk_paned_class_def) {
        return 0;
    }
    fdk_paned *p = paned_of(paned);
    bool horiz = p->orientation == FDK_HORIZONTAL;
    fdk_i32 extent = horiz ? paned->bounds.width : paned->bounds.height;
    return paned_applied_position(p, extent);
}

bool fdk_paned_position_is_set(fdk_widget *paned) {
    if (paned == NULL || paned->klass != &fdk_paned_class_def) {
        return false;
    }
    return paned_of(paned)->position_set;
}

void fdk_paned_unset_position(fdk_widget *paned) {
    if (paned == NULL || paned->klass != &fdk_paned_class_def) {
        return;
    }
    fdk_paned *p = paned_of(paned);
    if (!p->position_set) {
        return;
    }
    p->position_set = false;
    p->position = 0;
    fdk_rect cur = paned->bounds;
    paned_arrange(paned, cur);
    fdk_widget_invalidate(paned);
    fdk__a11y_notify(paned, FDK_A11Y_VALUE_CHANGED, 0);
}
