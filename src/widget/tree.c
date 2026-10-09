#define FDK_LOG_TAG "widgets"

/*
 * tree.c — Tree widget (Phase 9)
 *
 * A hierarchical, expandable list built exactly like List: an
 * internal ScrollView with one row widget per VISIBLE node. The
 * model is a flat array of node records (text, parent handle,
 * first-child/next-sibling links kept implicitly through ordering +
 * parent indices), so node HANDLES stay stable across growth, and
 * the visible sequence is derived by a pre-order walk that skips
 * collapsed subtrees.
 *
 * Row anatomy: [indent (depth * 16px)] [expander glyph for parents]
 * [text]. Parents draw a vector triangle pointing right when
 * collapsed, down when expanded — no font needed for the expander
 * (the same discipline as the title-bar band buttons).
 *
 * Selection: SINGLE only (the classic tree; multi-select trees are
 * their own design problem — parked honestly in the roadmap entry).
 * Keyboard: Up/Down walk VISIBLE nodes; Left collapses a parent or
 * jumps to its parent; Right expands a parent or enters its first
 * child; Home/End/PageUp/PageDown behave like List.
 *
 * 1.4.3 — the parked multi-select tree, shipped: the List's exact
 * model (fdk_tree_selection_mode), ctrl/shift/ctrl+shift click
 * semantics over the VISIBLE sequence, and the rubber band with
 * ctrl-union sweeps over empty tree space, auto-scrolling when the
 * sweep chases the viewport edge — everything the List does, in the
 * tree's visible-row space.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h" /* timer lifecycle via context */

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

#define TREE_ROW_PAD_Y 5
#define TREE_INDENT 16
#define TREE_EXPANDER 14

typedef struct fdk_tree_node_rec {
    char *text;        /* owned                          */
    size_t parent;     /* FDK_TREE_NODE_NONE for roots   */
    size_t first_child;
    size_t next_sibling;
    bool expanded;
    bool selected;
    bool is_parent;    /* has at least one child         */
    bool dead;         /* removed: tombstone keeps every
                       * OTHER handle stable (the store
                       * never reuses an index)          */
    fdk_row_icon icon; /* 1.4.2: the glyph before the text
                       * (rides the MODEL, so it survives
                       * collapses and re-expansions)    */
} fdk_tree_node_rec;

typedef struct fdk_tree_row {
    fdk_widget base;
    size_t node;       /* model index of the shown node  */
} fdk_tree_row;

typedef struct fdk_tree {
    fdk_widget base;
    fdk_font *font;    /* borrowed */
    fdk_tree_node_rec *nodes;
    size_t count;
    size_t capacity;
    fdk_widget *scroll;
    fdk_widget *rows;      /* visible-rows container        */
    fdk_tree_row **row_widgets; /* one per VISIBLE node      */
    size_t row_count;
    size_t row_capacity;
    size_t anchor;         /* keyboard/selection position (visible idx) */
    fdk_tree_selection_fn on_selection_changed;
    void *on_selection_data;
    /* ---- 1.4.3: multi-selection + the rubber band ---- */
    fdk_tree_selection_mode mode;   /* default SINGLE                */
    bool banding;
    bool band_ctrl;      /* the union modifier at press           */
    fdk_f32 band_x, band_y;         /* anchor, tree(viewport)-local  */
    fdk_f32 band_now_x, band_now_y; /* pointer, tree(viewport)-local */
    fdk_f32 band_anchor_cy; /* the anchor's CONTENT-space y — the
                              * List's auto-scroll rule (the anchor
                              * glues to the content; the sweep grows
                              * as the view chases the pointer)      */
    bool *band_base;     /* pre-band snapshot over ALL nodes (ctrl) */
    size_t band_base_cap;
    fdk_timer *band_scroll_timer;  /* edge-chasing auto-scroll      */
} fdk_tree;

static fdk_tree *tree_of(fdk_widget *w) {
    return (fdk_tree *)(void *)w;
}
static fdk_tree_row *trow_of(fdk_widget *w) {
    return (fdk_tree_row *)(void *)w;
}

static const fdk_widget_class fdk_tree_row_class_def;
extern const fdk_widget_class fdk_tree_class_def;

/* ---- geometry ---- */

static fdk_i32 tree_row_height(const fdk_tree *t) {
    if (t->font == NULL) {
        return 20;
    }
    fdk_font_metrics m;
    fdk_font_get_metrics(t->font, &m);
    fdk_i32 h = m.ascent + m.descent + TREE_ROW_PAD_Y * 2;
    return (h < 16) ? 16 : h;
}

/* Rebuilds the VISIBLE row sequence (pre-order, skipping collapsed
 * subtrees) and syncs the row widgets to it. O(nodes + rows). */
static void tree_rebuild_visible(fdk_tree *t);

static void tree_relayout(fdk_tree *t) {
    tree_rebuild_visible(t);
    if (t->rows == NULL) {
        return;
    }
    fdk_i32 rh = tree_row_height(t);
    fdk_i32 w = 80;
    for (size_t i = 0; i < t->row_count; i++) {
        fdk_tree_node_rec *n = &t->nodes[t->row_widgets[i]->node];
        fdk_i32 depth = 0;
        for (size_t p = n->parent; p != FDK_TREE_NODE_NONE;
             p = t->nodes[p].parent) {
            depth++;
        }
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(t->font, n->text, &tw, &th);
        fdk_i32 need = tw + TREE_INDENT * (depth + 1) +
                       TREE_EXPANDER + 8 +
                       fdk__row_icon_advance(n->icon);
        if (need > w) {
            w = need;
        }
    }
    for (size_t i = 0; i < t->row_count; i++) {
        fdk_rect r = { 0, (fdk_i32)(i * (size_t)rh), w, rh };
        fdk_widget_set_bounds(&t->row_widgets[i]->base, r);
    }
    fdk_widget_set_natural_size(t->rows, w,
                                (fdk_i32)(t->row_count * (size_t)rh));
    if (t->scroll != NULL && t->base.bounds.width > 0 &&
        t->base.bounds.height > 0) {
        fdk_rect inner = { 0, 0, t->base.bounds.width,
                           t->base.bounds.height };
        fdk_widget_set_bounds(t->scroll, inner);
    }
    fdk_widget_child_layout_changed(t->rows->parent);
}

/* ---- model helpers ---- */

