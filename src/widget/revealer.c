/*
 * revealer.c — the generalized reveal flight (1.4.2).
 *
 * GTK's GtkRevealer: a container that shows or hides its ONE child
 * (max_children = 1 — a second child is a loud INVALID_ARGUMENT)
 * with a sliding animation instead of a pop. The Expander (1.4.1)
 * proved the mechanics — the door SELF-CROPS: the revealer's own
 * bounds track reveal * child-extent every tick, so the paint walk's
 * clip stack does the cropping with no parent cooperation, and the
 * child is arranged at its FULL natural size (the reveal is a clip,
 * never a squeeze — content does not reflow while it slides).
 *
 * Transitions: SLIDE_DOWN (the default — the expander's door),
 * SLIDE_UP, SLIDE_LEFT, SLIDE_RIGHT, and NONE (an instant show/hide
 * with the same visibility semantics). The anchored edge follows the
 * direction: DOWN/RIGHT keep the origin fixed and grow into the
 * room; UP/LEFT keep the far edge fixed (the door's origin MOVES as
 * it opens — a toast pinned to a window's bottom edge rises in
 * place). Under a packing container the parent re-asserts the slot's
 * origin on every notifier tick, so a box-parented revealer grows
 * into its slot downward/rightward regardless of direction — the
 * self-crop's edge anchoring is what plain-parented, hand-positioned
 * revealers get (the expander's documented contract, generalized).
 *
 * CROSSFADE is deliberately absent: the software renderer's paint
 * walk composites to a flat surface with a clip stack, not an alpha
 * scene graph — an honest slide, not a half-faked fade.
 *
 * The flight rides the 1.3.8 animator (~160 ms cubic-out by
 * default, settable); standalone trees snap — the headless-honesty
 * rule every animation obeys. on_revealed fires when the flight
 * lands (either direction — the natural completion point for
 * chaining UI, e.g. destroying a toast after it slides away).
 *
 * While fully hidden (reveal == 0) the child is FDK_WF_VISIBLE-off:
 * input-transparent, skipped by paints, out of the a11y tree's
 * SHOWING set — exactly as if it were not there, which is also the
 * revealer's measured size: 0x0 (nothing to lay out).
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"

#define REVEALER_DEFAULT_MS 160

static void revealer_tick(fdk_animation *anim, double eased,
                          void *user);
static void revealer_done(fdk_animation *anim, bool finished,
                          void *user);
static void revealer_apply_child_visibility(fdk_revealer *rv);

static bool revealer_is_vertical(fdk_revealer_transition t) {
    return t == FDK_REVEAL_SLIDE_DOWN || t == FDK_REVEAL_SLIDE_UP;
}

/* The child's natural size (0x0 when there is none) — visibility-
 * INDEPENDENT, the expander's rule: the revealer owns that flag. */
