/* test_a11y.c — the accessibility core (Phase 10, first slice),
 * headless: describe/subscribe/perform over the real catalog.
 *
 * Everything here runs without a display because the a11y layer IS
 * the widget layer — the same discipline as the rest of the headless
 * suite. */

#include "fdk/fdk.h"
#include "fdk/fdk_a11y.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__,        \
                    __LINE__);                                          \
            g_fail++;                                                   \
        } else {                                                        \
            printf("[ok] %s\n", msg);                                   \
        }                                                               \
    } while (0)

/* ---- notification recorder ---- */

#define REC_MAX 64
typedef struct {
    fdk_a11y_event events[REC_MAX];
    size_t count;
} recorder;

static void record_event(const fdk_a11y_event *ev, void *user) {
    recorder *r = user;
    if (r->count < REC_MAX) {
        r->events[r->count++] = *ev;
    }
}

static bool rec_has(const recorder *r, fdk_a11y_event_kind kind,
                    const fdk_widget *w, fdk_a11y_state_flag flag) {
    for (size_t i = 0; i < r->count; i++) {
        if (r->events[i].kind == kind && r->events[i].widget == w &&
            r->events[i].state_flag == flag) {
            return true;
        }
    }
    return false;
}

/* ---- helpers ---- */

static fdk_font *g_font = NULL;

/* Depth-first search for the nth widget whose a11y role matches —
 * the exact enumeration a bridge performs (child_at + describe). */
static fdk_widget *find_by_role_impl(fdk_widget *w, fdk_a11y_role role,
                                     int want, int *seen) {
    fdk_a11y_info info;
    if (fdk_ok(fdk_a11y_describe(w, &info))) {
        if (info.role == role) {
            if (*seen == want) {
                fdk_a11y_info_free(&info);
                return w;
            }
            (*seen)++;
        }
        fdk_a11y_info_free(&info);
    }
    for (size_t i = 0; i < fdk_widget_child_count(w); i++) {
        fdk_widget *hit = find_by_role_impl(fdk_widget_child_at(w, i),
                                            role, want, seen);
        if (hit != NULL) {
            return hit;
        }
    }
    return NULL;
}

static fdk_widget *find_by_role(fdk_widget *root, fdk_a11y_role role,
                                int index) {
    int seen = 0;
    return find_by_role_impl(root, role, index, &seen);
}

static void load_font(void) {
    const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]);
         i++) {
        g_font = fdk_font_load(candidates[i], 14);
        if (g_font != NULL) {
            return;
        }
    }
    g_font = NULL; /* fontless: names fall back to NULL — still testable */
}

/* ---- describe over the catalog ---- */