static fdk_result tree_grow(fdk_tree *t) {
    if (t->count < t->capacity) {
        return FDK_OK;
    }
    size_t cap = (t->capacity == 0) ? 8 : t->capacity * 2;
    fdk_tree_node_rec *grown =
        fdk_realloc(t->nodes, cap * sizeof(*grown));
    if (grown == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    t->nodes = grown;
    t->capacity = cap;
    return FDK_OK;
}

/* Appends `node` (and, while expanded, its subtree in pre-order) to
 * the scratch visible sequence. */
static void walk_visible(fdk_tree *t, size_t node, size_t **vis,
                         size_t *n, size_t *cap) {
    if (*n == *cap) {
        size_t new_cap = (*cap == 0) ? 16 : *cap * 2;
        size_t *grown = fdk_realloc(*vis, new_cap * sizeof(**vis));
        if (grown == NULL) {
            return; /* OOM: visible list truncates honestly */
        }
        *vis = grown;
        *cap = new_cap;
    }
    (*vis)[(*n)++] = node;
    if (t->nodes[node].expanded) {
        for (size_t c = t->nodes[node].first_child;
             c != FDK_TREE_NODE_NONE; c = t->nodes[c].next_sibling) {
            walk_visible(t, c, vis, n, cap);
        }
    }
}

/* Rebuilds the visible row sequence and syncs row widgets to it:
 *   pass 1 derives the visible node indices (pre-order, skipping
 *   collapsed subtrees) into a scratch array;
 *   pass 2 creates/destroys "tree-row" widgets in the rows container
 *   until their count matches, then maps them onto the slots.
 * Row widgets are recreated top-down so z-order follows row order. */
static void tree_rebuild_visible(fdk_tree *t) {
    size_t *vis = NULL;
    size_t n = 0;
    size_t cap = 0;
    for (size_t i = 0; i < t->count; i++) {
        if (t->nodes[i].parent == FDK_TREE_NODE_NONE &&
            !t->nodes[i].dead) {
            walk_visible(t, i, &vis, &n, &cap);
        }
    }

    /* Current row-widget count in the container. */
    size_t existing = 0;
    if (t->rows != NULL) {
        size_t cn = fdk_widget_child_count(t->rows);
        for (size_t i = 0; i < cn; i++) {
            if (fdk_widget_child_at(t->rows, i)->klass ==
                &fdk_tree_row_class_def) {
                existing++;
            }
        }
    }

    /* Grow the widget pointer array to the visible count. */
    while (n > t->row_capacity) {
        size_t new_cap = (t->row_capacity == 0) ? 16
                                                : t->row_capacity * 2;
        fdk_tree_row **grown =
            fdk_realloc(t->row_widgets, new_cap * sizeof(*grown));
        if (grown == NULL) {
            n = t->row_capacity; /* truncate honestly */
            break;
        }
        t->row_widgets = grown;
        t->row_capacity = new_cap;
    }

    /* Create missing widgets (appended in order). */
    for (size_t i = existing; i < n; i++) {
        fdk_widget *rw = NULL;
        if (!fdk_ok(fdk_widget_create(t->rows, &fdk_tree_row_class_def,
                                      (fdk_rect){0, 0, 0, 0}, &rw))) {
            n = i;
            break;
        }
    }
    /* Destroy surplus widgets (from the top of the z-order). */
    if (existing > n && t->rows != NULL) {
        size_t to_kill = existing - n;
        while (to_kill > 0) {
            size_t cn = fdk_widget_child_count(t->rows);
            fdk_widget *victim = NULL;
            for (size_t k = cn; k-- > 0;) {
                fdk_widget *cw = fdk_widget_child_at(t->rows, k);
                if (cw->klass == &fdk_tree_row_class_def) {
                    victim = cw;
                    break;
                }
            }
            if (victim == NULL) {
                break;
            }
            fdk_widget_destroy(victim);
            to_kill--;
        }
    }

    /* Map widgets (in child order = row order) onto the slots. */
    size_t slot = 0;
    if (t->rows != NULL) {
        size_t cn = fdk_widget_child_count(t->rows);
        for (size_t i = 0; i < cn && slot < n; i++) {
            fdk_widget *cw = fdk_widget_child_at(t->rows, i);
            if (cw->klass == &fdk_tree_row_class_def) {
                trow_of(cw)->node = vis[slot];
                t->row_widgets[slot] = trow_of(cw);
                slot++;
            }
        }
    }
    t->row_count = slot;
    fdk_free(vis);
}

/* ---- selection ---- */

/* A11y helper: the row widget currently displaying `node`, or NULL
 * when the node is hidden (collapsed ancestor). Notifications fire
 * on rows because the a11y tree walks the WIDGET tree; a hidden
 * node simply has no a11y presence until it becomes visible. */
static void tree_notify_row_for_node(fdk_tree *t, size_t node,
                                      fdk_a11y_event_kind kind,
                                      fdk_a11y_state_flag flag) {
    for (size_t i = 0; i < t->row_count; i++) {
        if (t->row_widgets[i] != NULL &&
            t->row_widgets[i]->node == node) {
            fdk__a11y_notify(&t->row_widgets[i]->base, kind, flag);
            return;
        }
    }
}

static void tree_fire_changed(fdk_tree *t) {
    if (t->on_selection_changed != NULL) {
        t->on_selection_changed(&t->base, t->on_selection_data);
    }
}

static void tree_clear_selection(fdk_tree *t);

/* Sets one node's selected flag with the a11y notification riding
 * the same path every selection change takes. */
static void tree_set_node_selected(fdk_tree *t, size_t node, bool on) {
    if (node >= t->count || t->nodes[node].dead ||
        t->nodes[node].selected == on) {
        return;
    }
    t->nodes[node].selected = on;
    tree_notify_row_for_node(t, node, FDK_A11Y_STATE_CHANGED,
                             FDK_A11Y_SELECTED);
}

/* Selects the VISIBLE rows [lo, hi] (slot indices), replacing or
 * unioning with the current selection. */
static void tree_select_visible_range(fdk_tree *t, size_t lo,
                                       size_t hi, bool keep_others) {
    if (!keep_others) {
        tree_clear_selection(t);
    }
    if (lo > hi) {
        size_t tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (hi >= t->row_count) {
        hi = t->row_count - 1;
    }
    for (size_t i = lo; i <= hi && i < t->row_count; i++) {
        tree_set_node_selected(t, t->row_widgets[i]->node, true);
    }
    fdk_widget_invalidate_all(&t->base);
}

/* Click semantics over the VISIBLE sequence — the List's exact
 * model: plain replaces; ctrl toggles one; shift ranges from the
 * anchor; ctrl+shift ranges without clearing. SINGLE keeps the
 * classic behavior (modifiers read as a plain click). */
static void tree_row_clicked(fdk_tree *t, size_t vis_index,
                              fdk_u32 mods) {
    if (t->mode == FDK_TREE_SELECTION_NONE) {
        return;
    }
    size_t node = t->row_widgets[vis_index]->node;
    if (t->mode == FDK_TREE_SELECTION_SINGLE) {
        t->anchor = vis_index;
        (void)fdk_tree_select(&t->base, node);
        return;
    }
    bool ctrl = (mods & FDK_MOD_CTRL) != 0;
    bool shift = (mods & FDK_MOD_SHIFT) != 0;
    if (ctrl && shift) {
        tree_select_visible_range(t, t->anchor, vis_index, true);
        /* Anchor unchanged (ctrl keeps it). */
    } else if (shift) {
        tree_select_visible_range(t, t->anchor, vis_index, false);
    } else if (ctrl) {
        tree_set_node_selected(t, node, !t->nodes[node].selected);
        t->anchor = vis_index;
        fdk_widget_invalidate_all(&t->base);
    } else {
        tree_clear_selection(t);
        tree_set_node_selected(t, node, true);
        t->anchor = vis_index;
        fdk_widget_invalidate_all(&t->base);
    }
    tree_fire_changed(t);
}

static void tree_clear_selection(fdk_tree *t) {
    for (size_t i = 0; i < t->count; i++) {
        if (t->nodes[i].selected) {
            t->nodes[i].selected = false;
            /* A11y: fire on the row currently SHOWING this node
             * (invisible nodes have no row to announce them). */
            tree_notify_row_for_node(t, i, FDK_A11Y_STATE_CHANGED,
                                     FDK_A11Y_SELECTED);
        }
    }
    fdk_widget_invalidate_all(&t->base);
}

fdk_tree_node fdk_tree_get_selected(fdk_widget *tree) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_TREE_NODE_NONE;
    }
    fdk_tree *t = tree_of(tree);
    for (size_t i = 0; i < t->count; i++) {
        if (!t->nodes[i].dead && t->nodes[i].selected) {
            return i;
        }
    }
    return FDK_TREE_NODE_NONE;
}

