/* test_widget.c — headless widget-foundation tests.
 *
 * Everything here runs against STANDALONE root widgets (parent = NULL)
 * and OFFSCREEN surfaces — no display, no backend, no window. That is
 * the point of the widget layer's design: hierarchy, state, focus,
 * event routing, invalidation, and painting are all tree-level
 * concerns with zero platform dependence, so they must be provable
 * with nothing but memory (the X11 integration test separately proves
 * the window glue: real XSendEvent input routed into a real window's
 * tree, and server-side pixel readback of tree painting).
 *
 * The event-injection entry point is the same public
 * fdk_widget_tree_handle_event() the window glue calls — what these
 * tests exercise is exactly what a real display exercises.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widget.h"

/* The subclassing test below embeds fdk_widget as its first member —
 * the same pattern FDK's own widget implementations (Phase 5/6,
 * under src/) use, which is why it needs the INTERNAL header: the
 * public API keeps fdk_widget opaque (see docs/abi-policy.md), so
 * embedding is available inside the library and its test suite, not
 * to applications. */
#include "widget/widget_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- helpers ---- */

static fdk_color rgb(int r, int g, int b) {
    fdk_color c = { .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f, .b = (fdk_f32)b / 255.0f,
                    .a = 1.0f };
    return c;
}

static fdk_u32 pack(int r, int g, int b) {
    return ((fdk_u32)r << 16) | ((fdk_u32)g << 8) | (fdk_u32)b;
}

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

static bool px_is(fdk_surface *s, int x, int y, int r, int g, int b) {
    return px_at(s, x, y) == pack(r, g, b);
}

static fdk_widget *mk_widget(fdk_widget *parent, int x, int y, int w, int h) {
    fdk_widget *wid = NULL;
    fdk_rect b = {x, y, w, h};
    assert(fdk_ok(fdk_widget_create(parent, NULL, b, &wid)));
    return wid;
}

/* event builders (window-event shapes, root coordinates) */
static fdk_event_data ev_motion(float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_POINTER_MOTION;
    e.pointer.position.x = x;
    e.pointer.position.y = y;
    return e;
}

static fdk_event_data ev_button(fdk_event_type t, float x, float y,
                                fdk_u32 button) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = button;
    return e;
}

static fdk_event_data ev_key(fdk_event_type t, fdk_scancode sc,
                             fdk_u32 mods) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.key.scancode = sc;
    e.key.modifiers = mods;
    return e;
}

static fdk_event_data ev_window_focus(int focused) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_WINDOW_FOCUS;
    e.focus.focused = focused;
    return e;
}

/* per-widget event recorder */
typedef struct recorder {
    int enter, leave, motion, down, up, scroll, key_down, key_up;
    int focus_in, focus_out;
    fdk_pointf last_local;
    bool handle;      /* whether the callback claims "handled" */
    fdk_widget *self; /* set on first event, checked for staleness */
} recorder;

static bool record_event(fdk_widget *w, const fdk_widget_event *e,
                         void *ud) {
    recorder *r = ud;
    r->self = w;
    switch (e->type) {
        case FDK_WIDGET_POINTER_ENTER: r->enter++; break;
        case FDK_WIDGET_POINTER_LEAVE: r->leave++; break;
        case FDK_WIDGET_POINTER_MOTION:
            r->motion++;
            r->last_local = e->position;
            break;
        case FDK_WIDGET_POINTER_DOWN:
            r->down++;
            r->last_local = e->pointer.position;
            break;
        case FDK_WIDGET_POINTER_UP:
            r->up++;
            r->last_local = e->pointer.position;
            break;
        case FDK_WIDGET_SCROLL: r->scroll++; break;
        case FDK_WIDGET_KEY_DOWN: r->key_down++; break;
        case FDK_WIDGET_KEY_UP: r->key_up++; break;
        case FDK_WIDGET_FOCUS_IN: r->focus_in++; break;
        case FDK_WIDGET_FOCUS_OUT: r->focus_out++; break;
    }
    return r->handle;
}

/* subclass machinery for lifecycle/measure tests */
typedef struct counted_widget {
    fdk_widget base;
    int *destroy_count;
    int measure_w, measure_h;
} counted_widget;

static void counted_destroy(fdk_widget *w) {
    counted_widget *cw = (counted_widget *)w;
    (*cw->destroy_count)++;
}

static void counted_measure(fdk_widget *w, fdk_size *out) {
    counted_widget *cw = (counted_widget *)w;
    out->width = cw->measure_w;
    out->height = cw->measure_h;
}

static const fdk_widget_class counted_class = {
    .size = sizeof(counted_widget),
    .name = "counted",
    .handle_event = NULL,
    .paint = NULL,
    .measure = counted_measure,
    .arrange = NULL,
    .destroy = counted_destroy,
};

