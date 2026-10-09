/*
 * expander.c — the disclosure section (1.4.1).
 *
 * GTK's GtkExpander / Qt's collapsible group: a header row (a
 * rotating disclosure triangle + label) that reveals or collapses
 * the ONE content child (max_children = 1 — a second child is a
 * loud INVALID_ARGUMENT). Activation: click anywhere on the header,
 * or Space/Enter while focused.
 *
 * THE REVEAL IS ANIMATED on the 1.3.8 animator: the header stays
 * put, the content slides out from under it. The mechanics: the
 * expander's measured height = header + reveal * content-natural;
 * the child is ALWAYS arranged at its full natural size below the
 * header, and the paint walk's clip stack (every widget clips to
 * its own bounds) crops it — the content never reflows while it
 * slides (rows don't re-wrap as the door opens; that is what makes
 * the motion read as a door, not a squeeze). Each tick re-notifies
 * the parent's layout so the growing expander pushes siblings
 * frame by frame. Standalone trees snap instantly — not because
 * the code path differs, but because nothing pumps the animator
 * without a clock (the headless seam drives it deterministically).
 *
 * While fully collapsed (reveal == 0) the child is FDK_WF_VISIBLE-
 * off: input-transparent, skipped by paints, out of the a11y tree's
 * SHOWING set — exactly as if it were not there.
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include <math.h>
#include <stdio.h>

/* Header band: the text line plus 8 px of breathing room, with a
 * 24 px floor (the triangle + air read right even fontless — a
 * fontless expander is still a clickable disclosure row). */
#define EXPANDER_HEADER_MIN 24

/* The reveal flight: 160 ms of cubic-out — fast enough to feel
 * like an answer, slow enough to be seen. */
#define EXPANDER_REVEAL_MS 160

/* Triangle geometry: a 10-px equilateral-ish glyph, 6 px in from
 * the left edge; the label follows 8 px after the glyph box. */
#define EXPANDER_TRI 10
#define EXPANDER_TRI_X 6
#define EXPANDER_LABEL_GAP 8

static fdk_i32 expander_header_h(const fdk_expander *e) {
    if (e->font == NULL) {
        return EXPANDER_HEADER_MIN;
    }
    fdk_i32 w = 0, h = 0;
    fdk__text_extent(e->font, "Ag", &w, &h);
    fdk_i32 band = h + 8;
    return (band < EXPANDER_HEADER_MIN) ? EXPANDER_HEADER_MIN : band;
}

/* The content child's natural size (0x0 when there is none).
 *
 * Deliberately INDEPENDENT of the child's FDK_WF_VISIBLE: the
 * expander owns that flag (hidden = collapsed door, not app
 * intent), and the child's slot must be laid at its FULL natural
 * height even while the door is shut — the clip stack crops the
 * reveal, the slot never re-flows mid-flight. Measuring through
 * the visibility would zero the slot of a just-collapsed child and
 * the door would open onto a starved layout. */