static void test_describe_catalog(void) {
    fdk_widget *root = NULL;
    fdk_result r = fdk_widget_create(NULL, NULL,
                                     (fdk_rect){0, 0, 400, 300}, &root);
    CHECK(fdk_ok(r), "root created");

    fdk_a11y_info info;

    /* The plain root: unknown role (standalone roots have no WINDOW
     * role — only window-owned roots do). */
    r = fdk_a11y_describe(root, &info);
    CHECK(fdk_ok(r), "describe plain root");
    CHECK(info.role == FDK_A11Y_ROLE_UNKNOWN, "plain root role unknown");
    CHECK((info.states & FDK_A11Y_VISIBLE) != 0 &&
              (info.states & FDK_A11Y_SHOWING) != 0 &&
              (info.states & FDK_A11Y_ENABLED) != 0,
          "core states: visible+showing+enabled");
    CHECK(info.bounds.width == 400 && info.bounds.height == 300,
          "bounds are root-absolute");
    fdk_a11y_info_free(&info);

    /* Label: role + name from text. */
    fdk_widget *label = NULL;
    r = fdk_label_create(root, g_font, "Hello", &label);
    CHECK(fdk_ok(r), "label created");
    r = fdk_a11y_describe(label, &info);
    CHECK(fdk_ok(r) && info.role == FDK_A11Y_ROLE_LABEL, "label role");
    CHECK(info.name != NULL && strcmp(info.name, "Hello") == 0,
          "label name from text");
    fdk_a11y_info_free(&info);

    /* Name override beats the computed name. */
    fdk_widget_set_accessible_name(label, "Greeting");
    r = fdk_a11y_describe(label, &info);
    CHECK(info.name != NULL && strcmp(info.name, "Greeting") == 0,
          "override beats computed name");
    fdk_a11y_info_free(&info);
    fdk_widget_set_accessible_name(label, NULL);
    r = fdk_a11y_describe(label, &info);
    CHECK(info.name != NULL && strcmp(info.name, "Hello") == 0,
          "NULL override clears back to computed");
    fdk_a11y_info_free(&info);

    /* Description override. */
    fdk_widget_set_accessible_description(label, "The greeting line");
    r = fdk_a11y_describe(label, &info);
    CHECK(info.description != NULL &&
              strcmp(info.description, "The greeting line") == 0,
          "description override");
    fdk_a11y_info_free(&info);
    fdk_widget_set_accessible_description(label, NULL);

    /* Button: role + name + ACTIVATE only with a callback. */
    fdk_widget *btn = NULL;
    r = fdk_button_create(root, g_font, "Apply", &btn);
    CHECK(fdk_ok(r), "button created");
    CHECK(fdk_a11y_actions_of(btn) == 0,
          "button without callback: no actions");
    fdk_a11y_perform(btn, FDK_A11Y_ACTION_ACTIVATE, 0.0);
    r = fdk_a11y_describe(btn, &info);
    CHECK(info.role == FDK_A11Y_ROLE_BUTTON &&
              info.name != NULL && strcmp(info.name, "Apply") == 0,
          "button role + name");
    fdk_a11y_info_free(&info);

    /* Progress bar: value interface 0..1 + text. */
    fdk_widget *bar = NULL;
    r = fdk_progress_create(root, &bar);
    CHECK(fdk_ok(r), "progress created");
    fdk_progress_set_fraction(bar, 0.75f);
    r = fdk_a11y_describe(bar, &info);
    CHECK(info.role == FDK_A11Y_ROLE_PROGRESS_BAR && info.has_value &&
              info.value_current == 0.75 && info.value_min == 0.0 &&
              info.value_max == 1.0,
          "progress value interface");
    CHECK(info.value_text != NULL && strcmp(info.value_text, "75%") == 0,
          "progress value_text rendering");
    CHECK(fdk_a11y_actions_of(bar) == 0,
          "progress bar: indicator, no actions");
    fdk_a11y_info_free(&info);

    /* Separator + Frame. */
    fdk_widget *sep = NULL;
    fdk_separator_create(root, FDK_HORIZONTAL, &sep);
    fdk_widget *frame = NULL;
    fdk_frame_create(root, g_font, "Profile", &frame);
    r = fdk_a11y_describe(sep, &info);
    CHECK(info.role == FDK_A11Y_ROLE_SEPARATOR, "separator role");
    fdk_a11y_info_free(&info);
    r = fdk_a11y_describe(frame, &info);
    CHECK(info.role == FDK_A11Y_ROLE_GROUP &&
              info.name != NULL && strcmp(info.name, "Profile") == 0,
          "frame role GROUP + title name");
    fdk_a11y_info_free(&info);

    /* NULL / dying safety. */
    CHECK(fdk_a11y_describe(NULL, &info) == FDK_ERR_INVALID_ARGUMENT,
          "describe NULL refused");
    fdk_a11y_info_free(NULL); /* NULL-safe */
    CHECK(1, "info_free NULL-safe");

    fdk_widget_destroy(root);
}

/* ---- SHOWING vs VISIBLE ---- */