static fdk_widget *mk_counted(fdk_widget *parent, int x, int y, int w,
                              int h, int *destroy_count) {
    fdk_widget *wid = NULL;
    fdk_rect b = {x, y, w, h};
    assert(fdk_ok(fdk_widget_create(parent, &counted_class, b, &wid)));
    counted_widget *cw = (counted_widget *)wid;
    cw->destroy_count = destroy_count;
    cw->measure_w = w;
    cw->measure_h = h;
    return wid;
}

/* ---- lifecycle & hierarchy ---- */

static void test_create_hierarchy_destroy(void) {
    int destroyed = 0;
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_counted(root, 0, 0, 100, 50, &destroyed);
    fdk_widget *b = mk_counted(root, 100, 0, 100, 50, &destroyed);
    fdk_widget *a1 = mk_counted(a, 10, 10, 30, 30, &destroyed);
    (void)b;

    assert(fdk_widget_parent(root) == NULL);
    assert(fdk_widget_parent(a) == root);
    assert(fdk_widget_child_count(root) == 2);
    assert(fdk_widget_child_at(root, 0) == a);   /* z-order: a bottom  */
    assert(fdk_widget_child_at(root, 1) == b);   /* b top              */
    assert(fdk_widget_child_at(root, 2) == NULL);
    assert(fdk_widget_child_count(a) == 1);
    assert(fdk_widget_parent(a1) == a);
    assert(fdk_widget_is_root(root));
    assert(!fdk_widget_is_root(a));

    /* absolute bounds compose up the chain */
    fdk_rect abs_a1 = fdk_widget_get_absolute_bounds(a1);
    assert(abs_a1.x == 10 && abs_a1.y == 10);
    assert(abs_a1.width == 30 && abs_a1.height == 30);

    /* destroying the middle of the tree frees the whole subtree */
    fdk_widget_destroy(a);
    assert(fdk_widget_child_count(root) == 1);
    assert(fdk_widget_child_at(root, 0) == b);
    assert(destroyed == 2); /* a and a1 (b survives) */

    /* destroying the root frees everything left */
    fdk_widget_destroy(root);
    assert(destroyed == 3);

    /* NULL is the documented safe no-op. (Destroying an ALREADY-FREED
     * pointer is ordinary C UB, like double free() — not a contract;
     * the mid-destruction no-op is exercised in the destroy-during-
     * dispatch test, where a DESTROYING-but-deferred widget is
     * destroyed a second time and must be ignored.) */
    fdk_widget_destroy(NULL);

    printf("[ok] create/hierarchy/absolute-bounds/destroy-cascade "
           "(subclass destroy hook ran %dx)\n", destroyed);
}

static void test_argument_checks(void) {
    fdk_widget *w = NULL;
    assert(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 10, 10},
                             NULL) == FDK_ERR_INVALID_ARGUMENT);

    /* class too small for the base struct */
    static const fdk_widget_class tiny = {
        .size = 8, .name = "tiny",
    };
    assert(fdk_widget_create(NULL, &tiny, (fdk_rect){0, 0, 1, 1}, &w) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(w == NULL);

    /* (The "attaching to a destroying parent is refused" contract is
     * exercised in test_destroy_during_dispatch against a
     * DESTROYING-but-still-allocated (deferred) parent — the only
     * state in which touching a destroying widget is defined; a
     * freed pointer is ordinary C UB, not a testable contract.) */
    fdk_widget *root = mk_widget(NULL, 0, 0, 10, 10);

    /* user data / naming round-trip */
    int sentinel = 42;
    fdk_widget_set_user_data(root, &sentinel);
    assert(fdk_widget_get_user_data(root) == &sentinel);
    assert(fdk_widget_get_name(root) != NULL); /* base class name */
    fdk_widget_set_name(root, "panel");
    assert(strcmp(fdk_widget_get_name(root), "panel") == 0);
    fdk_widget_set_name(root, NULL);

    fdk_widget_destroy(root);
    printf("[ok] argument checks, user data, naming\n");
}

static void test_reparent_raise_lower(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_widget(root, 0, 0, 100, 100);
    fdk_widget *b = mk_widget(root, 100, 0, 100, 100);
    fdk_widget *c = mk_widget(a, 10, 10, 40, 40);

    /* reparent c from a to b: absolute bounds shift with the chain */
    assert(fdk_ok(fdk_widget_reparent(c, b)));
    assert(fdk_widget_child_count(a) == 0);
    assert(fdk_widget_child_count(b) == 1);
    assert(fdk_widget_parent(c) == b);
    fdk_rect abs = fdk_widget_get_absolute_bounds(c);
    assert(abs.x == 110 && abs.y == 10); /* b is at (100,0) */

    /* cycle refusal: b under c */
    assert(fdk_widget_reparent(b, c) == FDK_ERR_INVALID_ARGUMENT);
    /* root reparent refusal */
    assert(fdk_widget_reparent(root, a) == FDK_ERR_INVALID_ARGUMENT);
    /* self reparent refusal */
    assert(fdk_widget_reparent(c, c) == FDK_ERR_INVALID_ARGUMENT);

    /* raise/lower flip z-order within the parent */
    assert(fdk_widget_child_at(root, 0) == a &&
           fdk_widget_child_at(root, 1) == b);
    fdk_widget_lower(b);
    assert(fdk_widget_child_at(root, 0) == b &&
           fdk_widget_child_at(root, 1) == a);
    fdk_widget_raise(b);
    assert(fdk_widget_child_at(root, 0) == a &&
           fdk_widget_child_at(root, 1) == b);
    /* idempotent raise of the already-top widget */
    fdk_widget_raise(b);
    assert(fdk_widget_child_at(root, 1) == b);

    fdk_widget_destroy(root);
    printf("[ok] reparent (incl. cycle/root refusal), raise/lower\n");
}

