/*
 * test_widget_drag_source.c — the §29/G-10 widget-level drag source.
 *
 * The §28 drag rig found it: fdk-files arms its pane drags from a
 * POINTER_DOWN in view_event (the List's event callback), but a press
 * on a ROW is consumed by the row's own handler (selection) — bubbling
 * STOPS there, the ancestor callback never sees the press, the drag
 * can never arm. Every list-shaped drag source has this defect; the
 * app-side workaround (re-arming from the row activation) is not a
 * source at all.
 *
 * What a serious app needs is the GTK4 GtkDragSource shape, as a
 * WIDGET-attached controller:
 *
 *   fdk_widget_set_drag_source(widget, threshold, on_begin, user)
 *
 *   - the press may land on ANY descendant (a row) — the controller
 *     sees it because it rides the dispatch itself, not a callback
 *     that the row's consumption starves;
 *   - motion while held, past `threshold' px, fires on_begin AT THE
 *     WIDGET (the app calls fdk_drag_begin_ex there with its real
 *     payload);
 *   - release before the threshold never fires (a click stays a
 *     click); after a begin the controller stands down until the next
 *     press;
 *   - the same widget can be a drop target too (a pane both drags
 *     from and drops onto); clearing (NULL fn) disarms.
 *
 * This file pins the controller's logic headlessly through the tree's
 * real dispatch (fdk_widget_tree_handle_event): a List with rows that
 * CONSUME their presses, plus the drag-source controller on the list.
 * The live backend legs (real XTEST drags) are the drag rig's.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_dnd.h"
#include "fdk/fdk_model.h"
#include "fdk/fdk_widgets.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static fdk_event_data ev_button(fdk_event_type t, float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = FDK_POINTER_BUTTON_LEFT;
    e.pointer_button.modifiers = 0;
    return e;
}

static fdk_event_data ev_motion(float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_POINTER_MOTION;
    e.pointer.position.x = x;
    e.pointer.position.y = y;
    return e;
}

/* ---- the app-side begin callback (what fdk-files would do) ------ */

static int g_begins;
static fdk_widget *g_begin_widget;
static void *g_begin_user;

static void *on_begin(fdk_widget *source_widget, void *user) {
    g_begins++;
    g_begin_widget = source_widget;
    g_begin_user = user;
    /* the real app calls fdk_drag_begin_ex here; headless has no
     * backend, so the controller's contract is what we assert */
    return NULL;
}

/* ---- G-10: the controller sees a press a descendant consumes ----- */

static void test_drag_source_behind_consuming_child(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, NULL, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){0, 0, 400, 300});

    /* rows that CONSUME their presses (the real List does: selection)
     */
    for (int i = 0; i < 3; i++) {
        (void)fdk_list_append(list, "row", NULL);
    }
    (void)fdk_list_select(list, 0); /* the arming precondition */

    g_begins = 0;
    g_begin_widget = NULL;
    assert(fdk_ok(fdk_widget_set_drag_source(list, 6, on_begin,
                                             (void *)0xA1)));
    assert(fdk_widget_get_drag_source(list));

    /* press ON THE ROW (y=10: row 0's band), move past the threshold:
     * the controller must fire even though the row consumed the
     * press. */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    50, 10);
    (void)fdk_widget_tree_handle_event(root, &down);
    /* a sub-threshold wiggle: no begin yet */
    fdk_event_data wiggle = ev_motion(53, 12);
    (void)fdk_widget_tree_handle_event(root, &wiggle);
    assert(g_begins == 0);
    /* past the threshold: exactly one begin */
    fdk_event_data moved = ev_motion(70, 14);
    (void)fdk_widget_tree_handle_event(root, &moved);
    assert(g_begins == 1);
    assert(g_begin_widget == list);
    assert(g_begin_user == (void *)0xA1);
    /* further motion while still held: no second begin */
    fdk_event_data more = ev_motion(90, 20);
    (void)fdk_widget_tree_handle_event(root, &more);
    assert(g_begins == 1);
    /* release stands the controller down */
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 90, 20);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(g_begins == 1);

    /* a fresh press+release under the threshold = a click, no begin */
    fdk_event_data down2 = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                     50, 10);
    (void)fdk_widget_tree_handle_event(root, &down2);
    fdk_event_data up2 = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 52, 11);
    (void)fdk_widget_tree_handle_event(root, &up2);
    assert(g_begins == 1);

    fdk_widget_destroy(root);
    printf("[ok] drag source: a press consumed by a child row still "
           "arms the widget's drag controller (threshold, one begin, "
           "click stays a click)\n");
}

/* ---- the controller is per-widget and clearable ------------------- */

static void test_drag_source_clear_and_isolation(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *a = NULL;
    fdk_widget *b = NULL;
    assert(fdk_ok(fdk_list_create(root, NULL, &a)));
    assert(fdk_ok(fdk_list_create(root, NULL, &b)));
    fdk_widget_set_bounds(a, (fdk_rect){0, 0, 200, 300});
    fdk_widget_set_bounds(b, (fdk_rect){200, 0, 200, 300});

    g_begins = 0;
    assert(fdk_ok(fdk_widget_set_drag_source(a, 6, on_begin, NULL)));
    /* b has no source: a press+drag there never fires a */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    250, 50);
    (void)fdk_widget_tree_handle_event(root, &down);
    fdk_event_data moved = ev_motion(260, 52);
    (void)fdk_widget_tree_handle_event(root, &moved);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 260, 52);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(g_begins == 0);

    /* a's own area fires exactly once */
    down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 50, 50);
    (void)fdk_widget_tree_handle_event(root, &down);
    moved = ev_motion(80, 50);
    (void)fdk_widget_tree_handle_event(root, &moved);
    assert(g_begins == 1);

    /* clearing disarms */
    assert(fdk_ok(fdk_widget_set_drag_source(a, 6, NULL, NULL)));
    assert(!fdk_widget_get_drag_source(a));
    down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 50, 50);
    (void)fdk_widget_tree_handle_event(root, &down);
    moved = ev_motion(80, 50);
    (void)fdk_widget_tree_handle_event(root, &moved);
    up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 80, 50);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(g_begins == 1);

    /* bad args refused */
    assert(!fdk_ok(fdk_widget_set_drag_source(NULL, 6, on_begin,
                                              NULL)));
    assert(!fdk_ok(fdk_widget_set_drag_source(b, 0, on_begin, NULL)));

    fdk_widget_destroy(root);
    printf("[ok] drag source: per-widget scoping, clearable, "
           "argument-checked\n");
}

/* ---- motion with no press never begins ----------------------------- */

static void test_drag_source_requires_press(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, NULL, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){0, 0, 400, 300});

    g_begins = 0;
    assert(fdk_ok(fdk_widget_set_drag_source(list, 6, on_begin,
                                             NULL)));
    /* bare motion, no button held */
    for (int x = 10; x < 200; x += 20) {
        fdk_event_data m = ev_motion((float)x, 40);
        (void)fdk_widget_tree_handle_event(root, &m);
    }
    assert(g_begins == 0);

    fdk_widget_destroy(root);
    printf("[ok] drag source: hover motion without a held button "
           "never begins a drag\n");
}

int main(void) {
    test_drag_source_behind_consuming_child();
    test_drag_source_clear_and_isolation();
    test_drag_source_requires_press();
    printf("all drag-source tests passed\n");
    return 0;
}