static void revealer_child_natural(const fdk_revealer *rv,
                                   fdk_size *out) {
    out->width = 0;
    out->height = 0;
    if (rv->base.child_count == 0) {
        return;
    }
    fdk_widget *child = rv->base.children[0];
    if ((child->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_widget_measure(child, out);
}

static void revealer_measure(fdk_widget *w, fdk_size *out) {
    fdk_revealer *rv = revealer_of(w);
    fdk_size cn;
    revealer_child_natural(rv, &cn);
    /* The endpoint rule: fully hidden = 0x0; the cross axis keeps
     * the child's full extent from the first crack of the door (the
     * expander's cross rule — a half-open door is full-width, a
     * shut one takes no room at all). */
    if (rv->reveal <= 0.0f) {
        out->width = 0;
        out->height = 0;
        return;
    }
    if (revealer_is_vertical(rv->transition)) {
        out->width = cn.width;
        out->height = (fdk_i32)((fdk_f32)cn.height * rv->reveal + 0.5f);
    } else {
        out->height = cn.height;
        out->width = (fdk_i32)((fdk_f32)cn.width * rv->reveal + 0.5f);
    }
}

/* Positions the child inside the CURRENT bounds per direction: full
 * natural extent on the main axis (the reveal is a clip), full
 * cross extent; UP/LEFT anchor the child to the FAR edge so the
 * visible window shows the leading edge of the content. */
static void revealer_place_child(fdk_revealer *rv) {
    if (rv->base.child_count == 0) {
        return;
    }
    fdk_widget *child = rv->base.children[0];
    if ((child->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_size cn;
    revealer_child_natural(rv, &cn);
    fdk_rect slot = {0, 0, cn.width, cn.height};
    if (revealer_is_vertical(rv->transition)) {
        slot.width = rv->base.bounds.width;
        if (rv->transition == FDK_REVEAL_SLIDE_UP) {
            slot.y = rv->base.bounds.height - cn.height;
        }
    } else {
        slot.height = rv->base.bounds.height;
        if (rv->transition == FDK_REVEAL_SLIDE_LEFT) {
            slot.x = rv->base.bounds.width - cn.width;
        }
    }
    fdk_widget_arrange(child, slot);
}

/* SELF-CROP (the expander's, edge-aware): the door's clip is this
 * widget's own bounds. DOWN/RIGHT keep the origin; UP/LEFT keep the
 * far edge — the door's origin moves as it opens. Runs at every
 * transition boundary: set_reveal_child (BEFORE the child can paint
 * uncropped), each tick, and done. */
static void revealer_self_crop(fdk_revealer *rv) {
    fdk_size cn;
    revealer_child_natural(rv, &cn);
    fdk_rect door = rv->base.bounds;
    if (revealer_is_vertical(rv->transition)) {
        fdk_i32 want = (fdk_i32)((fdk_f32)cn.height * rv->reveal + 0.5f);
        if (want < 0) {
            want = 0;
        }
        if (rv->transition == FDK_REVEAL_SLIDE_UP) {
            fdk_i32 bottom = door.y + door.height;
            door.y = bottom - want;
            door.height = want;
        } else if (door.height != want) {
            door.height = want;
        }
    } else {
        fdk_i32 want = (fdk_i32)((fdk_f32)cn.width * rv->reveal + 0.5f);
        if (want < 0) {
            want = 0;
        }
        if (rv->transition == FDK_REVEAL_SLIDE_LEFT) {
            fdk_i32 right = door.x + door.width;
            door.x = right - want;
            door.width = want;
        } else if (door.width != want) {
            door.width = want;
        }
    }
    if (door.x != rv->base.bounds.x || door.y != rv->base.bounds.y ||
        door.width != rv->base.bounds.width ||
        door.height != rv->base.bounds.height) {
        fdk_widget_set_bounds(&rv->base, door);
    }
    /* The visible window moved: the child's slot follows (UP/LEFT
     * shift the slot's origin with the door). */
    revealer_place_child(rv);
}

static void revealer_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_revealer *rv = revealer_of(w);
    fdk_widget_set_bounds(w, assigned);
    /* Visibility follows the door at EVERY arrange: a child attached
     * to a hidden revealer starts hidden (the create default is
     * visible; this hook catches it). */
    revealer_apply_child_visibility(rv);
    revealer_place_child(rv);
}

/* The layout notifier's hook (box.c): the child joined or
 * re-measured — re-place it at the CURRENT bounds. */
void fdk__revealer_layout_changed(fdk_widget *w) {
    if (w == NULL || w->klass != &fdk_revealer_class_def) {
        return;
    }
    if ((w->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    revealer_arrange(w, w->bounds);
}

/* ---- the flight ---- */

static void revealer_apply_child_visibility(fdk_revealer *rv) {
    if (rv->base.child_count == 0) {
        return;
    }
    fdk_widget *child = rv->base.children[0];
    /* Shown from the first crack of the door OPENING (the target
     * counts while the flight has yet to tick — the expander's
     * rule: hidden only at rest, fully collapsed). */
    bool want = rv->reveal > 0.0f || rv->revealed;
    bool have = (child->flags & FDK_WF_VISIBLE) != 0;
    if (want != have) {
        fdk_widget_set_visible(child, want);
    }
}

/* Retargets from the CURRENT blend (cancel + restart — the
 * animator's compose rule; mid-flight retargets never teleport). */
static void revealer_fly(fdk_revealer *rv) {
    if (rv->anim != NULL) {
        fdk_animation_cancel(rv->anim);
        rv->anim = NULL;
    }
    if (rv->duration_ms == 0 || rv->transition == FDK_REVEAL_NONE) {
        /* Duration 0 or the NONE transition: the flight is a snap
         * (the animator's own duration-0 contract) — land now. */
        rv->reveal = rv->revealed ? 1.0f : 0.0f;
        revealer_self_crop(rv);
        revealer_apply_child_visibility(rv);
        fdk_widget_child_layout_changed(rv->base.parent);
        if (rv->on_revealed != NULL) {
            rv->on_revealed(&rv->base, rv->revealed,
                            rv->on_revealed_data);
        }
        return;
    }
    rv->anim = fdk_widget_animate(&rv->base, rv->duration_ms,
                                  FDK_EASE_CUBIC_OUT,
                                  revealer_tick, revealer_done, rv);
}

static void revealer_tick(fdk_animation *anim, double eased,
                          void *user) {
    (void)anim;
    fdk_revealer *rv = user;
    fdk_f32 target = rv->revealed ? 1.0f : 0.0f;
    fdk_f32 from = rv->reveal_from;
    fdk_f32 now = from + (target - from) * (fdk_f32)eased;
    if (now < 0.0f) {
        now = 0.0f;
    } else if (now > 1.0f) {
        now = 1.0f;
    }
    if (now == rv->reveal) {
        return;
    }
    rv->reveal = now;
    revealer_self_crop(rv);
    /* The door moved: every ancestor's layout is stale — re-notify
     * (the paned/expander hooks ride the same notifier chain). */
    fdk_widget_child_layout_changed(rv->base.parent);
}

static void revealer_done(fdk_animation *anim, bool finished,
                          void *user) {
    (void)anim;
    (void)finished; /* both endings land somewhere honest */
    fdk_revealer *rv = user;
    if (rv->anim == anim) {
        rv->anim = NULL;
    }
    rv->reveal = rv->revealed ? 1.0f : 0.0f;
    revealer_self_crop(rv);
    revealer_apply_child_visibility(rv);
    fdk_widget_child_layout_changed(rv->base.parent);
    if (rv->on_revealed != NULL) {
        rv->on_revealed(&rv->base, rv->revealed, rv->on_revealed_data);
        /* The callback may have destroyed the widget. */
    }
}

static void revealer_paint(fdk_widget *w, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip) {
    (void)surface;
    (void)bounds;
    (void)clip;
    (void)w;
    /* The revealer paints nothing itself: the child (arranged at
     * full natural size, cropped by the door's bounds) IS the
     * content. A background would slide with the door — wrong — and
     * an app wanting a surface behind the content gives the child
     * one. */
}

/* ---- a11y ---- */

static const fdk_a11y_class revealer_a11y = {
    .role = FDK_A11Y_ROLE_PANEL,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_revealer_class_def = {
    .size = sizeof(fdk_revealer),
    .name = "revealer",
    .handle_event = NULL,
    .paint = revealer_paint,
    .measure = revealer_measure,
    .arrange = revealer_arrange,
    .destroy = NULL,
    .a11y = &revealer_a11y,
    .max_children = 1, /* the content IS the one slot */
};

fdk_result fdk_revealer_create(fdk_widget *parent,
                               fdk_widget **out_revealer) {
    if (out_revealer == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_revealer_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_revealer *rv = revealer_of(w);
    rv->transition = FDK_REVEAL_SLIDE_DOWN;
    rv->duration_ms = REVEALER_DEFAULT_MS;
    rv->revealed = false;
    rv->reveal = 0.0f;
    rv->reveal_from = 0.0f;
    rv->anim = NULL;
    *out_revealer = w;
    return FDK_OK;
}

void fdk_revealer_set_reveal_child(fdk_widget *revealer, bool reveal) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return;
    }
    fdk_revealer *rv = revealer_of(revealer);
    reveal = reveal ? true : false;
    if (rv->revealed == reveal) {
        return;
    }
    rv->revealed = reveal;
    /* Depart from wherever the door currently is. */
    rv->reveal_from = rv->reveal;
    /* Crop BEFORE the child turns visible: between here and the
     * first tick, a shown child would otherwise paint to the full
     * arranged bounds (the pop-in the expander's tests caught). */
    revealer_self_crop(rv);
    revealer_apply_child_visibility(rv);
    revealer_fly(rv);
    fdk_widget_invalidate(revealer);
}

bool fdk_revealer_get_reveal_child(fdk_widget *revealer) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return false;
    }
    return revealer_of(revealer)->revealed;
}

/* GTK's distinction: the TARGET (reveal-child) vs where the flight
 * actually is (child-revealed) — chains that tear down content on
 * "fully hidden" read THIS, not the target. */
bool fdk_revealer_get_child_revealed(fdk_widget *revealer) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return false;
    }
    return revealer_of(revealer)->reveal >= 1.0f;
}

void fdk_revealer_set_transition(fdk_widget *revealer,
                                 fdk_revealer_transition transition) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return;
    }
    switch (transition) {
    case FDK_REVEAL_NONE:
    case FDK_REVEAL_SLIDE_DOWN:
    case FDK_REVEAL_SLIDE_UP:
    case FDK_REVEAL_SLIDE_LEFT:
    case FDK_REVEAL_SLIDE_RIGHT:
        break;
    default:
        return;
    }
    fdk_revealer *rv = revealer_of(revealer);
    if (rv->transition == transition) {
        return;
    }
    if (rv->anim != NULL) {
        /* Refuse mid-flight: the blend's meaning would silently
         * change axes under the running tick. Wait for the landing
         * (or cancel by retargeting to the current state first). */
        return;
    }
    rv->transition = transition;
    if (transition == FDK_REVEAL_NONE) {
        /* NONE means instant: land wherever the target says. */
        rv->reveal = rv->revealed ? 1.0f : 0.0f;
    }
    revealer_self_crop(rv);
    revealer_apply_child_visibility(rv);
    fdk_widget_invalidate(revealer);
    fdk_widget_child_layout_changed(revealer->parent);
}

fdk_revealer_transition fdk_revealer_get_transition(
    fdk_widget *revealer) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return FDK_REVEAL_SLIDE_DOWN;
    }
    return revealer_of(revealer)->transition;
}

void fdk_revealer_set_transition_duration(fdk_widget *revealer,
                                          fdk_u32 duration_ms) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return;
    }
    if (duration_ms > 10000u) {
        return; /* a ten-second door is a wall, not a reveal */
    }
    revealer_of(revealer)->duration_ms = duration_ms;
}

fdk_u32 fdk_revealer_get_transition_duration(fdk_widget *revealer) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return REVEALER_DEFAULT_MS;
    }
    return revealer_of(revealer)->duration_ms;
}

void fdk_revealer_set_on_revealed(fdk_widget *revealer,
                                  fdk_revealer_revealed_fn on_revealed,
                                  void *user_data) {
    if (revealer == NULL ||
        revealer->klass != &fdk_revealer_class_def) {
        return;
    }
    fdk_revealer *rv = revealer_of(revealer);
    rv->on_revealed = on_revealed;
    rv->on_revealed_data = user_data;
}