/* ---- state ---- */

static void test_visibility_enabled(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *panel = mk_widget(root, 0, 0, 100, 100);
    fdk_widget *leaf = mk_widget(panel, 10, 10, 50, 50);

    assert(fdk_widget_get_visible(root) &&
           fdk_widget_is_effectively_visible(leaf));
    fdk_widget_set_visible(panel, false);
    assert(!fdk_widget_get_visible(panel));
    assert(fdk_widget_get_visible(leaf));          /* own flag intact */
    assert(!fdk_widget_is_effectively_visible(leaf)); /* chain ANDed */
    assert(!fdk_widget_is_effectively_visible(panel));
    fdk_widget_set_visible(panel, true);
    assert(fdk_widget_is_effectively_visible(leaf));

    fdk_widget_set_enabled(panel, false);
    assert(fdk_widget_get_enabled(leaf));            /* own flag intact */
    assert(!fdk_widget_is_effectively_enabled(leaf));
    fdk_widget_set_enabled(panel, true);
    assert(fdk_widget_is_effectively_enabled(leaf));

    /* hidden/disabled widgets are input-transparent (hit-test skips):
     * a motion over the hidden panel lands on the ROOT instead.
     * (5,5) is inside the panel but outside its child leaf (10..60),
     * so the hit target is the panel; unhandled events then bubble to
     * the root — both recorders legitimately see it. */
    recorder rr, pr;
    memset(&rr, 0, sizeof(rr));
    memset(&pr, 0, sizeof(pr));
    fdk_widget_set_event_callback(root, record_event, &rr);
    fdk_widget_set_event_callback(panel, record_event, &pr);

    fdk_event_data m = ev_motion(5, 5);
    assert(!fdk_widget_tree_handle_event(root, &m)); /* nobody handled */
    assert(pr.motion == 1);   /* panel was the hit target             */
    assert(rr.motion == 1);   /* unhandled -> bubbled up to root      */

    fdk_widget_set_visible(panel, false);
    m = ev_motion(5, 5);
    assert(!fdk_widget_tree_handle_event(root, &m));
    assert(pr.motion == 1);   /* unchanged: hidden panel skipped      */
    assert(rr.motion == 2);   /* root was the hit target now          */

    fdk_widget_set_enabled(panel, false);
    fdk_widget_set_visible(panel, true);
    m = ev_motion(5, 5);
    assert(!fdk_widget_tree_handle_event(root, &m));
    assert(pr.motion == 1);   /* disabled panel: also skipped */
    assert(rr.motion == 3);

    fdk_widget_destroy(root);
    printf("[ok] visibility/enabled chains + input pass-through\n");
}

/* ---- painting ---- */

static void test_paint_zorder_and_clipping(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 100, &s)));

    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget_set_background(root, rgb(20, 20, 20));
    fdk_widget *bottom = mk_widget(root, 10, 10, 100, 80);
    fdk_widget_set_background(bottom, rgb(200, 40, 40));
    fdk_widget *top = mk_widget(root, 60, 30, 100, 60); /* overlaps bottom */
    fdk_widget_set_background(top, rgb(40, 200, 40));
    fdk_widget *child_of_bottom = mk_widget(bottom, 20, 20, 30, 30);
    fdk_widget_set_background(child_of_bottom, rgb(40, 40, 200));
    /* sticks out of its parent's BOTTOM edge: must be clipped at
     * paint time (bottom spans y 10..90; poke claims y 75..135) */
    fdk_widget *poke = mk_widget(bottom, 5, 65, 60, 60);
    fdk_widget_set_background(poke, rgb(250, 250, 0));

    fdk_widget_tree_paint(root, s);

    /* z-order: later sibling over earlier, children over parents */
    assert(px_is(s, 15, 15, 200, 40, 40));    /* bottom over root     */
    assert(px_is(s, 150, 80, 40, 200, 40));   /* top over root        */
    assert(px_is(s, 100, 50, 40, 200, 40));   /* overlap: top wins    */
    assert(px_is(s, 32, 32, 40, 40, 200));    /* child over bottom    */

    /* poke: visible slice (x 15..60, y 75..90) escapes top's rect
     * (top spans x 60..160) — the rest is covered by top or clipped
     * away by bottom */
    assert(px_is(s, 30, 80, 250, 250, 0));    /* poke's free slice    */
    assert(px_is(s, 72, 80, 40, 200, 40));    /* top covers poke      */
    assert(px_is(s, 30, 95, 20, 20, 20));     /* past bottom's bottom:
                                                 poke clipped away    */

    /* hidden subtrees paint nothing */
    fdk_widget_set_visible(bottom, false);
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);
    assert(px_is(s, 15, 15, 20, 20, 20));     /* bottom + children gone */
    assert(px_is(s, 150, 80, 40, 200, 40));   /* top unaffected         */

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] paint: z-order, parent clipping, hidden subtree\n");
}