fdk_result fdk_tree_select(fdk_widget *tree, fdk_tree_node node) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_tree *t = tree_of(tree);
    if (node != FDK_TREE_NODE_NONE &&
        (node >= t->count || t->nodes[node].dead)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tree_clear_selection(t);
    if (node != FDK_TREE_NODE_NONE) {
        t->nodes[node].selected = true;
        tree_notify_row_for_node(t, node, FDK_A11Y_STATE_CHANGED,
                                 FDK_A11Y_SELECTED);
    }
    tree_fire_changed(t);
    return FDK_OK;
}

/* ---- rows ---- */

static void row_paint(fdk_widget *w, fdk_surface *surface,
                      fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    /* row -> rows -> scrollview -> tree */
    fdk_widget *rows = w->parent;
    fdk_widget *scroll = rows ? rows->parent : NULL;
    fdk_widget *tree_w = scroll ? scroll->parent : NULL;
    if (tree_w == NULL || tree_w->klass != &fdk_tree_class_def) {
        return;
    }
    fdk_tree *t = tree_of(tree_w);
    fdk_tree_row *row = trow_of(w);
    if (row->node >= t->count) {
        return;
    }
    fdk_tree_node_rec *n = &t->nodes[row->node];

    if (n->selected) {
        /* The 1.3.2 SELECTION_BACKGROUND token, wired in 1.4.0. */
        fdk_surface_fill_rect(surface, bounds, fdk__pal_selection());
    } else if ((w->flags & FDK_WF_HOVERED) != 0 &&
               (w->flags & FDK_WF_ENABLED) != 0) {
        fdk_surface_fill_rect(surface, bounds, fdk__pal_row_hover());
    }

    fdk_i32 depth = 0;
    for (size_t p = n->parent; p != FDK_TREE_NODE_NONE;
         p = t->nodes[p].parent) {
        depth++;
    }
    fdk_i32 x = bounds.x + TREE_INDENT * (depth + 1);

    /* Expander: a small stroked triangle (right = collapsed, down =
     * expanded). Line primitives only — the same font-independent
     * vector-glyph discipline as the title-bar band buttons. */
    if (n->is_parent) {
        fdk_i32 cx = x - TREE_EXPANDER / 2 - 2;
        fdk_i32 cy = bounds.y + bounds.height / 2;
        fdk_i32 half = 4;
        fdk_color col = fdk__pal_text();
        fdk_i32 ax, ay, bx, by, dx, dy;
        if (n->expanded) {
            ax = cx - half; ay = cy - half / 2;
            bx = cx + half; by = cy - half / 2;
            dx = cx;        dy = cy + half / 2;
        } else {
            ax = cx - half / 2; ay = cy - half;
            bx = cx - half / 2; by = cy + half;
            dx = cx + half / 2; dy = cy;
        }
        fdk_surface_draw_line(surface, ax, ay, bx, by, col);
        fdk_surface_draw_line(surface, bx, by, dx, dy, col);
        fdk_surface_draw_line(surface, dx, dy, ax, ay, col);
    }

    if (t->font == NULL) {
        return;
    }
    fdk_i32 baseline = fdk__center_baseline(t->font, bounds.y,
                                            bounds.height);
    /* The symbolic glyph (1.4.2): the List's language, between the
     * expander zone and the text, vertically centered in its 16-px
     * box. */
    fdk_i32 text_x = x;
    if (n->icon != FDK_ROW_ICON_NONE) {
        fdk_i32 gy = bounds.y + (bounds.height - 16) / 2;
        if (gy < bounds.y) {
            gy = bounds.y;
        }
        fdk__row_icon_paint(surface, n->icon, text_x, gy,
                             (w->flags & FDK_WF_ENABLED) == 0);
        text_x += fdk__row_icon_advance(n->icon);
    }
    fdk__draw_text(surface, t->font, n->text, fdk__pal_text(), text_x,
                   baseline);
}