static void test_showing(void) {
    fdk_widget *root = NULL;
    fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 100, 100}, &root);
    fdk_widget *mid = NULL;
    fdk_widget_create(root, NULL, (fdk_rect){0, 0, 100, 50}, &mid);
    fdk_widget *leaf = NULL;
    fdk_label_create(mid, g_font, "Deep", &leaf);

    fdk_a11y_info info;
    fdk_a11y_describe(leaf, &info);
    CHECK((info.states & FDK_A11Y_SHOWING) != 0, "leaf showing");
    fdk_a11y_info_free(&info);

    fdk_widget_set_visible(mid, false);
    fdk_a11y_describe(leaf, &info);
    CHECK((info.states & FDK_A11Y_VISIBLE) != 0 &&
              (info.states & FDK_A11Y_SHOWING) == 0,
          "hidden ancestor: leaf VISIBLE but not SHOWING");
    fdk_a11y_info_free(&info);
    fdk_a11y_describe(mid, &info);
    CHECK((info.states & FDK_A11Y_VISIBLE) == 0,
          "mid itself not VISIBLE");
    fdk_a11y_info_free(&info);

    fdk_widget_destroy(root);
}

/* ---- notifications ---- */

static void test_notifications(void) {
    fdk_widget *root = NULL;
    fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 100, 100}, &root);
    fdk_widget *btn = NULL;
    fdk_button_create(root, g_font, "Go", &btn);

    recorder rec = {0};
    fdk_result r = fdk_a11y_subscribe(NULL, record_event, &rec);
    CHECK(fdk_ok(r), "global subscribe");

    /* children changed on create/destroy. */
    fdk_widget *extra = NULL;
    fdk_label_create(root, g_font, "x", &extra);
    CHECK(rec_has(&rec, FDK_A11Y_CHILDREN_CHANGED, root, 0),
          "children-changed on create");

    /* bounds. */
    fdk_widget_set_bounds(btn, (fdk_rect){5, 5, 40, 20});
    CHECK(rec_has(&rec, FDK_A11Y_BOUNDS_CHANGED, btn, 0),
          "bounds-changed on set_bounds");

    /* visible / enabled. */
    fdk_widget_set_visible(btn, false);
    CHECK(rec_has(&rec, FDK_A11Y_STATE_CHANGED, btn, FDK_A11Y_VISIBLE),
          "state-changed VISIBLE");
    fdk_widget_set_visible(btn, true);
    fdk_widget_set_enabled(btn, false);
    CHECK(rec_has(&rec, FDK_A11Y_STATE_CHANGED, btn, FDK_A11Y_ENABLED),
          "state-changed ENABLED");
    fdk_widget_set_enabled(btn, true);

    /* focus: both ends notify. */
    fdk_widget_set_can_focus(btn, true);
    fdk_widget *other = NULL;
    fdk_label_create(root, g_font, "y", &other);
    (void)other;
    fdk_widget *second = NULL;
    fdk_button_create(root, g_font, "Two", &second);
    fdk_widget_set_can_focus(second, true);
    rec.count = 0;
    fdk_widget_focus(btn);
    CHECK(rec_has(&rec, FDK_A11Y_STATE_CHANGED, btn, FDK_A11Y_FOCUSED),
          "focus-in notified");
    fdk_widget_focus(second);
    CHECK(rec_has(&rec, FDK_A11Y_STATE_CHANGED, btn, FDK_A11Y_FOCUSED) &&
              rec_has(&rec, FDK_A11Y_STATE_CHANGED, second,
                      FDK_A11Y_FOCUSED),
          "focus move notifies both ends");

    /* name: label text change. */
    rec.count = 0;
    fdk_widget *lbl = NULL;
    fdk_label_create(root, g_font, "before", &lbl);
    (void)fdk_label_set_text(lbl, "after");
    CHECK(rec_has(&rec, FDK_A11Y_NAME_CHANGED, lbl, 0),
          "name-changed on label text");

    /* value: progress. */
    rec.count = 0;
    fdk_widget *bar = NULL;
    fdk_progress_create(root, &bar);
    fdk_progress_set_fraction(bar, 0.5f);
    CHECK(rec_has(&rec, FDK_A11Y_VALUE_CHANGED, bar, 0),
          "value-changed on progress");

    /* checked: checkbox. */
    rec.count = 0;
    fdk_widget *cb = NULL;
    fdk_checkbox_create(root, g_font, "Opt", &cb);
    fdk_checkbox_set_checked(cb, true);
    fdk_a11y_info info;
    fdk_a11y_describe(cb, &info);
    CHECK((info.states & FDK_A11Y_CHECKED) != 0, "checkbox CHECKED state");
    fdk_a11y_info_free(&info);
    CHECK(rec_has(&rec, FDK_A11Y_STATE_CHANGED, cb, FDK_A11Y_CHECKED),
          "state-changed CHECKED on programmatic set");

    /* radio group: sibling unchecks notify too. */
    rec.count = 0;
    fdk_widget *group = NULL;
    fdk_widget_create(root, NULL, (fdk_rect){0, 0, 100, 60}, &group);
    fdk_widget *r1 = NULL, *r2 = NULL;
    fdk_radio_create(group, g_font, "A", &r1);
    fdk_radio_create(group, g_font, "B", &r2);
    fdk_radio_set_checked(r1, true);
    fdk_radio_set_checked(r2, true);
    CHECK(rec_has(&rec, FDK_A11Y_STATE_CHANGED, r1, FDK_A11Y_CHECKED) &&
              rec_has(&rec, FDK_A11Y_STATE_CHANGED, r2, FDK_A11Y_CHECKED),
          "radio swap notifies both radios");

    /* destroy: children-changed (subject already detached — global
     * subscribers only). */
    rec.count = 0;
    fdk_widget_destroy(extra);
    CHECK(rec_has(&rec, FDK_A11Y_CHILDREN_CHANGED, root, 0),
          "children-changed on destroy");

    /* scope filtering: a subtree subscriber does not see outside. */
    recorder scoped = {0};
    fdk_widget *box = NULL;
    fdk_widget_create(root, NULL, (fdk_rect){0, 0, 50, 50}, &box);
    CHECK(fdk_ok(fdk_a11y_subscribe(box, record_event, &scoped)),
          "scoped subscribe");
    scoped.count = 0;
    fdk_widget_set_bounds(btn, (fdk_rect){6, 6, 40, 20}); /* outside */
    CHECK(scoped.count == 0, "scoped subscriber ignores outside");
    fdk_widget *inner = NULL;
    fdk_label_create(box, g_font, "in", &inner);
    CHECK(rec_has(&scoped, FDK_A11Y_CHILDREN_CHANGED, box, 0),
          "scoped subscriber sees inside");

    /* duplicate subscribe is a no-op; unsubscribe works; limit fires. */
    CHECK(fdk_ok(fdk_a11y_subscribe(NULL, record_event, &rec)) &&
              fdk_a11y_subscribe(NULL, record_event, &rec) == FDK_OK,
          "duplicate subscribe no-op");
    CHECK(fdk_a11y_unsubscribe(box, record_event, &scoped) == FDK_OK,
          "unsubscribe");
    CHECK(fdk_a11y_unsubscribe(box, record_event, &scoped) ==
              FDK_ERR_NOT_FOUND,
          "double unsubscribe NOT_FOUND");

    recorder many[FDK_A11Y_MAX_SUBSCRIBERS + 1];
    size_t ok_count = 0;
    (void)fdk_a11y_unsubscribe(NULL, record_event, &rec); /* free slot 0 */
    for (size_t i = 0; i <= FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        if (fdk_ok(fdk_a11y_subscribe(NULL, record_event, &many[i]))) {
            ok_count++;
        }
    }
    CHECK(ok_count == FDK_A11Y_MAX_SUBSCRIBERS,
          "subscriber limit enforced");
    for (size_t i = 0; i <= FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        (void)fdk_a11y_unsubscribe(NULL, record_event, &many[i]);
    }

    fdk_widget_destroy(root);
}