static void test_paint_damage_partial(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 100, &s)));

    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget_set_background(root, rgb(10, 10, 10));
    fdk_widget *left = mk_widget(root, 0, 0, 100, 100);
    fdk_widget_set_background(left, rgb(200, 30, 30));
    fdk_widget *right = mk_widget(root, 100, 0, 100, 100);
    fdk_widget_set_background(right, rgb(30, 30, 200));

    fdk_widget_tree_paint(root, s);
    assert(!fdk_widget_tree_has_damage(root));
    /* close the first frame: resets the SURFACE's damage (the tree's
     * damage was reset by the paint) — this is the frame boundary the
     * application's present would be in a rendered app. */
    assert(fdk_ok(fdk_surface_present(s)));

    /* marker pixels: outside (150,50) vs inside (50,50) the region we
     * are about to damage. Written raw; only invalidate() declares
     * damage — the raw-write contract from fdk_surface.h. */
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    info.pixels[(size_t)50 * (size_t)info.stride + (size_t)150] =
        pack(1, 1, 1);
    info.pixels[(size_t)50 * (size_t)info.stride + (size_t)50] =
        pack(2, 2, 2);

    /* change ONLY left's color: damage must bound to left's bounds */
    fdk_widget_set_background(left, rgb(30, 200, 30));
    assert(fdk_widget_tree_has_damage(root));
    fdk_widget_tree_paint(root, s);

    /* what did the surface actually record (i.e. what would go over
     * the wire)? Exactly the damaged region — no more. */
    fdk_rect dmg;
    assert(fdk_surface_get_damage_bounds(s, &dmg));
    assert(dmg.x == 0 && dmg.y == 0 && dmg.width == 100 && dmg.height == 100);

    /* repainted region: new left color; raw marker inside it gone */
    assert(px_is(s, 50, 50, 30, 200, 30));
    /* untouched region: right's color INTACT, raw marker SURVIVED —
     * proof the repaint never touched those pixels */
    assert(px_at(s, 150, 50) == pack(1, 1, 1));
    assert(px_is(s, 150, 20, 30, 30, 200));

    /* clean frame: close frame 2, then paint again with no tree damage
     * -> neither tree nor surface records anything (a no-op paint). */
    assert(fdk_ok(fdk_surface_present(s)));
    fdk_widget_tree_paint(root, s);
    assert(!fdk_surface_get_damage_bounds(s, &dmg));
    assert(!fdk_widget_tree_has_damage(root));

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] damage-driven partial repaint (bounds + untouched "
           "pixels survive)\n");
}

static void test_move_invalidates_both_regions(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 100, &s)));
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget_set_background(root, rgb(0, 0, 0));
    fdk_widget *mover = mk_widget(root, 10, 10, 40, 40);
    fdk_widget_set_background(mover, rgb(255, 255, 255));

    fdk_widget_tree_paint(root, s);
    assert(px_is(s, 30, 30, 255, 255, 255));

    /* move: BOTH the old and new regions must repaint (old area back
     * to root background, new area the widget's) */
    fdk_rect nb = {100, 50, 40, 40};
    fdk_widget_set_bounds(mover, nb);
    fdk_widget_tree_paint(root, s);
    assert(px_is(s, 30, 30, 0, 0, 0));          /* old region restored */
    assert(px_is(s, 120, 70, 255, 255, 255));   /* new region drawn    */

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] set_bounds invalidates old + new regions\n");
}

/* ---- events: pointer ---- */