static bool row_handle_event(fdk_widget *w,
                             const fdk_widget_event *ev) {
    if (ev->type != FDK_WIDGET_POINTER_DOWN ||
        ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
        return false;
    }
    fdk_widget *rows = w->parent;
    fdk_widget *scroll = rows ? rows->parent : NULL;
    fdk_widget *tree_w = scroll ? scroll->parent : NULL;
    if (tree_w == NULL || tree_w->klass != &fdk_tree_class_def) {
        return false;
    }
    fdk_tree *t = tree_of(tree_w);
    fdk_tree_row *row = trow_of(w);
    if (row->node >= t->count) {
        return false;
    }
    fdk_tree_node_rec *n = &t->nodes[row->node];

    /* Expander zone? */
    if (n->is_parent) {
        fdk_i32 depth = 0;
        for (size_t p = n->parent; p != FDK_TREE_NODE_NONE;
             p = t->nodes[p].parent) {
            depth++;
        }
        fdk_i32 zone_x = TREE_INDENT * (depth + 1) - TREE_EXPANDER;
        fdk_f32 lx = ev->pointer.position.x;
        if (lx >= (fdk_f32)zone_x && lx < (fdk_f32)(zone_x + TREE_EXPANDER + 4)) {
            n->expanded = !n->expanded;
            tree_relayout(t);
            return true;
        }
    }
    /* 1.4.3: the multi-select click semantics (SINGLE reads every
     * modifier as a plain click — the classic behavior). */
    for (size_t i = 0; i < t->row_count; i++) {
        if (t->row_widgets[i]->node == row->node) {
            tree_row_clicked(t, i, ev->pointer.modifiers);
            break;
        }
    }
    return true;
}

static void row_destroy(fdk_widget *w) {
    (void)w; /* nothing owned per-row (text lives in the model) */
}

/* ---- a11y ---- */

/* row -> rows -> scrollview -> tree */
static fdk_widget *tree_of_row(const fdk_widget *row) {
    fdk_widget *cur = (row != NULL) ? row->parent : NULL;
    while (cur != NULL && cur->klass != &fdk_tree_class_def) {
        cur = cur->parent;
    }
    return cur;
}

static void tree_row_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    fdk_widget *tree_w = tree_of_row(w);
    if (tree_w == NULL) {
        return;
    }
    const fdk_tree *t = (const fdk_tree *)(const void *)tree_w;
    const fdk_tree_row *row = (const fdk_tree_row *)(const void *)w;
    if (row->node < t->count) {
        const fdk_tree_node_rec *n = &t->nodes[row->node];
        if (n->text != NULL) {
            out->name = fdk__strdup(n->text);
        }
        if (n->selected) {
            out->states |= FDK_A11Y_SELECTED;
        }
        if (n->is_parent) {
            if (n->expanded) {
                out->states |= FDK_A11Y_EXPANDED;
            }
        }
    }
}

static fdk_a11y_action_set tree_row_a11y_actions(const fdk_widget *w) {
    fdk_widget *tree_w = tree_of_row(w);
    if (tree_w == NULL) {
        return 0;
    }
    const fdk_tree *t = (const fdk_tree *)(const void *)tree_w;
    const fdk_tree_row *row = (const fdk_tree_row *)(const void *)w;
    if (row->node >= t->count || !t->nodes[row->node].is_parent) {
        return (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE;
    }
    return FDK_A11Y_ACTION_ACTIVATE | FDK_A11Y_ACTION_EXPAND |
           FDK_A11Y_ACTION_COLLAPSE;
}

static bool tree_row_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                  double value) {
    (void)value;
    fdk_widget *tree_w = tree_of_row(w);
    if (tree_w == NULL) {
        return false;
    }
    fdk_tree *t = tree_of(tree_w);
    fdk_tree_row *row = trow_of(w);
    if (row->node >= t->count) {
        return false;
    }
    switch (action) {
    case FDK_A11Y_ACTION_ACTIVATE:
        return fdk_ok(fdk_tree_select(tree_w, row->node));
    case FDK_A11Y_ACTION_EXPAND:
    case FDK_A11Y_ACTION_COLLAPSE: {
        bool want = (action == FDK_A11Y_ACTION_EXPAND);
        if (!t->nodes[row->node].is_parent ||
            t->nodes[row->node].expanded == want) {
            return false;
        }
        return fdk_ok(fdk_tree_node_expand(tree_w, row->node, want));
    }
    default:
        return false;
    }
}

static const fdk_a11y_class tree_row_a11y = {
    .role = FDK_A11Y_ROLE_TREE_ITEM,
    .describe = tree_row_a11y_describe,
    .actions = tree_row_a11y_actions,
    .perform = tree_row_a11y_perform,
};

static const fdk_widget_class fdk_tree_row_class_def = {
    .size = sizeof(fdk_tree_row),
    .name = "tree-row",
    .handle_event = row_handle_event,
    .paint = row_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = row_destroy,
    .a11y = &tree_row_a11y,
};

/* ---- tree-level events: keyboard + the 1.4.3 rubber band ---- */

/* Applies the band's live selection: every VISIBLE row whose
 * content-space extent intersects the band (viewport y + scroll
 * offset), unioned with the pre-band snapshot when Ctrl armed the
 * sweep — the List's exact mapping over the visible sequence. */
static void tree_apply_band(fdk_tree *t) {
    if (t->row_count == 0) {
        return;
    }
    fdk_i32 rh = tree_row_height(t);
    fdk_i32 off_y = 0;
    if (t->scroll != NULL) {
        fdk_i32 ox = 0;
        fdk_scrollview_get_scroll_offset(t->scroll, &ox, &off_y);
    }
    /* The ANCHOR is content-space; the moving edge rides the
     * CURRENT offset — the List's auto-scroll extension rule. */
    fdk_f32 now_cy = t->band_now_y + (fdk_f32)off_y;
    fdk_f32 y1 = (t->band_anchor_cy < now_cy) ? t->band_anchor_cy
                                               : now_cy;
    fdk_f32 y2 = (t->band_anchor_cy < now_cy) ? now_cy
                                               : t->band_anchor_cy;
    fdk_i64 lo = (fdk_i64)y1 - 1;
    fdk_i64 hi = (fdk_i64)y2 + 1;
    fdk_i64 r_lo = lo / rh;
    if (r_lo < 0) {
        r_lo = 0;
    }
    fdk_i64 r_hi = (hi + rh - 1) / rh; /* exclusive */
    if (r_hi > (fdk_i64)t->row_count) {
        r_hi = (fdk_i64)t->row_count;
    }
    bool changed = false;
    for (size_t i = 0; i < t->row_count; i++) {
        bool want = ((fdk_i64)i >= r_lo && (fdk_i64)i < r_hi) ||
                    (t->band_ctrl && t->band_base != NULL &&
                     t->row_widgets[i]->node < t->band_base_cap &&
                     t->band_base[t->row_widgets[i]->node]);
        size_t node = t->row_widgets[i]->node;
        if (t->nodes[node].selected != want) {
            tree_set_node_selected(t, node, want);
            changed = true;
        }
    }
    if (changed) {
        fdk_widget_invalidate_all(&t->base);
    }
}

/* The band auto-scroll constants — the List's values, shared by
 * policy (the same feel everywhere). */