/* ---- actions ---- */

static int g_activations = 0;
static void count_activate(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_activations++;
}

static double g_last_slider = -1.0;
static void slider_changed(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_last_slider = fdk_slider_get_value(w);
}

static void test_actions(void) {
    fdk_widget *root = NULL;
    fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 300, 200}, &root);

    /* Button ACTIVATE: the callback fires, exactly the Space path. */
    g_activations = 0;
    fdk_widget *btn = NULL;
    fdk_button_create(root, g_font, "Fire", &btn);
    fdk_button_set_on_activate(btn, count_activate, NULL);
    CHECK((fdk_a11y_actions_of(btn) & FDK_A11Y_ACTION_ACTIVATE) != 0,
          "button advertises ACTIVATE");
    fdk_result r = fdk_a11y_perform(btn, FDK_A11Y_ACTION_ACTIVATE, 0.0);
    CHECK(fdk_ok(r) && g_activations == 1,
          "button ACTIVATE fires the callback");

    /* Checkbox ACTIVATE toggles + notifies. */
    fdk_widget *cb = NULL;
    fdk_checkbox_create(root, g_font, "C", &cb);
    r = fdk_a11y_perform(cb, FDK_A11Y_ACTION_ACTIVATE, 0.0);
    CHECK(fdk_ok(r) && fdk_checkbox_is_checked(cb),
          "checkbox ACTIVATE checks it");

    /* FOCUS action: universal over focusable widgets. */
    CHECK(fdk_a11y_perform(btn, FDK_A11Y_ACTION_FOCUS, 0.0) == FDK_OK &&
              fdk_widget_has_focus(btn),
          "FOCUS action focuses");
    CHECK(fdk_a11y_perform(root, FDK_A11Y_ACTION_FOCUS, 0.0) ==
              FDK_ERR_UNSUPPORTED,
          "FOCUS on non-focusable refused");

    /* Slider: value interface + SET_VALUE/INCREMENT/DECREMENT. */
    g_last_slider = -1.0;
    fdk_widget *slider = NULL;
    fdk_slider_create(root, 0.0, 100.0, 40.0, &slider);
    fdk_slider_set_step(slider, 5.0);
    fdk_slider_set_on_changed(slider, slider_changed, NULL);
    fdk_a11y_info info;
    fdk_a11y_describe(slider, &info);
    CHECK(info.role == FDK_A11Y_ROLE_SLIDER && info.has_value &&
              info.value_current == 40.0 && info.value_min == 0.0 &&
              info.value_max == 100.0,
          "slider value interface");
    fdk_a11y_info_free(&info);
    r = fdk_a11y_perform(slider, FDK_A11Y_ACTION_SET_VALUE, 55.0);
    CHECK(fdk_ok(r) && fdk_slider_get_value(slider) == 55.0 &&
              g_last_slider == 55.0,
          "slider SET_VALUE moves + fires");
    r = fdk_a11y_perform(slider, FDK_A11Y_ACTION_INCREMENT, 0.0);
    CHECK(fdk_ok(r) && fdk_slider_get_value(slider) == 60.0,
          "slider INCREMENT steps by the quantized step");
    r = fdk_a11y_perform(slider, FDK_A11Y_ACTION_DECREMENT, 0.0);
    CHECK(fdk_ok(r) && fdk_slider_get_value(slider) == 55.0,
          "slider DECREMENT steps back");

    /* SpinButton SET_VALUE. */
    fdk_widget *spin = NULL;
    fdk_spin_create(root, g_font, 0.0, 10.0, 5.0, &spin);
    fdk_spin_set_step(spin, 1.0);
    r = fdk_a11y_perform(spin, FDK_A11Y_ACTION_SET_VALUE, 7.0);
    CHECK(fdk_ok(r) && fdk_spin_get_value(spin) == 7.0,
          "spin SET_VALUE");
    r = fdk_a11y_perform(spin, FDK_A11Y_ACTION_INCREMENT, 0.0);
    CHECK(fdk_ok(r) && fdk_spin_get_value(spin) == 8.0,
          "spin INCREMENT");

    /* List: row ACTIVATE = plain-click selection; SELECTED state. */
    fdk_widget *list = NULL;
    fdk_list_create(root, g_font, &list);
    fdk_list_append(list, "one", NULL);
    fdk_list_append(list, "two", NULL);
    fdk_list_append(list, "three", NULL);
    fdk_a11y_describe(list, &info);
    CHECK(info.role == FDK_A11Y_ROLE_LIST &&
              (info.states & FDK_A11Y_MULTI_SELECTABLE) == 0,
          "list role, single-select: no MULTI_SELECTABLE");
    fdk_a11y_info_free(&info);
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    fdk_a11y_describe(list, &info);
    CHECK((info.states & FDK_A11Y_MULTI_SELECTABLE) != 0,
          "multi list advertises MULTI_SELECTABLE");
    fdk_a11y_info_free(&info);
    /* Find a row widget by its a11y ROLE — the public walker
     * (fdk_widget_child_at) + describe is exactly how a bridge
     * enumerates the tree. */
    fdk_widget *row2 = find_by_role(list, FDK_A11Y_ROLE_LIST_ITEM, 1);
    CHECK(row2 != NULL, "list rows reachable through the tree");
    if (row2 != NULL) {
        fdk_a11y_describe(row2, &info);
        CHECK(info.role == FDK_A11Y_ROLE_LIST_ITEM &&
                  info.name != NULL && strcmp(info.name, "two") == 0,
              "row role + name");
        fdk_a11y_info_free(&info);
        CHECK((fdk_a11y_actions_of(row2) & FDK_A11Y_ACTION_ACTIVATE) != 0,
              "row advertises ACTIVATE");
        r = fdk_a11y_perform(row2, FDK_A11Y_ACTION_ACTIVATE, 0.0);
        CHECK(fdk_ok(r) && fdk_list_is_selected(list, 1),
              "row ACTIVATE selects it");
        fdk_a11y_describe(row2, &info);
        CHECK((info.states & FDK_A11Y_SELECTED) != 0,
              "row SELECTED state");
        fdk_a11y_info_free(&info);
    }

    /* Tree: expand/collapse/select on rows. */
    fdk_widget *tree = NULL;
    fdk_tree_create(root, g_font, &tree);
    fdk_tree_node top = FDK_TREE_NODE_NONE;
    fdk_tree_node_add(tree, FDK_TREE_NODE_NONE, "Parent", &top);
    fdk_tree_node kid = FDK_TREE_NODE_NONE;
    fdk_tree_node_add(tree, top, "Child", &kid);
    fdk_tree_node leaf2 = FDK_TREE_NODE_NONE;
    fdk_tree_node_add(tree, FDK_TREE_NODE_NONE, "Sibling", &leaf2);
    fdk_tree_node_expand(tree, top, true);
    fdk_widget *trow = find_by_role(tree, FDK_A11Y_ROLE_TREE_ITEM, 0);
    CHECK(trow != NULL, "tree rows reachable");
    if (trow != NULL) {
        fdk_a11y_describe(trow, &info);
        CHECK(info.role == FDK_A11Y_ROLE_TREE_ITEM &&
                  info.name != NULL && strcmp(info.name, "Parent") == 0 &&
                  (info.states & FDK_A11Y_EXPANDED) != 0,
              "tree row: role + name + EXPANDED");
        fdk_a11y_info_free(&info);
        CHECK((fdk_a11y_actions_of(trow) &
               (FDK_A11Y_ACTION_EXPAND | FDK_A11Y_ACTION_COLLAPSE |
                FDK_A11Y_ACTION_ACTIVATE)) ==
                  (FDK_A11Y_ACTION_EXPAND | FDK_A11Y_ACTION_COLLAPSE |
                   FDK_A11Y_ACTION_ACTIVATE),
              "parent row advertises expand/collapse/activate");
        r = fdk_a11y_perform(trow, FDK_A11Y_ACTION_COLLAPSE, 0.0);
        CHECK(fdk_ok(r) && !fdk_tree_node_is_expanded(tree, top),
              "row COLLAPSE collapses");
        fdk_a11y_describe(trow, &info);
        CHECK((info.states & FDK_A11Y_EXPANDED) == 0,
              "collapsed row drops EXPANDED");
        fdk_a11y_info_free(&info);
        r = fdk_a11y_perform(trow, FDK_A11Y_ACTION_ACTIVATE, 0.0);
        CHECK(fdk_ok(r) && fdk_tree_get_selected(tree) == top,
              "row ACTIVATE selects the node");
        r = fdk_a11y_perform(trow, FDK_A11Y_ACTION_EXPAND, 0.0);
        CHECK(fdk_ok(r) && fdk_tree_node_is_expanded(tree, top),
              "row EXPAND expands");
    }

    /* Notebook SET_VALUE switches pages. */
    fdk_widget *nb = NULL;
    fdk_notebook_create(root, g_font, &nb);
    fdk_widget *p1 = NULL, *p2 = NULL;
    fdk_widget_create(root, NULL, (fdk_rect){0, 0, 10, 10}, &p1);
    fdk_widget_create(root, NULL, (fdk_rect){0, 0, 10, 10}, &p2);
    CHECK(fdk_ok(fdk_notebook_append_page(nb, p1, "First")) &&
              fdk_ok(fdk_notebook_append_page(nb, p2, "Second")),
          "notebook pages appended (adopted)");
    fdk_a11y_describe(nb, &info);
    CHECK(info.role == FDK_A11Y_ROLE_TAB_LIST && info.has_value &&
              info.value_max == 1.0,
          "notebook role + value interface");
    fdk_a11y_info_free(&info);
    r = fdk_a11y_perform(nb, FDK_A11Y_ACTION_SET_VALUE, 1.0);
    CHECK(fdk_ok(r) && fdk_notebook_get_current_page(nb) == 1,
          "notebook SET_VALUE switches page");

    /* Combo SET_VALUE + HAS_POPUP state. */
    fdk_widget *combo = NULL;
    fdk_combo_create(root, g_font, &combo);
    fdk_combo_append(combo, "Red", NULL);
    fdk_combo_append(combo, "Green", NULL);
    fdk_combo_append(combo, "Blue", NULL);
    fdk_a11y_describe(combo, &info);
    CHECK(info.role == FDK_A11Y_ROLE_COMBO_BOX &&
              (info.states & FDK_A11Y_HAS_POPUP) != 0 &&
              info.has_value && info.value_current == -1.0,
          "combo role + HAS_POPUP + none-active value");
    fdk_a11y_info_free(&info);
    r = fdk_a11y_perform(combo, FDK_A11Y_ACTION_SET_VALUE, 2.0);
    CHECK(fdk_ok(r) && fdk_combo_get_active(combo) == 2,
          "combo SET_VALUE");
    fdk_a11y_describe(combo, &info);
    CHECK(info.name != NULL && strcmp(info.name, "Blue") == 0,
          "combo name follows the active row");
    fdk_a11y_info_free(&info);

    /* ScrollView SET_VALUE scrolls. */
    fdk_widget *sv = NULL;
    fdk_scrollview_create(root, &sv);
    fdk_widget *content = NULL;
    fdk_widget_create(root, NULL, (fdk_rect){0, 0, 100, 800}, &content);
    fdk_widget_set_natural_size(content, 100, 800);
    CHECK(fdk_ok(fdk_scrollview_set_content(sv, content)),
          "scrollview content set (adopted)");
    fdk_widget_set_bounds(sv, (fdk_rect){0, 0, 100, 100});
    fdk_a11y_describe(sv, &info);
    CHECK(info.role == FDK_A11Y_ROLE_SCROLL_AREA && info.has_value &&
              info.value_max > 0.0,
          "scrollview role + scroll value interface");
    double max = info.value_max;
    fdk_a11y_info_free(&info);
    r = fdk_a11y_perform(sv, FDK_A11Y_ACTION_SET_VALUE, max);
    fdk_i32 sx = 0, sy = 0;
    fdk_scrollview_get_scroll_offset(sv, &sx, &sy);
    CHECK(fdk_ok(r) && sy == (fdk_i32)max,
          "scrollview SET_VALUE scrolls to max");

    /* Unsupported / invalid actions. */
    CHECK(fdk_a11y_perform(NULL, FDK_A11Y_ACTION_ACTIVATE, 0.0) ==
              FDK_ERR_INVALID_ARGUMENT,
          "perform on NULL refused");
    CHECK(fdk_a11y_perform(root, 0, 0.0) == FDK_ERR_INVALID_ARGUMENT,
          "perform with action 0 refused");
    CHECK(fdk_a11y_perform(root, FDK_A11Y_ACTION_ACTIVATE, 0.0) ==
              FDK_ERR_UNSUPPORTED,
          "plain widget: ACTIVATE unsupported");

    fdk_widget_destroy(root);
}