static void test_pointer_hover_motion(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_widget(root, 0, 0, 100, 50);     /* bottom-left  */
    fdk_widget *b = mk_widget(root, 100, 0, 100, 50);   /* bottom-right */

    recorder rr, ra, rb;
    memset(&rr, 0, sizeof(rr));
    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    /* a and b CONSUME their events (handle = true), so root's
     * recorder only sees events where the ROOT itself is the hit
     * target — bubbling stops at a/b. */
    ra.handle = true;
    rb.handle = true;
    fdk_widget_set_event_callback(root, record_event, &rr);
    fdk_widget_set_event_callback(a, record_event, &ra);
    fdk_widget_set_event_callback(b, record_event, &rb);

    /* motion into a: ENTER + MOTION with widget-LOCAL coordinates */
    fdk_event_data m = ev_motion(10, 20);
    assert(fdk_widget_tree_handle_event(root, &m)); /* handled by a */
    assert(ra.enter == 1 && ra.motion == 1);
    assert(ra.last_local.x == 10.0f && ra.last_local.y == 20.0f);
    assert(fdk_widget_is_hovered(a));
    assert(rr.motion == 0); /* nothing bubbled: a consumed it */

    /* second motion inside a: no new ENTER */
    m = ev_motion(20, 20);
    assert(fdk_widget_tree_handle_event(root, &m));
    assert(ra.enter == 1 && ra.motion == 2);

    /* cross into b: a LEAVE, b ENTER+MOTION (b-local coords) */
    m = ev_motion(150, 25);
    assert(fdk_widget_tree_handle_event(root, &m));
    assert(ra.leave == 1);
    assert(rb.enter == 1 && rb.motion == 1);
    assert(rb.last_local.x == 50.0f && rb.last_local.y == 25.0f);
    assert(!fdk_widget_is_hovered(a) && fdk_widget_is_hovered(b));

    /* onto bare root background: b LEAVE (consumed), root MOTION —
     * root does not handle, so the window event is unhandled */
    m = ev_motion(100, 75);
    assert(!fdk_widget_tree_handle_event(root, &m));
    assert(rb.leave == 1);
    assert(rr.motion == 1);
    assert(fdk_widget_is_hovered(root));

    /* outside the root entirely: hover cleared, nobody gets motion */
    m = ev_motion(250, 75);
    assert(!fdk_widget_tree_handle_event(root, &m));
    assert(rr.leave == 1 && rr.motion == 1);
    assert(!fdk_widget_is_hovered(root));

    /* window ENTER event establishes hover (a handled its ENTER) */
    m = ev_motion(10, 20);
    assert(fdk_widget_tree_handle_event(root, &m));
    assert(ra.enter == 2);
    /* window LEAVE event does the same as out-of-root motion */
    fdk_event_data wl;
    memset(&wl, 0, sizeof(wl));
    wl.type = FDK_EVENT_POINTER_LEAVE;
    assert(fdk_widget_tree_handle_event(root, &wl)); /* a handled LEAVE */
    assert(ra.leave == 2);
    assert(!fdk_widget_is_hovered(a));

    fdk_widget_destroy(root);
    printf("[ok] pointer: hover enter/leave synthesis, local coords\n");
}

static void test_pointer_grab(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_widget(root, 0, 0, 100, 100);
    fdk_widget *b = mk_widget(root, 100, 0, 100, 100);

    recorder ra;
    memset(&ra, 0, sizeof(ra));
    fdk_widget_set_event_callback(a, record_event, &ra);

    /* press on a, move onto b, release: ALL of it goes to a (implicit
     * grab), with the release's coords in a-local space (negative/off-
     * bounds local coordinates are legal for grabbed releases) */
    fdk_event_data dn = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 50, 50, 1);
    (void)fdk_widget_tree_handle_event(root, &dn);
    assert(ra.down == 1);

    fdk_event_data m = ev_motion(150, 60); /* over b now */
    (void)fdk_widget_tree_handle_event(root, &m);
    assert(ra.motion == 1);
    assert(ra.last_local.x == 150.0f && ra.last_local.y == 60.0f);

    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 150, 60, 1);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(ra.up == 1);
    assert(ra.last_local.x == 150.0f &&
           ra.last_local.y == 60.0f); /* a is at the origin: a-local
                                         == root-local */

    /* grab released: motion over b now goes to b (root here, b has no
     * recorder — verify via a NOT receiving it) */
    m = ev_motion(150, 60);
    (void)fdk_widget_tree_handle_event(root, &m);
    assert(ra.motion == 1);

    /* press with nothing under the pointer: no grab, unhandled */
    fdk_event_data miss = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 250, 60, 1);
    assert(!fdk_widget_tree_handle_event(root, &miss));

    /* press on a then a second press elsewhere: still grabbed by a */
    dn = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 50, 50, 1);
    (void)fdk_widget_tree_handle_event(root, &dn);
    dn = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 150, 60, 3);
    (void)fdk_widget_tree_handle_event(root, &dn);
    assert(ra.down == 3); /* both presses delivered to the grab target */

    (void)b;
    fdk_widget_destroy(root);
    printf("[ok] pointer: implicit grab, press-to-release pairing\n");
}