#define TREE_BAND_SCROLL_MS 40
#define TREE_BAND_SCROLL_STEP 24
#define TREE_BAND_EDGE 16

static void tree_band_scroll_disarm(fdk_tree *t) {
    if (t->band_scroll_timer != NULL) {
        fdk_timer_remove(t->band_scroll_timer);
        t->band_scroll_timer = NULL;
    }
}

static void tree_band_scroll_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_tree *t = user;
    if (!t->banding || t->scroll == NULL) {
        t->band_scroll_timer = NULL;
        return;
    }
    fdk_i32 off_x = 0, off_y = 0;
    fdk_scrollview_get_scroll_offset(t->scroll, &off_x, &off_y);
    (void)off_x;
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(t->scroll, &vw, &vh);
    (void)vw;
    fdk_i32 dy = 0;
    if (t->band_now_y < (fdk_f32)TREE_BAND_EDGE) {
        dy = -TREE_BAND_SCROLL_STEP;
    } else if (t->band_now_y > (fdk_f32)(vh - TREE_BAND_EDGE)) {
        dy = TREE_BAND_SCROLL_STEP;
    }
    if (dy == 0) {
        tree_band_scroll_disarm(t);
        return;
    }
    fdk_i32 want = off_y + dy;
    if (want < 0) {
        want = 0;
    }
    (void)fdk_scrollview_scroll_to(t->scroll, 0, want);
    fdk_i32 new_off_y = 0;
    fdk_scrollview_get_scroll_offset(t->scroll, &off_x, &new_off_y);
    if (new_off_y == off_y) {
        tree_band_scroll_disarm(t);
        return;
    }
    tree_apply_band(t);
    fdk_widget_invalidate(&t->base);
}

static void tree_band_scroll_track(fdk_tree *t) {
    if (t->scroll == NULL) {
        return;
    }
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(t->scroll, &vw, &vh);
    (void)vw;
    bool outside = t->band_now_y < (fdk_f32)TREE_BAND_EDGE ||
                   t->band_now_y > (fdk_f32)(vh - TREE_BAND_EDGE);
    if (!outside) {
        tree_band_scroll_disarm(t);
        return;
    }
    if (t->band_scroll_timer == NULL) {
        fdk_context *ctx =
            fdk__window_context(fdk__widget_window_owner(&t->base));
        if (ctx != NULL) {
            t->band_scroll_timer =
                fdk_timer_add(ctx, TREE_BAND_SCROLL_MS, true,
                              tree_band_scroll_tick, t);
        }
    }
}

static bool tree_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev) {
    fdk_tree *t = tree_of(w);

    /* ---- the rubber-band sweep (1.4.3) ----
     *
     * A left press reaching the TREE itself is on EMPTY space (rows
     * consume their own presses; the scrollview does not consume
     * presses) — the classic band start. MULTIPLE mode only; the
     * implicit grab (press returned true) delivers the motions and
     * the release to the tree alone. Ctrl arms a union sweep. */
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT ||
            t->mode != FDK_TREE_SELECTION_MULTIPLE ||
            (w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        t->banding = true;
        t->band_ctrl = (ev->pointer.modifiers & FDK_MOD_CTRL) != 0;
        t->band_x = t->band_now_x = ev->pointer.position.x;
        t->band_y = t->band_now_y = ev->pointer.position.y;
        /* The anchor glues to the CONTENT (the List's rule): the
         * sweep grows under auto-scroll, the anchor row stays. */
        {
            fdk_i32 ox = 0, oy = 0;
            if (t->scroll != NULL) {
                fdk_scrollview_get_scroll_offset(t->scroll, &ox, &oy);
            }
            t->band_anchor_cy = t->band_y + (fdk_f32)oy;
        }
        /* Ctrl-union snapshot: the selection as the sweep found it
         * (over ALL nodes — collapsed ones too; the model holds the
         * state, the walk only shows it). */
        if (t->band_ctrl && t->count > 0) {
            bool *snap = fdk_alloc_array(t->count, sizeof(bool));
            if (snap != NULL) {
                for (size_t i = 0; i < t->count; i++) {
                    snap[i] = t->nodes[i].selected;
                }
                t->band_base = snap;
                t->band_base_cap = t->count;
            }
        } else {
            fdk_free(t->band_base);
            t->band_base = NULL;
            t->band_base_cap = 0;
        }
        /* A zero-extent band selects nothing: the plain sweep clears
         * (an empty-space click deselecting); the ctrl sweep keeps.
         * A press that STARTS at the edge chases immediately (the
         * motion path's rule, at the gesture's birth — the List's). */
        tree_apply_band(t);
        tree_band_scroll_track(t);
        tree_fire_changed(t);
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION:
        if (!t->banding) {
            return false;
        }
        t->band_now_x = ev->position.x;
        t->band_now_y = ev->position.y;
        tree_apply_band(t);
        tree_band_scroll_track(t);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (!t->banding) {
            return false;
        }
        t->banding = false;
        tree_band_scroll_disarm(t);
        fdk_free(t->band_base);
        t->band_base = NULL;
        t->band_base_cap = 0;
        /* The band rect disappears with the gesture: repaint. */
        fdk_widget_invalidate(w);
        /* One gesture: the callback fires here (and at the press). */
        tree_fire_changed(t);
        return true;
    default:
        break;
    }

    if (ev->type != FDK_WIDGET_KEY_DOWN ||
        (w->flags & FDK_WF_FOCUSED) == 0 || t->row_count == 0) {
        return false;
    }
    fdk_tree_node sel = fdk_tree_get_selected(w);
    /* The keyboard cursor is the SELECTED visible row (single
     * selection makes it unambiguous); -1 when nothing is selected —
     * the cold start, where Down selects the FIRST row. */
    fdk_i64 cur = -1;
    if (sel != FDK_TREE_NODE_NONE) {
        for (size_t i = 0; i < t->row_count; i++) {
            if (t->row_widgets[i]->node == sel) {
                cur = (fdk_i64)i;
                break;
            }
        }
    }
    fdk_i64 next = cur;
    switch (ev->key.scancode) {
    case FDK_KEY_UP:
        next = (cur <= 0) ? 0 : cur - 1;
        break;
    case FDK_KEY_DOWN:
        next = (cur < 0) ? 0
                         : ((cur + 1 < (fdk_i64)t->row_count)
                                ? cur + 1
                                : cur);
        break;
    case FDK_KEY_HOME:
        next = 0;
        break;
    case FDK_KEY_END:
        next = (fdk_i64)t->row_count - 1;
        break;
    case FDK_KEY_PAGE_UP:
    case FDK_KEY_PAGE_DOWN: {
        fdk_i32 vh = 160;
        if (t->scroll != NULL) {
            fdk_i32 vw2 = 0;
            fdk__scrollview_viewport(t->scroll, &vw2, &vh);
        }
        fdk_i32 rh = tree_row_height(t);
        fdk_i64 page = (rh > 0) ? (vh / rh) : 10;
        if (page < 1) {
            page = 1;
        }
        next = (ev->key.scancode == FDK_KEY_PAGE_UP)
            ? cur - page
            : cur + page;
        if (next < 0) {
            next = 0;
        }
        if (next >= (fdk_i64)t->row_count) {
            next = (fdk_i64)t->row_count - 1;
        }
        break;
    }
    case FDK_KEY_LEFT: {
        /* Collapsed parent: collapse. Expanded parent: jump to its
         * parent. Leaf: jump to its parent. */
        if (cur < 0) {
            return true; /* nothing selected: nothing to collapse */
        }
        size_t node = t->row_widgets[cur]->node;
        fdk_tree_node_rec *n = &t->nodes[node];
        if (n->is_parent && n->expanded) {
            n->expanded = false;
            tree_relayout(t);
        } else if (n->parent != FDK_TREE_NODE_NONE) {
            for (size_t i = 0; i < t->row_count; i++) {
                if (t->row_widgets[i]->node == n->parent) {
                    t->anchor = i;
                    (void)fdk_tree_select(w, n->parent);
                    return true;
                }
            }
        }
        return true;
    }
    case FDK_KEY_RIGHT: {
        if (cur < 0) {
            return true;
        }
        size_t node = t->row_widgets[cur]->node;
        fdk_tree_node_rec *n = &t->nodes[node];
        if (n->is_parent && !n->expanded) {
            n->expanded = true;
            tree_relayout(t);
        } else if (n->is_parent && n->first_child != FDK_TREE_NODE_NONE) {
            tree_relayout(t); /* structure current */
            for (size_t i = 0; i < t->row_count; i++) {
                if (t->row_widgets[i]->node == n->first_child) {
                    t->anchor = i;
                    (void)fdk_tree_select(w, n->first_child);
                    return true;
                }
            }
        }
        return true;
    }
    default:
        return false;
    }
    if (next != cur || sel == FDK_TREE_NODE_NONE) {
        if (next >= 0) {
            /* 1.4.3: shift extends the range from the anchor in
             * MULTIPLE mode (the keyboard twin of shift-click); plain
             * keys take the classic single-select walk. */
            bool shift = (ev->key.modifiers & FDK_MOD_SHIFT) != 0;
            if (t->mode == FDK_TREE_SELECTION_MULTIPLE && shift) {
                tree_select_visible_range(t, t->anchor, (size_t)next,
                                          false);
                tree_fire_changed(t);
            } else {
                t->anchor = (size_t)next;
                (void)fdk_tree_select(w, t->row_widgets[next]->node);
            }
        }
    }
    return true;
}