/* ---- entry: the new modes + a11y states ---- */

static void test_entry_modes(void) {
    fdk_widget *root = NULL;
    fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 200, 100}, &root);

    fdk_widget *entry = NULL;
    fdk_result r = fdk_entry_create(root, g_font, "secret", &entry);
    CHECK(fdk_ok(r), "entry created");

    fdk_a11y_info info;
    fdk_a11y_describe(entry, &info);
    CHECK(info.role == FDK_A11Y_ROLE_ENTRY &&
              (info.states & FDK_A11Y_EDITABLE) != 0 &&
              info.value_text != NULL &&
              strcmp(info.value_text, "secret") == 0,
          "entry role + EDITABLE + text as value");
    fdk_a11y_info_free(&info);

    /* password mode: text still exposed (bridges mask), EDITABLE
     * unchanged. */
    fdk_entry_set_password(entry, true);
    CHECK(fdk_entry_is_password(entry), "password mode set");
    fdk_a11y_describe(entry, &info);
    CHECK((info.states & FDK_A11Y_EDITABLE) != 0 &&
              info.value_text != NULL &&
              strcmp(info.value_text, "secret") == 0,
          "password entry: text still readable (bridge masks)");
    fdk_a11y_info_free(&info);
    fdk_entry_set_password(entry, false);

    /* read-only: EDITABLE flips to READ_ONLY. */
    fdk_entry_set_read_only(entry, true);
    fdk_a11y_describe(entry, &info);
    CHECK((info.states & FDK_A11Y_READ_ONLY) != 0 &&
              (info.states & FDK_A11Y_EDITABLE) == 0,
          "read-only entry: READ_ONLY state");
    fdk_a11y_info_free(&info);

    /* read-only refuses typed edits but accepts programmatic text. */
    r = fdk_entry_set_text(entry, "still works");
    CHECK(fdk_ok(r) && strcmp(fdk_entry_get_text(entry), "still works") == 0,
          "read-only: programmatic set_text works");
    fdk_entry_set_read_only(entry, false);

    /* max length: growth refused, shrink fine, existing kept. */
    fdk_entry_set_max_length(entry, 5);
    CHECK(fdk_entry_get_max_length(entry) == 5, "max length get");
    r = fdk_entry_set_text(entry, "0123456789");
    CHECK(r == FDK_ERR_INVALID_ARGUMENT,
          "set_text beyond max refused");
    r = fdk_entry_set_text(entry, "01234");
    CHECK(fdk_ok(r), "set_text within max ok");
    fdk_entry_set_max_length(entry, 2);
    CHECK(strcmp(fdk_entry_get_text(entry), "01234") == 0,
          "shrinking the cap does not truncate");

    /* VALUE_CHANGED fires on text mutation. */
    fdk_entry_set_max_length(entry, 0); /* clear the 2-byte cap */
    recorder rec = {0};
    fdk_a11y_subscribe(NULL, record_event, &rec);
    rec.count = 0;
    (void)fdk_entry_set_text(entry, "abc");
    CHECK(rec_has(&rec, FDK_A11Y_VALUE_CHANGED, entry, 0),
          "entry text change fires VALUE_CHANGED");

    fdk_widget_destroy(root);
}

/* ---- role names ---- */

static void test_role_names(void) {
    CHECK(strcmp(fdk_a11y_role_name(FDK_A11Y_ROLE_BUTTON), "button") == 0,
          "role name: button");
    CHECK(strcmp(fdk_a11y_role_name(FDK_A11Y_ROLE_CHECK_MENU_ITEM),
                 "check menu item") == 0,
          "role name: check menu item");
    CHECK(strcmp(fdk_a11y_role_name((fdk_a11y_role)9999), "unknown") == 0,
          "role name: out of range -> unknown");
    CHECK(fdk_a11y_role_name(FDK_A11Y_ROLE_UNKNOWN) != NULL,
          "role name never NULL");
}

int main(void) {
    load_font();
    test_role_names();
    test_describe_catalog();
    test_showing();
    test_notifications();
    test_actions();
    test_entry_modes();
    if (g_font != NULL) {
        fdk_font_destroy(g_font);
    }
    if (g_fail != 0) {
        fprintf(stderr, "%d FAILURES\n", g_fail);
        return 1;
    }
    printf("all accessibility tests passed\n");
    return 0;
}