static void test_event_bubbling(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *panel = mk_widget(root, 100, 0, 100, 100); /* abs (100,0) */
    fdk_widget *leaf = mk_widget(panel, 10, 20, 50, 50);   /* abs (110,20)*/

    recorder rr, rp, rl;
    memset(&rr, 0, sizeof(rr));
    memset(&rp, 0, sizeof(rp));
    memset(&rl, 0, sizeof(rl));
    fdk_widget_set_event_callback(root, record_event, &rr);
    fdk_widget_set_event_callback(panel, record_event, &rp);
    fdk_widget_set_event_callback(leaf, record_event, &rl);

    /* leaf doesn't handle -> bubbles to panel (panel-local coords),
     * then to root (root-local coords) */
    fdk_event_data m = ev_motion(120, 30);
    (void)fdk_widget_tree_handle_event(root, &m);
    assert(rl.motion == 1 && rp.motion == 1 && rr.motion == 1);
    assert(rl.last_local.x == 10.0f && rl.last_local.y == 10.0f);
    assert(rp.last_local.x == 20.0f && rp.last_local.y == 30.0f);
    assert(rr.last_local.x == 120.0f && rr.last_local.y == 30.0f);

    /* leaf handles -> nobody below sees it */
    memset(&rr, 0, sizeof(rr));
    memset(&rp, 0, sizeof(rp));
    rl.handle = true;
    m = ev_motion(120, 30);
    assert(fdk_widget_tree_handle_event(root, &m)); /* consumed by leaf */
    assert(rl.motion == 2 && rp.motion == 0 && rr.motion == 0);
    rl.handle = false;

    /* handled by the CLASS hook also stops bubbling */
    fdk_widget_destroy(root);
    printf("[ok] event bubbling: chain order, per-level local coords, "
           "handled stops propagation\n");
}

static void test_scroll(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_widget(root, 0, 0, 100, 100);

    recorder ra;
    memset(&ra, 0, sizeof(ra));
    fdk_widget_set_event_callback(a, record_event, &ra);

    fdk_event_data sc;
    memset(&sc, 0, sizeof(sc));
    sc.type = FDK_EVENT_POINTER_SCROLL;
    sc.scroll.position.x = 50;
    sc.scroll.position.y = 50;
    sc.scroll.delta_y = -3.0f;
    (void)fdk_widget_tree_handle_event(root, &sc);
    assert(ra.scroll == 1);

    sc.scroll.position.x = 150; /* over root, not a */
    (void)fdk_widget_tree_handle_event(root, &sc);
    assert(ra.scroll == 1);

    fdk_widget_destroy(root);
    printf("[ok] scroll routing to hit target\n");
}

/* ---- focus & keyboard ---- */

static void test_focus_lifecycle(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_widget(root, 0, 0, 100, 100);
    fdk_widget *b = mk_widget(root, 100, 0, 100, 100);
    fdk_widget_set_can_focus(a, true);
    fdk_widget_set_can_focus(b, true);

    recorder ra, rb;
    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    fdk_widget_set_event_callback(a, record_event, &ra);
    fdk_widget_set_event_callback(b, record_event, &rb);

    /* non-focusable by default */
    assert(!fdk_widget_focus(root));
    assert(fdk_widget_focus(a));
    assert(ra.focus_in == 1);
    assert(fdk_widget_has_focus(a));
    assert(fdk_widget_tree_get_focused(root) == a);

    /* refocusing the same widget: no events */
    assert(fdk_widget_focus(a));
    assert(ra.focus_in == 1);

    /* switch focus: OUT then IN */
    assert(fdk_widget_focus(b));
    assert(ra.focus_out == 1 && rb.focus_in == 1);
    assert(!fdk_widget_has_focus(a) && fdk_widget_has_focus(b));

    /* hiding the focused widget drops focus (with FOCUS_OUT). The
     * earlier switch-away OUT went to a; this is b's first. */
    fdk_widget_set_visible(b, false);
    assert(rb.focus_out == 1);
    assert(fdk_widget_tree_get_focused(root) == NULL);

    /* disabling also drops focus (a's 2nd OUT: 1st was the switch
     * to b) */
    assert(fdk_widget_focus(a));
    fdk_widget_set_enabled(a, false);
    assert(ra.focus_out == 2);
    assert(fdk_widget_tree_get_focused(root) == NULL);
    fdk_widget_set_enabled(a, true);

    /* clearing can_focus drops focus */
    assert(fdk_widget_focus(a));
    fdk_widget_set_can_focus(a, false);
    assert(ra.focus_out == 3);
    assert(!fdk_widget_focus(a)); /* and re-focus now fails */
    fdk_widget_set_can_focus(a, true);

    /* window focus loss/regain mirrors into the focused widget */
    assert(fdk_widget_focus(a));
    fdk_event_data blur = ev_window_focus(0);
    assert(!fdk_widget_tree_handle_event(root, &blur)); /* not consumed */
    assert(ra.focus_out == 4);
    assert(fdk_widget_tree_get_focused(root) == a); /* tree keeps it */

    fdk_event_data focus_ev = ev_window_focus(1);
    assert(!fdk_widget_tree_handle_event(root, &focus_ev));
    assert(ra.focus_in == 5);

    /* explicit clear */
    fdk_widget_tree_clear_focus(root);
    assert(ra.focus_out == 5);
    assert(fdk_widget_tree_get_focused(root) == NULL);

    fdk_widget_destroy(root);
    printf("[ok] focus: eligibility, switching, drop on hide/disable, "
           "window blur/regain, clear\n");
}