static void tree_measure(fdk_widget *w, fdk_size *out) {
    fdk_tree *t = tree_of(w);
    fdk_i32 rh = tree_row_height(t);
    size_t shown = (t->row_count < 8) ? t->row_count : 8;
    fdk_i32 width = 80;
    for (size_t i = 0; i < t->row_count; i++) {
        fdk_tree_node_rec *n = &t->nodes[t->row_widgets[i]->node];
        fdk_i32 depth = 0;
        for (size_t p = n->parent; p != FDK_TREE_NODE_NONE;
             p = t->nodes[p].parent) {
            depth++;
        }
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(t->font, n->text, &tw, &th);
        fdk_i32 need = tw + TREE_INDENT * (depth + 1) + TREE_EXPANDER +
                       8 + fdk__row_icon_advance(n->icon);
        if (need > width) {
            width = need;
        }
    }
    out->width = width;
    out->height = (fdk_i32)(shown * (size_t)rh);
    if (out->height == 0) {
        out->height = rh;
    }
}

static void tree_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_tree *t = tree_of(w);
    if (t->scroll != NULL) {
        fdk_rect inner = { 0, 0, assigned.width, assigned.height };
        fdk_widget_set_bounds(t->scroll, inner);
        fdk_widget_child_layout_changed(t->scroll);
    }
}

/* The tree's own paint: the rubber band under the rows (the tree's
 * paint runs before its scrollview subtree, so selection fills land
 * over the tint — the List's exact layering, accent-derived). */