static void expander_content_natural(const fdk_expander *e,
                                     fdk_size *out) {
    out->width = 0;
    out->height = 0;
    if (e->base.child_count == 0) {
        return;
    }
    fdk_widget *child = e->base.children[0];
    if ((child->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_widget_measure(child, out);
}

static void expander_measure(fdk_widget *w, fdk_size *out) {
    fdk_expander *e = expander_of(w);
    fdk_i32 header = expander_header_h(e);
    fdk_size content;
    expander_content_natural(e, &content);

    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(e->font, e->label, &tw, &th);

    fdk_i32 width = EXPANDER_TRI_X + EXPANDER_TRI + EXPANDER_LABEL_GAP +
                    tw;
    /* The cross axis collapses with the door too: a fully collapsed
     * expander is the header alone (GTK parity); the content's
     * width counts from the first crack of the door open. */
    if (e->reveal > 0.0f && content.width > width) {
        width = content.width;
    }
    fdk_i32 shown = (fdk_i32)((fdk_f32)content.height * e->reveal + 0.5f);
    if (shown < 0) {
        shown = 0;
    }
    if (shown > content.height) {
        shown = content.height;
    }
    out->width = width;
    out->height = header + shown;
}

static void expander_reveal_tick(fdk_animation *anim, double eased,
                                 void *user);
static void expander_reveal_done(fdk_animation *anim, bool finished,
                                 void *user);
static void expander_apply_child_visibility(fdk_expander *e);

/* SELF-CROP: the door's clip is this widget's own bounds (the paint
 * walk crops every subtree to it). A container parent re-arranges
 * per tick anyway (the layout notifier), but a plain-parented or
 * hand-positioned expander would otherwise keep its roomy arranged
 * bounds and the door would not crop — the content would just POP
 * in. The door's bounds track header + reveal * content EXACTLY —
 * both directions (a crop that only shinks could never reopen). A
 * container parent re-asserts its own assignment on the notifier
 * chain the tick also fires, so an expand-filled expander keeps the
 * parent's word; a plain parent sees the door fly. Runs at every
 * reveal transition: set_expanded (BEFORE the child can paint
 * uncropped), each tick, and done. */
static void expander_self_crop(fdk_expander *e) {
    fdk_size content;
    expander_content_natural(e, &content);
    fdk_i32 want = expander_header_h(e) +
                   (fdk_i32)((fdk_f32)content.height * e->reveal + 0.5f);
    if (want != e->base.bounds.height) {
        fdk_rect door = e->base.bounds;
        door.height = want;
        fdk_widget_set_bounds(&e->base, door);
    }
}

static void expander_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_expander *e = expander_of(w);
    fdk_widget_set_bounds(w, assigned);

    /* Visibility follows the door at EVERY arrange: a child
     * ATTACHED to a collapsed expander starts hidden (the create
     * default is visible; this is the hook that catches it — the
     * layout notifier routes every child attach through here). */
    expander_apply_child_visibility(e);

    if (w->child_count == 0) {
        return;
    }
    fdk_i32 header = expander_header_h(e);
    /* The child always gets the FULL cross width and its natural
     * height — the reveal is a clip, not a squeeze (file header
     * comment). The expander's own bounds do the cropping. */
    fdk_size content;
    expander_content_natural(e, &content);
    fdk_rect slot = {0, header, assigned.width, content.height};
    fdk_widget_arrange(w->children[0], slot);
}

/* The layout notifier's hook (box.c): the expander's subtree
 * changed (child added / child's natural size changed) — re-run
 * the arrangement at the CURRENT bounds. */
void fdk__expander_layout_changed(fdk_widget *w) {
    if (w == NULL || w->klass != &fdk_expander_class_def) {
        return;
    }
    if ((w->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_rect cur = w->bounds;
    expander_arrange(w, cur);
}

static void expander_paint(fdk_widget *w, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_expander *e = expander_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_i32 header = expander_header_h(e);
    if (header > bounds.height) {
        header = bounds.height;
    }
    const bool disabled = (w->flags & FDK_WF_ENABLED) == 0;

    /* The header's hover/press affordance: a full-width rounded
     * pill in the dense-row hover fill (soft, modern), pressed
     * darkens to the control-pressed family. */
    if (!disabled && (e->hovering || e->pressed)) {
        fdk_rect pill = {bounds.x, bounds.y, bounds.width, header};
        fdk_color fill = e->pressed ? fdk__pal_control_pressed()
                                    : fdk__pal_row_hover();
        fdk_surface_fill_rounded_rect(surface, pill, 6, fill);
    }

    /* The disclosure chevron: pointing right when collapsed,
     * rotating to pointing-down when expanded — the SAME vector
     * glyph language as the file dialog's breadcrumb chevrons and
     * the combo's dropdown arrow. The rotation rides `reveal`
     * (the content flight's blend), so the glyph and the door move
     * as one (GTK's expander rotates through the animation, not at
     * its ends). */
    fdk_f32 t = e->reveal; /* 0 = right-pointing, 1 = down-pointing */
    fdk_i32 cx = bounds.x + EXPANDER_TRI_X + EXPANDER_TRI / 2;
    fdk_i32 cy = bounds.y + header / 2;
    fdk_f32 s = (fdk_f32)EXPANDER_TRI * 0.5f; /* half-extent */
    /* The right-chevron's three points (tip + two ends), rotated
     * by t * 90 degrees around the center. At t = 0: ">"; at
     * t = 1: "v" (the tip lands at (cx, cy + s/2)). */
    fdk_f32 ang = t * (float)(3.14159265358979323846 / 2.0);
    fdk_f32 ca = cosf(ang);
    fdk_f32 sa = sinf(ang);
    fdk_f32 pts[3][2] = {
        {s * 0.5f, 0.0f},          /* the tip (right at rest)  */
        {-s * 0.5f, -s},           /* upper end                */
        {-s * 0.5f, s},            /* lower end                */
    };
    fdk_i32 rx[3];
    fdk_i32 ry[3];
    for (int i = 0; i < 3; i++) {
        rx[i] = (fdk_i32)((fdk_f32)cx + pts[i][0] * ca - pts[i][1] * sa +
                          0.5f);
        ry[i] = (fdk_i32)((fdk_f32)cy + pts[i][0] * sa + pts[i][1] * ca +
                          0.5f);
    }
    fdk_color chev = disabled ? fdk__pal_text_disabled() : fdk__pal_text();
    fdk_surface_draw_line_aa(surface, rx[1], ry[1], rx[0], ry[0], chev);
    fdk_surface_draw_line_aa(surface, rx[0], ry[0], rx[2], ry[2], chev);

    /* The label, vertically centered in the header. */
    if (e->font != NULL && e->label != NULL) {
        fdk_i32 lx = bounds.x + EXPANDER_TRI_X + EXPANDER_TRI +
                     EXPANDER_LABEL_GAP;
        fdk_i32 baseline = fdk__center_baseline(e->font, bounds.y,
                                                header);
        fdk__draw_text(surface, e->font, e->label,
                       disabled ? fdk__pal_text_disabled()
                                : fdk__pal_text(),
                       lx, baseline);
    }

    /* Focus ring: the header is the focusable surface — a ring
     * around the header band (themed color/width, 1.4.0 rules). */
    if ((w->flags & FDK_WF_FOCUSED) != 0 && !disabled) {
        fdk_i32 fw = fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH);
        fdk_rect ring = {bounds.x + fw, bounds.y + fw,
                         bounds.width - fw * 2, header - fw * 2};
        if (ring.width > 0 && ring.height > 0) {
            fdk_surface_draw_rounded_rect(surface, ring, 6,
                                          fdk__pal_focus_ring());
        }
    }
}

/* ---- the reveal flight ---- */

static void expander_reveal_tick(fdk_animation *anim, double eased,
                                 void *user);
static void expander_reveal_done(fdk_animation *anim, bool finished,
                                 void *user);
static void expander_apply_child_visibility(fdk_expander *e);

/* Retargets the reveal animation from the CURRENT blend (cancel +
 * restart — the animator's documented compose rule; mid-flight
 * retargets never teleport). */
static void expander_fly(fdk_expander *e) {
    if (e->anim != NULL) {
        fdk_animation_cancel(e->anim);
        e->anim = NULL;
    }
    e->anim = fdk_widget_animate(&e->base, EXPANDER_REVEAL_MS,
                                 FDK_EASE_CUBIC_OUT,
                                 expander_reveal_tick,
                                 expander_reveal_done, e);
}

static void expander_reveal_tick(fdk_animation *anim, double eased,
                                 void *user) {
    (void)anim;
    fdk_expander *e = user;
    fdk_f32 target = e->expanded ? 1.0f : 0.0f;
    fdk_f32 from = e->reveal_from;
    fdk_f32 now = from + (target - from) * (fdk_f32)eased;
    if (now < 0.0f) {
        now = 0.0f;
    } else if (now > 1.0f) {
        now = 1.0f;
    }
    if (now == e->reveal) {
        return;
    }
    e->reveal = now;
    expander_self_crop(e);
    /* The door moved: the expander's measured height changed, so
     * every ancestor's layout is stale — re-notify (the box's own
     * child-change hook; the paned and this widget's hooks ride the
     * same notifier chain). */
    fdk_widget_child_layout_changed(e->base.parent);
}

static void expander_reveal_done(fdk_animation *anim, bool finished,
                                 void *user) {
    (void)anim;
    (void)finished; /* both endings land somewhere honest (below) */
    fdk_expander *e = user;
    if (e->anim == anim) {
        e->anim = NULL;
    }
    /* Snap to the exact end state on both natural completion and
     * cancel (a canceled reveal still lands somewhere honest), and
     * drop/park the child's visibility at the endpoints. */
    e->reveal = e->expanded ? 1.0f : 0.0f;
    expander_self_crop(e);
    expander_apply_child_visibility(e);
    fdk_widget_child_layout_changed(e->base.parent);
}

/* Child visibility follows the ENDPOINTS: hidden only at exactly
 * reveal 0 (input-transparent, skipped), shown the moment the door
 * starts opening. */
static void expander_apply_child_visibility(fdk_expander *e) {
    if (e->base.child_count == 0) {
        return;
    }
    fdk_widget *child = e->base.children[0];
    bool want = e->reveal > 0.0f || e->expanded;
    bool have = (child->flags & FDK_WF_VISIBLE) != 0;
    if (want != have) {
        fdk_widget_set_visible(child, want);
    }
}

/* ---- activation ---- */

static void expander_toggle(fdk_widget *w) {
    fdk_expander *e = expander_of(w);
    fdk_expander_set_expanded(w, !e->expanded);
}

static bool expander_handle_event(fdk_widget *w,
                                  const fdk_widget_event *ev) {
    fdk_expander *e = expander_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        if (ev->pointer.position.y < (fdk_f32)expander_header_h(e)) {
            e->pressed = true;
            fdk_widget_invalidate(w);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_UP: {
        if (!e->pressed) {
            return false;
        }
        e->pressed = false;
        fdk_widget_invalidate(w);
        /* Release inside the header toggles (the implicit grab
         * delivered the release even if the pointer left). */
        if (ev->pointer.position.y >= 0.0f &&
            ev->pointer.position.y < (fdk_f32)expander_header_h(e) &&
            (w->flags & FDK_WF_ENABLED) != 0) {
            expander_toggle(w);
            /* The callback may have destroyed the widget — the
             * toggle path (set_expanded) re-enters user code. */
        }
        return true;
    }
    case FDK_WIDGET_KEY_DOWN:
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        if (ev->key.scancode == FDK_KEY_SPACE ||
            ev->key.scancode == FDK_KEY_ENTER) {
            expander_toggle(w);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_ENTER:
        if ((w->flags & FDK_WF_ENABLED) != 0 && !e->hovering) {
            e->hovering = true;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        if (e->hovering) {
            e->hovering = false;
            fdk_widget_invalidate(w);
        }
        return false;
    default:
        return false;
    }
}

static void expander_destroy(fdk_widget *w) {
    fdk_expander *e = expander_of(w);
    /* The animator drops the animation itself when the widget dies
     * (no done() fires — the documented funeral rule); the handle
     * pointer just goes dangling into freed memory, which is fine
     * because nothing reads it after destroy. */
    fdk_free(e->label);
}

/* ---- a11y ---- */

static void expander_a11y_describe(const fdk_widget *w,
                                   fdk_a11y_info *out) {
    const fdk_expander *e = (const fdk_expander *)(const void *)w;
    if (e->label != NULL) {
        out->name = fdk__strdup(e->label);
    }
    if (e->expanded) {
        out->states |= FDK_A11Y_EXPANDED;
    }
}

static bool expander_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                  double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    if ((w->flags & FDK_WF_ENABLED) == 0) {
        return false;
    }
    expander_toggle(w);
    return true;
}

static fdk_a11y_action_set expander_a11y_actions(const fdk_widget *w) {
    (void)w;
    return (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE;
}

static const fdk_a11y_class expander_a11y = {
    .role = FDK_A11Y_ROLE_EXPANDER,
    .describe = expander_a11y_describe,
    .actions = expander_a11y_actions,
    .perform = expander_a11y_perform,
};

const fdk_widget_class fdk_expander_class_def = {
    .size = sizeof(fdk_expander),
    .name = "expander",
    .handle_event = expander_handle_event,
    .paint = expander_paint,
    .measure = expander_measure,
    .arrange = expander_arrange,
    .destroy = expander_destroy,
    .a11y = &expander_a11y,
    .max_children = 1, /* the content IS the one slot */
};

fdk_result fdk_expander_create(fdk_widget *parent, fdk_font *font,
                               const char *label,
                               fdk_widget **out_expander) {
    if (out_expander == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_expander_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_expander *e = expander_of(w);
    e->font = font;
    e->label = fdk__strdup(label);
    if (label != NULL && e->label == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    e->expanded = false;
    e->reveal = 0.0f;
    e->reveal_from = 0.0f;
    e->anim = NULL;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_expander = w;
    return FDK_OK;
}

fdk_result fdk_expander_set_label(fdk_widget *expander,
                                  const char *label) {
    if (expander == NULL || expander->klass != &fdk_expander_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_expander *e = expander_of(expander);
    char *copy = fdk__strdup(label);
    if (label != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(e->label);
    e->label = copy;
    fdk_widget_invalidate(expander);
    fdk_widget_child_layout_changed(expander->parent);
    fdk__a11y_notify(expander, FDK_A11Y_NAME_CHANGED, 0);
    return FDK_OK;
}

const char *fdk_expander_get_label(fdk_widget *expander) {
    if (expander == NULL || expander->klass != &fdk_expander_class_def) {
        return NULL;
    }
    return expander_of(expander)->label;
}

void fdk_expander_set_expanded(fdk_widget *expander, bool expanded) {
    if (expander == NULL || expander->klass != &fdk_expander_class_def) {
        return;
    }
    fdk_expander *e = expander_of(expander);
    expanded = expanded ? true : false;
    if (e->expanded == expanded) {
        return;
    }
    e->expanded = expanded;
    /* The flight departs from wherever the door currently is. */
    e->reveal_from = e->reveal;
    /* Crop BEFORE the child turns visible: between here and the
     * first tick, a shown child would otherwise paint to the full
     * arranged bounds (the pop-in the tests caught live). */
    expander_self_crop(e);
    expander_apply_child_visibility(e);
    expander_fly(e);
    fdk_widget_invalidate(expander);
    fdk__a11y_notify(expander, FDK_A11Y_STATE_CHANGED,
                     FDK_A11Y_EXPANDED);
    if (e->on_changed != NULL) {
        e->on_changed(expander, expanded, e->on_changed_data);
        /* The callback may have destroyed the widget. */
    }
}

bool fdk_expander_is_expanded(fdk_widget *expander) {
    if (expander == NULL || expander->klass != &fdk_expander_class_def) {
        return false;
    }
    return expander_of(expander)->expanded;
}

void fdk_expander_set_on_changed(fdk_widget *expander,
                                 fdk_expander_changed_fn on_changed,
                                 void *user_data) {
    if (expander == NULL || expander->klass != &fdk_expander_class_def) {
        return;
    }
    fdk_expander *e = expander_of(expander);
    e->on_changed = on_changed;
    e->on_changed_data = user_data;
}