static void test_key_routing_and_tab(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_widget(root, 0, 0, 100, 100);
    fdk_widget *b = mk_widget(root, 100, 0, 100, 100);
    fdk_widget_set_can_focus(a, true);
    fdk_widget_set_can_focus(b, true);

    recorder ra, rb;
    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    fdk_widget_set_event_callback(a, record_event, &ra);
    fdk_widget_set_event_callback(b, record_event, &rb);

    /* no focus: keys are unhandled */
    fdk_event_data k = ev_key(FDK_EVENT_KEY_DOWN, 30, 0);
    assert(!fdk_widget_tree_handle_event(root, &k));

    /* focused widget receives keys; unhandled keys bubble (a's parent
     * is the root, which has no recorder here) */
    assert(fdk_widget_focus(a));
    k = ev_key(FDK_EVENT_KEY_DOWN, 30, 0);
    assert(!fdk_widget_tree_handle_event(root, &k));
    assert(ra.key_down == 1);

    /* Tab advances focus to b and is CONSUMED by the tree */
    k = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_TAB, 0);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(ra.focus_out == 1 && rb.focus_in == 1);
    assert(fdk_widget_tree_get_focused(root) == b);

    /* Shift+Tab goes backward, wrapping at the ends */
    k = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_TAB, FDK_MOD_SHIFT);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_widget_tree_get_focused(root) == a);

    /* plain Tab wraps around too (a -> b -> a) */
    k = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_TAB, 0);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_widget_tree_get_focused(root) == b);
    k = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_TAB, 0);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_widget_tree_get_focused(root) == a);

    /* a widget that handles Tab itself wins over the built-in
     * (ra.key_down: 1 'x' + 3 Tab key-downs delivered to a so far,
     * this handled Tab is the 4th — every Tab DOWN is DELIVERED to
     * the focused widget; only its unhandled-ness triggers
     * traversal) */
    ra.handle = true;
    k = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_TAB, 0);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(ra.key_down == 4);
    assert(fdk_widget_tree_get_focused(root) == a); /* unchanged */
    ra.handle = false;

    /* Ctrl+Tab is NOT built-in traversal (app keeps it) */
    k = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_TAB, FDK_MOD_CTRL);
    assert(!fdk_widget_tree_handle_event(root, &k));

    /* advance with nothing focusable: focus was already dropped by
     * set_can_focus (a's 3rd OUT — 2 from Tab advances + this one);
     * the Tab is still consumed by the traversal. */
    fdk_widget_set_can_focus(a, false);
    fdk_widget_set_can_focus(b, false);
    k = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_TAB, 0);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(ra.focus_out == 3);
    assert(fdk_widget_tree_get_focused(root) == NULL);

    fdk_widget_destroy(root);
    printf("[ok] keyboard: routing to focus, bubbling, Tab/Shift+Tab "
           "traversal (wrap, override, modifier guard)\n");
}

/* ---- reentrancy: destroy during dispatch ---- */

static fdk_widget *g_suicide_target = NULL;
static bool destroy_target_cb(fdk_widget *w, const fdk_widget_event *e,
                              void *ud) {
    (void)w;
    (void)e;
    (void)ud;
    if (g_suicide_target != NULL) {
        fdk_widget *t = g_suicide_target;
        g_suicide_target = NULL;
        fdk_widget_destroy(t);
        fdk_widget_destroy(t); /* mid-destruction: documented no-op */
        /* t is DESTROYING but still allocated (deferred free): the
         * documented refusals must hold in exactly this state. */
        fdk_widget *probe = NULL;
        assert(fdk_widget_create(t, NULL, (fdk_rect){0, 0, 1, 1},
                                 &probe) == FDK_ERR_INVALID_ARGUMENT);
    }
    return false;
}

static void test_destroy_during_dispatch(void) {
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *a = mk_widget(root, 0, 0, 100, 100);
    (void)mk_widget(a, 10, 10, 50, 50); /* subtree that dies with a */

    /* 1. widget destroys ITSELF from its own DOWN handler; dispatch
     *    unwinds cleanly and the deferred teardown frees `a` and its
     *    subtree AT handle_event's exit — by design. Afterwards only
     *    the root may be touched. */
    g_suicide_target = a;
    fdk_widget_set_event_callback(a, destroy_target_cb, NULL);
    fdk_event_data dn = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 50, 50, 1);
    (void)fdk_widget_tree_handle_event(root, &dn); /* frees a here */
    assert(fdk_widget_child_count(root) == 0);

    /* tree still functional after the self-destroy */
    fdk_widget *c = mk_widget(root, 0, 0, 10, 10);
    assert(fdk_widget_child_count(root) == 1);
    fdk_event_data m = ev_motion(5, 5);
    (void)fdk_widget_tree_handle_event(root, &m);
    (void)c;

    /* 2. widget destroys its own ANCESTOR from a child handler */
    fdk_widget *root2 = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *mid = mk_widget(root2, 0, 0, 100, 100);
    fdk_widget *leaf = mk_widget(mid, 10, 10, 50, 50);
    g_suicide_target = mid;
    fdk_widget_set_event_callback(leaf, destroy_target_cb, NULL);
    dn = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 20, 20, 1);
    (void)fdk_widget_tree_handle_event(root2, &dn);
    assert(fdk_widget_child_count(root2) == 0);

    /* 3. destroy the WHOLE ROOT from a handler; the dispatch unwinds
     *    and the deferred teardown frees it — no leak, no UAF (ASan
     *    would fire on either) */
    fdk_widget *root3 = mk_widget(NULL, 0, 0, 200, 100);
    fdk_widget *x = mk_widget(root3, 0, 0, 100, 100);
    g_suicide_target = root3;
    fdk_widget_set_event_callback(x, destroy_target_cb, NULL);
    dn = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 50, 50, 1);
    (void)fdk_widget_tree_handle_event(root3, &dn); /* root freed here */

    fdk_widget_destroy(root);
    fdk_widget_destroy(root2);
    printf("[ok] destroy-during-dispatch: self, ancestor, whole root "
           "(deferred, ASan-clean)\n");
}