static void tree_paint(fdk_widget *w, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_tree *t = tree_of(w);
    if (!t->banding || bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_f32 x1 = (t->band_x < t->band_now_x) ? t->band_x
                                              : t->band_now_x;
    fdk_f32 x2 = (t->band_x < t->band_now_x) ? t->band_now_x
                                              : t->band_x;
    /* The anchor's on-screen y tracks the CONTENT it is glued to
     * (the List's rule); the moving edge is the pointer. */
    fdk_i32 ay_off = 0;
    if (t->scroll != NULL) {
        fdk_i32 axo = 0;
        fdk_scrollview_get_scroll_offset(t->scroll, &axo, &ay_off);
    }
    fdk_f32 anchor_vy = t->band_anchor_cy - (fdk_f32)ay_off;
    fdk_f32 y1 = (anchor_vy < t->band_now_y) ? anchor_vy
                                              : t->band_now_y;
    fdk_f32 y2 = (anchor_vy < t->band_now_y) ? t->band_now_y
                                              : anchor_vy;
    fdk_i32 bx1 = (fdk_i32)x1 + bounds.x;
    fdk_i32 by1 = (fdk_i32)y1 + bounds.y;
    fdk_i32 bx2 = (fdk_i32)x2 + bounds.x;
    fdk_i32 by2 = (fdk_i32)y2 + bounds.y;
    /* Clamp the edges to the bounds (the grab delivers out-of-widget
     * coordinates while the pointer is dragged past the edge) — the
     * List's exact clamping. */
    if (bx1 < bounds.x) {
        bx1 = bounds.x;
    }
    if (by1 < bounds.y) {
        by1 = bounds.y;
    }
    if (bx2 > bounds.x + bounds.width) {
        bx2 = bounds.x + bounds.width;
    }
    if (by2 > bounds.y + bounds.height) {
        by2 = bounds.y + bounds.height;
    }
    fdk_i32 bw = bx2 - bx1;
    fdk_i32 bh = by2 - by1;
    if (bw <= 0 || bh <= 0) {
        return;
    }
    fdk_color accent = fdk__pal_accent();
    fdk_color fill = {accent.r, accent.g, accent.b, accent.a * 0.18f};
    fdk_color edge = {accent.r, accent.g, accent.b, accent.a * 0.55f};
    fdk_rect band = {bx1, by1, bw, bh};
    fdk_surface_fill_rect(surface, band, fill);
    fdk_surface_draw_rect(surface, band, edge);
}

static void tree_destroy(fdk_widget *w) {
    fdk_tree *t = tree_of(w);
    tree_band_scroll_disarm(t);
    fdk_free(t->band_base); /* a band interrupted by destruction */
    for (size_t i = 0; i < t->count; i++) {
        fdk_free(t->nodes[i].text);
    }
    fdk_free(t->nodes);
    fdk_free(t->row_widgets);
}

static const fdk_a11y_class tree_a11y = {
    .role = FDK_A11Y_ROLE_TREE,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_tree_class_def = {
    .size = sizeof(fdk_tree),
    .name = "tree",
    .handle_event = tree_handle_event,
    .paint = tree_paint,
    .measure = tree_measure,
    .arrange = tree_arrange,
    .destroy = tree_destroy,
    .a11y = &tree_a11y,
};

/* ---- public API ---- */

fdk_result fdk_tree_create(fdk_widget *parent, fdk_font *font,
                           fdk_widget **out_tree) {
    if (out_tree == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_tree_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_tree *t = tree_of(w);
    t->font = font;
    t->anchor = 0;
    fdk_widget_set_can_focus(w, true);

    r = fdk_scrollview_create(w, &t->scroll);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    r = fdk_widget_create(t->scroll, NULL, (fdk_rect){0, 0, 0, 0},
                          &t->rows);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    (void)fdk_scrollview_set_content(t->scroll, t->rows);

    fdk_widget_child_layout_changed(w->parent);
    *out_tree = w;
    return FDK_OK;
}

fdk_result fdk_tree_node_add(fdk_widget *tree, fdk_tree_node parent,
                             const char *text, fdk_tree_node *out_node) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_tree *t = tree_of(tree);
    if (parent != FDK_TREE_NODE_NONE &&
        (parent >= t->count || t->nodes[parent].dead)) {
        return FDK_ERR_INVALID_ARGUMENT; /* removed nodes are gone */
    }
    fdk_result r = tree_grow(t);
    if (!fdk_ok(r)) {
        return r;
    }
    char *copy = fdk__strdup(text != NULL ? text : "");
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t idx = t->count;
    t->nodes[idx] = (fdk_tree_node_rec){
        .text = copy,
        .parent = parent,
        .first_child = FDK_TREE_NODE_NONE,
        .next_sibling = FDK_TREE_NODE_NONE,
        .expanded = false,
        .selected = false,
        .is_parent = false,
        .dead = false,
        .icon = FDK_ROW_ICON_NONE,
    };
    t->count++;
    if (parent != FDK_TREE_NODE_NONE) {
        fdk_tree_node_rec *p = &t->nodes[parent];
        p->is_parent = true;
        if (p->first_child == FDK_TREE_NODE_NONE) {
            p->first_child = idx;
        } else {
            size_t c = p->first_child;
            while (t->nodes[c].next_sibling != FDK_TREE_NODE_NONE) {
                c = t->nodes[c].next_sibling;
            }
            t->nodes[c].next_sibling = idx;
        }
    }
    tree_relayout(t);
    if (out_node != NULL) {
        *out_node = idx;
    }
    return FDK_OK;
}

/* ---- node icons (1.4.2) ---- */

fdk_result fdk_tree_node_set_icon(fdk_widget *tree,
                                  fdk_tree_node node, fdk_row_icon icon) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_tree *t = tree_of(tree);
    if (node == FDK_TREE_NODE_NONE || node >= t->count ||
        t->nodes[node].dead) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    switch (icon) {
    case FDK_ROW_ICON_NONE:
    case FDK_ROW_ICON_FOLDER:
    case FDK_ROW_ICON_HOME:
    case FDK_ROW_ICON_DRIVE:
    case FDK_ROW_ICON_FILE:
        break;
    default:
        return FDK_ERR_INVALID_ARGUMENT; /* unknown values refused, */
    }                                      /* not coerced            */
    if (t->nodes[node].icon == icon) {
        return FDK_OK;
    }
    t->nodes[node].icon = icon;
    /* The row's width accounting changed: re-layout (the batched
     * fill path folds this into one settle when the app batches). */
    tree_relayout(t);
    return FDK_OK;
}

fdk_row_icon fdk_tree_node_get_icon(fdk_widget *tree,
                                    fdk_tree_node node) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_ROW_ICON_NONE;
    }
    fdk_tree *t = tree_of(tree);
    if (node == FDK_TREE_NODE_NONE || node >= t->count ||
        t->nodes[node].dead) {
        return FDK_ROW_ICON_NONE;
    }
    return t->nodes[node].icon;
}

/* ---- node removal (1.3.0) ----
 *
 * Handles are indices into the node store, and the store NEVER
 * reuses an index — removal TOMBSTONES the record (dead = true,
 * text freed, links left in place) so every OTHER live handle in the
 * application's hands stays stable, which was always the contract.
 * The sibling chain is unlinked so walks never enter the dead
 * subtree; the root scan and child counts skip dead records
 * defensively as well. */

/* Kills a subtree: tombstones every record (freeing texts), reports
 * whether anything in it was selected. Links are NOT torn down —
 * dead links are simply never followed. */
static bool tree_kill_subtree(fdk_tree *t, size_t node) {
    bool had_selection = t->nodes[node].selected;
    fdk_free(t->nodes[node].text);
    t->nodes[node].text = NULL;
    t->nodes[node].dead = true;
    t->nodes[node].selected = false;
    for (size_t c = t->nodes[node].first_child;
         c != FDK_TREE_NODE_NONE; c = t->nodes[c].next_sibling) {
        if (tree_kill_subtree(t, c)) {
            had_selection = true;
        }
    }
    return had_selection;
}

fdk_result fdk_tree_node_remove(fdk_widget *tree, fdk_tree_node node) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_tree *t = tree_of(tree);
    if (node >= t->count || t->nodes[node].dead) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Unlink from the parent's sibling chain (dead links are never
     * followed, but unlinking keeps is_parent truthful). */
    size_t parent = t->nodes[node].parent;
    if (parent != FDK_TREE_NODE_NONE) {
        if (t->nodes[parent].first_child == node) {
            t->nodes[parent].first_child = t->nodes[node].next_sibling;
        } else {
            size_t c = t->nodes[parent].first_child;
            while (c != FDK_TREE_NODE_NONE &&
                   t->nodes[c].next_sibling != node) {
                c = t->nodes[c].next_sibling;
            }
            if (c != FDK_TREE_NODE_NONE) {
                t->nodes[c].next_sibling = t->nodes[node].next_sibling;
            }
        }
        if (t->nodes[parent].first_child == FDK_TREE_NODE_NONE) {
            t->nodes[parent].is_parent = false;
        }
    }
    bool had_selection = tree_kill_subtree(t, node);
    tree_relayout(t);
    /* Virtual children (the visible rows) changed for a11y. */
    fdk__a11y_notify(&t->base, FDK_A11Y_CHILDREN_CHANGED, 0);
    if (had_selection) {
        /* The selection died with its node: clear, fire once. */
        tree_fire_changed(t);
    }
    return FDK_OK;
}

/* Removes every node. Unlike node_remove (tombstones, handles of
 * the survivors stay stable), clear RESETS the store — every handle
 * from before the clear is invalid, which is the only honest
 * contract for "the whole model is gone". */
void fdk_tree_clear(fdk_widget *tree) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return;
    }
    fdk_tree *t = tree_of(tree);
    bool had_selection = false;
    for (size_t i = 0; i < t->count; i++) {
        if (t->nodes[i].selected) {
            had_selection = true;
        }
        fdk_free(t->nodes[i].text);
    }
    t->count = 0;
    t->anchor = 0;
    tree_relayout(t);
    fdk__a11y_notify(&t->base, FDK_A11Y_CHILDREN_CHANGED, 0);
    if (had_selection) {
        tree_fire_changed(t);
    }
}

fdk_result fdk_tree_node_set_text(fdk_widget *tree, fdk_tree_node node,
                                  const char *text) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_tree *t = tree_of(tree);
    if (node >= t->count || t->nodes[node].dead) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = fdk__strdup(text != NULL ? text : "");
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(t->nodes[node].text);
    t->nodes[node].text = copy;
    tree_relayout(t);
    return FDK_OK;
}

const char *fdk_tree_node_text(fdk_widget *tree, fdk_tree_node node) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return NULL;
    }
    fdk_tree *t = tree_of(tree);
    if (node >= t->count || t->nodes[node].dead) {
        return NULL;
    }
    return t->nodes[node].text;
}

fdk_result fdk_tree_node_expand(fdk_widget *tree, fdk_tree_node node,
                                bool expanded) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_tree *t = tree_of(tree);
    if (node >= t->count || !t->nodes[node].is_parent ||
        t->nodes[node].dead) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (t->nodes[node].expanded != expanded) {
        t->nodes[node].expanded = expanded;
        /* A11y: fire BEFORE the relayout remaps rows to nodes. */
        tree_notify_row_for_node(t, node, FDK_A11Y_STATE_CHANGED,
                                 FDK_A11Y_EXPANDED);
        tree_relayout(t);
    }
    return FDK_OK;
}

bool fdk_tree_node_is_expanded(fdk_widget *tree, fdk_tree_node node) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return false;
    }
    fdk_tree *t = tree_of(tree);
    if (node >= t->count) {
        return false;
    }
    return t->nodes[node].expanded;
}

size_t fdk_tree_node_child_count(fdk_widget *tree, fdk_tree_node node) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return 0;
    }
    fdk_tree *t = tree_of(tree);
    if (node != FDK_TREE_NODE_NONE &&
        (node >= t->count || t->nodes[node].dead)) {
        return 0;
    }
    size_t parent = (node == FDK_TREE_NODE_NONE) ? FDK_TREE_NODE_NONE
                                                 : node;
    size_t n = 0;
    for (size_t c = (parent == FDK_TREE_NODE_NONE)
             ? FDK_TREE_NODE_NONE
             : t->nodes[parent].first_child;
         c != FDK_TREE_NODE_NONE; c = t->nodes[c].next_sibling) {
        if (!t->nodes[c].dead) {
            n++;
        }
    }
    if (parent == FDK_TREE_NODE_NONE) {
        /* Root-level count. */
        n = 0;
        for (size_t i = 0; i < t->count; i++) {
            if (t->nodes[i].parent == FDK_TREE_NODE_NONE &&
                !t->nodes[i].dead) {
                n++;
            }
        }
    }
    return n;
}

size_t fdk_tree_visible_count(fdk_widget *tree) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return 0;
    }
    return tree_of(tree)->row_count;
}

void fdk_tree_set_on_selection_changed(fdk_widget *tree,
                                       fdk_tree_selection_fn fn,
                                       void *user_data) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return;
    }
    fdk_tree *t = tree_of(tree);
    t->on_selection_changed = fn;
    t->on_selection_data = user_data;
}

/* ---- selection mode + multi-selection API (1.4.3) ---- */

void fdk_tree_set_selection_mode(fdk_widget *tree,
                                 fdk_tree_selection_mode mode) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return;
    }
    switch (mode) {
    case FDK_TREE_SELECTION_SINGLE:
    case FDK_TREE_SELECTION_NONE:
    case FDK_TREE_SELECTION_MULTIPLE:
        break;
    default:
        return; /* unknown values ignored (documented) */
    }
    fdk_tree *t = tree_of(tree);
    if (t->mode == mode) {
        return;
    }
    /* Switching modes resets the selection to whatever SINGLE would
     * show of it: the first selected node, or none. A stale
     * multi-selection under a SINGLE tree is a lie waiting to be
     * read. */
    fdk_tree_node keep = (mode == FDK_TREE_SELECTION_NONE)
        ? FDK_TREE_NODE_NONE
        : fdk_tree_get_selected(tree);
    tree_clear_selection(t);
    if (keep != FDK_TREE_NODE_NONE) {
        t->nodes[keep].selected = true;
        tree_notify_row_for_node(t, keep, FDK_A11Y_STATE_CHANGED,
                                 FDK_A11Y_SELECTED);
    }
    t->mode = mode;
    fdk_widget_invalidate(tree);
}

fdk_tree_selection_mode fdk_tree_get_selection_mode(fdk_widget *tree) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return FDK_TREE_SELECTION_SINGLE;
    }
    return tree_of(tree)->mode;
}

size_t fdk_tree_get_selected_nodes(fdk_widget *tree,
                                   fdk_tree_node *out_nodes,
                                   size_t max_nodes) {
    if (tree == NULL || tree->klass != &fdk_tree_class_def) {
        return 0;
    }
    fdk_tree *t = tree_of(tree);
    size_t written = 0;
    for (size_t i = 0; i < t->count && written < max_nodes; i++) {
        if (!t->nodes[i].dead && t->nodes[i].selected) {
            if (out_nodes != NULL) {
                out_nodes[written] = i;
            }
            written++;
        }
    }
    return written;
}