/* ---- measure / arrange hooks ---- */

static void test_measure_arrange_hooks(void) {
    int destroyed = 0;
    fdk_widget *root = mk_widget(NULL, 0, 0, 200, 100);

    /* default measure: natural size = current bounds */
    fdk_size natural;
    fdk_widget_measure(root, &natural);
    assert(natural.width == 200 && natural.height == 100);

    /* subclass measure hook wins */
    counted_widget *cw = (counted_widget *)mk_counted(root, 0, 0, 80, 20,
                                                       &destroyed);
    cw->measure_w = 123;
    cw->measure_h = 45;
    fdk_widget_measure(&cw->base, &natural);
    assert(natural.width == 123 && natural.height == 45);

    /* default arrange: assigns via set_bounds */
    fdk_rect assigned = {5, 5, 60, 15};
    fdk_widget_arrange(&cw->base, assigned);
    fdk_rect got = fdk_widget_get_bounds(&cw->base);
    assert(got.x == 5 && got.y == 5 && got.width == 60 && got.height == 15);

    fdk_widget_destroy(root);
    assert(destroyed == 1);
    printf("[ok] measure/arrange hooks (defaults + subclass override)\n");
}

/* ---- base style ---- */

static void test_base_style_paint(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(100, 100, &s)));
    fdk_widget *root = mk_widget(NULL, 0, 0, 100, 100);
    fdk_widget_set_background(root, rgb(120, 60, 30));
    fdk_widget_set_corner_radius(root, 20);

    fdk_widget_tree_paint(root, s);

    /* rounded corners cut out; center filled */
    assert(px_is(s, 50, 50, 120, 60, 30));
    assert(px_is(s, 1, 1, 0, 0, 0));      /* corner untouched (zeroed) */
    assert(px_is(s, 20, 20, 120, 60, 30)); /* inside the arc           */

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] base style: background + corner radius paint\n");
}

/* ---- isolation between trees ---- */

static void test_tree_isolation(void) {
    fdk_widget *root1 = mk_widget(NULL, 0, 0, 100, 100);
    fdk_widget *root2 = mk_widget(NULL, 0, 0, 100, 100);
    fdk_widget *a1 = mk_widget(root1, 0, 0, 100, 100);

    recorder r1;
    memset(&r1, 0, sizeof(r1));
    fdk_widget_set_event_callback(a1, record_event, &r1);

    /* events injected into tree 2 never reach tree 1 */
    fdk_event_data m = ev_motion(50, 50);
    (void)fdk_widget_tree_handle_event(root2, &m);
    assert(r1.motion == 0);
    assert(!fdk_widget_is_hovered(a1));

    /* and vice versa */
    (void)fdk_widget_tree_handle_event(root1, &m);
    assert(r1.motion == 1);

    /* damage books are per-tree: a fresh root is damaged (never
     * painted — creation damages), so settle tree 2 first, then
     * invalidating tree 1 must not mark it. */
    fdk_surface *s2 = NULL;
    assert(fdk_ok(fdk_surface_create(100, 100, &s2)));
    fdk_widget_tree_paint(root2, s2);
    fdk_surface_destroy(s2);
    assert(!fdk_widget_tree_has_damage(root2));

    fdk_widget_invalidate(a1);
    assert(fdk_widget_tree_has_damage(root1));
    assert(!fdk_widget_tree_has_damage(root2));

    fdk_widget_destroy(root1);
    fdk_widget_destroy(root2);
    printf("[ok] standalone trees are isolated (events + damage)\n");
}

int main(void) {
    test_create_hierarchy_destroy();
    test_argument_checks();
    test_reparent_raise_lower();
    test_visibility_enabled();
    test_paint_zorder_and_clipping();
    test_paint_damage_partial();
    test_move_invalidates_both_regions();
    test_pointer_hover_motion();
    test_pointer_grab();
    test_event_bubbling();
    test_scroll();
    test_focus_lifecycle();
    test_key_routing_and_tab();
    test_destroy_during_dispatch();
    test_measure_arrange_hooks();
    test_base_style_paint();
    test_tree_isolation();
    printf("all headless widget tests passed\n");
    return 0;
}
