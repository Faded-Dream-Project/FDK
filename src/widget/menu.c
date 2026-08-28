#define FDK_LOG_TAG "widgets"

/*
 * menu.c — Menu model + popup session + MenuBar (Phase 9 completion)
 *
 * Three cooperating pieces:
 *
 *   1. The MODEL (fdk_menu): an item list the application owns.
 *      Items are individually allocated so handles stay stable as
 *      the list grows.
 *
 *   2. The VIEW (fdk_menu_view_class_def): a widget that renders a
 *      model's items (rows, check/radio glyphs, submenu arrows,
 *      shortcut labels) and turns pointer/keyboard input into model
 *      actions. Views live inside popup windows' roots.
 *
 *   3. The SESSION (fdk_menu_session): the popup-window chain. It
 *      owns the toolkit popup windows (auto-painted, destroy-
 *      notified), routes "open/close/switch" between bar, submenus,
 *      and the keyboard, and tears the chain down on dismissal.
 *
 * Sessions are static slots (max 4 concurrent chains: one per
 * menubar/context/combo trigger — a fifth popup request closes the
 * oldest first, with a warning). The view and the bar hold borrowed
 * session pointers; the destroy-notify hook on every popup window
 * keeps those references from dangling across any destroy path
 * (app-driven, parent-window sweep, shutdown force-destroy).
 *
 * X11 vs Wayland grab notes: X11 grabs do not stack, so closing a
 * submenu re-asserts the parent popup's grab (fdk__window_regrab);
 * out-of-bounds MOTION under an X11 grab still arrives at the popup
 * (reported against the grab window), which the session uses to
 * keep bar hover-switching alive. Wayland's xdg_popup grab DOES
 * return to the parent automatically (protocol guarantee), and
 * nested popups MUST each grab ("the parent of a grabbing popup
 * must ... be another xdg_popup with an explicit grab") — both
 * behaviors live in the backend, not here.
 */

#include "widgets_internal.h"
#include "menu_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>
#include <stdio.h>

/* ---- tunables (layout constants; the row height minimum is the
 * themed FDK_TM_MENU_ITEM_HEIGHT metric) ---- */

#define MENU_PAD_X 8       /* row text left/right padding        */
#define MENU_GUTTER 22     /* check/radio gutter width           */
#define MENU_ARROW_GUTTER 18 /* submenu arrow column width       */
#define MENU_SEP_H 9       /* separator row height               */
#define MENU_MIN_W 48      /* never narrower than this           */
#define MENU_MAX_H 512     /* clamp: no scrolling in v1 (docs)   */
#define MENU_FONT_PAD 8    /* row height = font extent + this   */

#define BAR_PAD_X 10       /* menubar title padding              */
#define BAR_LEFT 6         /* menubar leading inset              */

/* =====================================================================
 * The model
 * ===================================================================== */

typedef enum fdk_menu_item_kind {
    FDK_MIK_NORMAL = 0,
    FDK_MIK_SEPARATOR = 1,
    FDK_MIK_CHECK = 2,
    FDK_MIK_RADIO = 3,
} fdk_menu_item_kind;

struct fdk_menu_item {
    char *text;       /* owned; NULL for separators          */
    char *shortcut;   /* owned display-only label            */
    fdk_menu_item_kind kind;
    bool enabled;
    bool checked;
    fdk_menu_activate_fn on_activate; /* per-item */
    void *on_activate_user;
    fdk_menu *submenu; /* borrowed */
};

struct fdk_menu {
    fdk_font *font;  /* borrowed — shared by every view of it */
    fdk_menu_item **items;
    size_t count;
    size_t cap;
    fdk_menu_activate_fn on_activate; /* menu-wide fallback */
    void *on_activate_user;
};

fdk_result fdk_menu_create(fdk_font *font, fdk_menu **out_menu) {
    if (out_menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu *m = fdk_alloc(sizeof(fdk_menu));
    if (m == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    m->font = font;
    m->items = NULL;
    m->count = 0;
    m->cap = 0;
    m->on_activate = NULL;
    m->on_activate_user = NULL;
    *out_menu = m;
    return FDK_OK;
}

void fdk_menu_destroy(fdk_menu *menu) {
    if (menu == NULL) {
        return;
    }
    for (size_t i = 0; i < menu->count; i++) {
        fdk_free(menu->items[i]->text);
        fdk_free(menu->items[i]->shortcut);
        fdk_free(menu->items[i]);
    }
    fdk_free(menu->items);
    fdk_free(menu);
}

size_t fdk_menu_item_count(fdk_menu *menu) {
    return (menu != NULL) ? menu->count : 0;
}

static fdk_menu_item *menu_new_item(fdk_menu *menu, const char *text,
                                    fdk_menu_item_kind kind) {
    if (menu->count == menu->cap) {
        size_t ncap = menu->cap * 2 + 4;
        fdk_menu_item **ni =
            fdk_realloc(menu->items, ncap * sizeof(fdk_menu_item *));
        if (ni == NULL) {
            return NULL;
        }
        menu->items = ni;
        menu->cap = ncap;
    }
    fdk_menu_item *it = fdk_alloc(sizeof(fdk_menu_item));
    if (it == NULL) {
        return NULL;
    }
    it->text = (text != NULL) ? fdk__strdup(text) : NULL;
    if (text != NULL && it->text == NULL) {
        fdk_free(it);
        return NULL;
    }
    it->shortcut = NULL;
    it->kind = kind;
    it->enabled = true;
    it->checked = false;
    it->on_activate = NULL;
    it->on_activate_user = NULL;
    it->submenu = NULL;
    menu->items[menu->count++] = it;
    return it;
}

fdk_result fdk_menu_append(fdk_menu *menu, const char *text,
                           fdk_menu_item **out_item) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_item *it = menu_new_item(menu, text, FDK_MIK_NORMAL);
    if (it == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    if (out_item != NULL) {
        *out_item = it;
    }
    return FDK_OK;
}

fdk_result fdk_menu_append_separator(fdk_menu *menu) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (menu_new_item(menu, NULL, FDK_MIK_SEPARATOR) == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    return FDK_OK;
}

fdk_result fdk_menu_append_check(fdk_menu *menu, const char *text,
                                 bool checked, fdk_menu_item **out_item) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_item *it = menu_new_item(menu, text, FDK_MIK_CHECK);
    if (it == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    it->checked = checked;
    if (out_item != NULL) {
        *out_item = it;
    }
    return FDK_OK;
}

fdk_result fdk_menu_append_radio(fdk_menu *menu, const char *text,
                                 bool checked, fdk_menu_item **out_item) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_item *it = menu_new_item(menu, text, FDK_MIK_RADIO);
    if (it == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    it->checked = checked;
    if (out_item != NULL) {
        *out_item = it;
    }
    return FDK_OK;
}

static fdk_menu_item *item_arg(fdk_menu_item *item) {
    if (item == NULL) {
        return NULL;
    }
    return item;
}

fdk_result fdk_menu_item_set_text(fdk_menu_item *item, const char *text) {
    item = item_arg(item);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = (text != NULL) ? fdk__strdup(text) : NULL;
    if (text != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(item->text);
    item->text = copy;
    return FDK_OK;
}

const char *fdk_menu_item_text(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item->text : NULL;
}

fdk_menu_item_type fdk_menu_item_get_type(fdk_menu_item *item) {
    if (item_arg(item) == NULL) {
        return FDK_MENU_ITEM_NORMAL;
    }
    switch (item->kind) {
    case FDK_MIK_SEPARATOR:
        return FDK_MENU_ITEM_SEPARATOR;
    case FDK_MIK_CHECK:
        return FDK_MENU_ITEM_CHECK;
    case FDK_MIK_RADIO:
        return FDK_MENU_ITEM_RADIO;
    default:
        return FDK_MENU_ITEM_NORMAL;
    }
}

void fdk_menu_item_set_enabled(fdk_menu_item *item, bool enabled) {
    if (item_arg(item) != NULL) {
        item->enabled = enabled;
    }
}

bool fdk_menu_item_is_enabled(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item->enabled : false;
}

void fdk_menu_item_set_checked(fdk_menu_item *item, bool checked) {
    item = item_arg(item);
    if (item == NULL) {
        return;
    }
    if (item->kind == FDK_MIK_RADIO && checked) {
        /* All radios of the same menu are one group: checking one
         * unchecks its radio siblings. Finding the owning menu from
         * an item pointer is not possible (items do not point back),
         * so the group semantics live in the activation path below,
         * which has the model at hand. For the DIRECT setter, a
         * radio can only be unchecked (leaving no selection) or
         * checked without knowing siblings — so the setter checks
         * the item itself and the activation path maintains the
         * group. This mirrors fdk_radio_set_checked's allowance. */
        item->checked = true;
    } else if (item->kind == FDK_MIK_CHECK || item->kind == FDK_MIK_RADIO) {
        item->checked = checked;
    }
}

bool fdk_menu_item_is_checked(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item->checked : false;
}

fdk_result fdk_menu_item_set_shortcut(fdk_menu_item *item,
                                      const char *shortcut) {
    item = item_arg(item);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = (shortcut != NULL) ? fdk__strdup(shortcut) : NULL;
    if (shortcut != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(item->shortcut);
    item->shortcut = copy;
    return FDK_OK;
}

fdk_result fdk_menu_item_set_submenu(fdk_menu_item *item,
                                     fdk_menu *submenu) {
    item = item_arg(item);
    if (item == NULL || item->kind != FDK_MIK_NORMAL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    item->submenu = submenu;
    return FDK_OK;
}

void fdk_menu_item_set_on_activate(fdk_menu_item *item,
                                   fdk_menu_activate_fn on_activate,
                                   void *user_data) {
    item = item_arg(item);
    if (item == NULL) {
        return;
    }
    item->on_activate = on_activate;
    item->on_activate_user = user_data;
}

void fdk_menu_set_on_activate(fdk_menu *menu,
                              fdk_menu_activate_fn on_activate,
                              void *user_data) {
    if (menu != NULL) {
        menu->on_activate = on_activate;
        menu->on_activate_user = user_data;
    }
}

/* ---- model geometry (shared by view + session) ---- */

/* Internal: a model's row height for normal rows. */
fdk_i32 fdk__menu_row_height(const fdk_menu *m) {
    fdk_i32 metric = fdk_theme_get_metric(NULL, FDK_TM_MENU_ITEM_HEIGHT);
    fdk_i32 fh = 0;
    if (m != NULL && m->font != NULL) {
        fdk_i32 fw = 0;
        fdk__text_extent(m->font, "Mg", &fw, &fh);
    }
    fdk_i32 h = fh + MENU_FONT_PAD;
    return (h > metric) ? h : metric;
}

/* Internal: which gutters the model's rows need. */
static void menu_gutters(const fdk_menu *m, bool *check_gutter,
                         bool *arrow_gutter) {
    *check_gutter = false;
    *arrow_gutter = false;
    if (m == NULL) {
        return;
    }
    for (size_t i = 0; i < m->count; i++) {
        if (m->items[i]->kind == FDK_MIK_CHECK ||
            m->items[i]->kind == FDK_MIK_RADIO) {
            *check_gutter = true;
        }
        if (m->items[i]->submenu != NULL) {
            *arrow_gutter = true;
        }
    }
}

/* Internal: a model's natural popup size (width, height) — the
 * session creates popup windows with this. min_width widens
 * (combo dropdowns match their combo). */
void fdk__menu_measure(const fdk_menu *m, fdk_i32 min_width,
                       fdk_i32 *out_w, fdk_i32 *out_h) {
    fdk_i32 w = MENU_MIN_W;
    fdk_i32 h = 0;
    bool cg = false, ag = false;
    menu_gutters(m, &cg, &ag);
    fdk_i32 rh = fdk__menu_row_height(m);
    if (m != NULL) {
        for (size_t i = 0; i < m->count; i++) {
            fdk_menu_item *it = m->items[i];
            if (it->kind == FDK_MIK_SEPARATOR) {
                h += MENU_SEP_H;
                continue;
            }
            h += rh;
            if (m->font == NULL || it->text == NULL) {
                continue;
            }
            fdk_i32 tw = 0, sw = 0;
            fdk__text_extent(m->font, it->text, &tw, NULL);
            if (it->shortcut != NULL) {
                fdk__text_extent(m->font, it->shortcut, &sw, NULL);
            }
            fdk_i32 need = MENU_PAD_X + tw + MENU_PAD_X / 2;
            if (sw > 0) {
                need += sw + MENU_PAD_X;
            }
            if (cg) {
                need += MENU_GUTTER;
            }
            if (ag) {
                need += MENU_ARROW_GUTTER;
            }
            need += MENU_PAD_X;
            if (need > w) {
                w = need;
            }
        }
    }
    if (w < min_width) {
        w = min_width;
    }
    if (h > MENU_MAX_H) {
        h = MENU_MAX_H;
    }
    if (h < 1) {
        h = 1;
    }
    *out_w = w;
    *out_h = h;
}

/* =====================================================================
 * The session (struct + slots live up here: the view handlers
 * reference session fields before the implementation section)
 * ===================================================================== */

#define FDK_MENU_MAX_CHAIN 8
#define FDK_MENU_MAX_SESSIONS 4

struct fdk_menu_session {
    bool active;
    uint64_t id;       /* monotonic slot identity: a callback may
                         recycle the slot (close_all + session_new);
                         captured ids detect that and skip the
                         now-meaningless close */
    fdk_widget *bar;   /* borrowed MenuBar; NULL for context/combo */
    int bar_index;     /* open bar title index, -1 when none       */
    /* Optional one-shot notification when this session's chain
     * fully ends (any path — activation, dismissal, popup death):
     * combo dropdowns free their temporary model here. Fired after
     * the session is fully cleared (the callback may open a fresh
     * dropdown on a new slot). */
    void (*closed)(void *user);
    void *closed_user;
    struct {
        fdk_window *popup; /* owned (destroyed by the session)     */
        fdk_widget *view;  /* borrowed; dies with the popup's tree */
        fdk_menu *model;   /* borrowed                             */
    } chain[FDK_MENU_MAX_CHAIN];
    int depth;
};

static fdk_menu_session g_sessions[FDK_MENU_MAX_SESSIONS];
static uint64_t g_session_next_id = 1;

/* =====================================================================
 * The view widget
 * ===================================================================== */

typedef struct fdk_menu_view {
    fdk_widget base;
    fdk_menu *model;   /* borrowed */
    fdk_menu_session *session; /* borrowed; NULL in headless tests */
    int level;         /* chain depth (0 = top)                   */
    int highlight;     /* pointer-hover row, -1 none              */
    int key_row;       /* keyboard cursor, -1 none                */
    int open_row;      /* row whose submenu is open, -1 none      */
} fdk_menu_view;

static fdk_menu_view *view_of(fdk_widget *w) {
    return (fdk_menu_view *)(void *)w;
}

/* ---- a11y ---- */
/* v1: menu items are PAINTED ROWS, not widgets — the a11y tree
 * exposes the menu itself (role MENU); per-item virtual nodes are
 * the documented Phase 10 follow-up (see roadmap). Item ACTIVATE
 * is still drivable through fdk_menu_item's own callbacks and the
 * session keyboard paths. */
static const fdk_a11y_class menu_view_a11y = {
    .role = FDK_A11Y_ROLE_MENU,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_menu_view_class_def = {
    .size = sizeof(fdk_menu_view),
    .name = "menu-view",
    .handle_event = fdk__menu_view_handle_event,
    .paint = fdk__menu_view_paint,
    .measure = fdk__menu_view_measure,
    .arrange = NULL,
    .destroy = NULL,
    .a11y = &menu_view_a11y,
};

void fdk__menu_view_measure(fdk_widget *w, fdk_size *out) {
    fdk_menu_view *v = view_of(w);
    fdk_i32 width = 0, height = 0;
    fdk__menu_measure(v->model, 0, &width, &height);
    out->width = width;
    out->height = height;
}

/* Test-support/internal: bind a model to a standalone view (the
 * session does this at popup creation; headless tests create views
 * directly and need the same binding). Refuses non-views. */
fdk_result fdk__menu_view_bind(fdk_widget *w, fdk_menu *model) {
    if (w == NULL || w->klass != &fdk_menu_view_class_def ||
        model == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_view *v = view_of(w);
    v->model = model;
    v->highlight = -1;
    v->key_row = -1;
    v->open_row = -1;
    fdk_widget_invalidate(w);
    return FDK_OK;
}

/* Internal: row index at widget-local y (-1 outside rows). */
int fdk__menu_row_at(fdk_widget *w, fdk_f32 y) {
    fdk_menu_view *v = view_of(w);
    if (v->model == NULL) {
        return -1;
    }
    fdk_i32 rh = fdk__menu_row_height(v->model);
    fdk_i32 yy = 0;
    for (size_t i = 0; i < v->model->count; i++) {
        fdk_i32 hh = (v->model->items[i]->kind == FDK_MIK_SEPARATOR)
                         ? MENU_SEP_H
                         : rh;
        if ((fdk_i32)y >= yy && (fdk_i32)y < yy + hh) {
            return (int)i;
        }
        yy += hh;
    }
    return -1;
}

/* Internal: row i's top edge. */
static fdk_i32 view_row_top(fdk_menu_view *v, int row) {
    fdk_i32 rh = fdk__menu_row_height(v->model);
    fdk_i32 yy = 0;
    for (int i = 0; i < row && (size_t)i < v->model->count; i++) {
        yy += (v->model->items[i]->kind == FDK_MIK_SEPARATOR) ? MENU_SEP_H
                                                              : rh;
    }
    return yy;
}

static void view_paint_check(fdk_surface *s, fdk_i32 cx, fdk_i32 cy,
                             fdk_color col) {
    fdk_surface_draw_line(s, cx - 4, cy, cx - 1, cy + 3, col);
    fdk_surface_draw_line(s, cx - 1, cy + 3, cx + 4, cy - 4, col);
}

static void view_paint_radio(fdk_surface *s, fdk_i32 cx, fdk_i32 cy,
                             fdk_color col) {
    /* Stroked diamond; a checked radio adds the inner cross so it
     * reads filled. Vector primitives only (font independence). */
    fdk_surface_draw_line(s, cx, cy - 4, cx + 4, cy, col);
    fdk_surface_draw_line(s, cx + 4, cy, cx, cy + 4, col);
    fdk_surface_draw_line(s, cx, cy + 4, cx - 4, cy, col);
    fdk_surface_draw_line(s, cx - 4, cy, cx, cy - 4, col);
}

static void view_paint_arrow(fdk_surface *s, fdk_i32 cx, fdk_i32 cy,
                             fdk_color col) {
    fdk_surface_draw_line(s, cx - 2, cy - 4, cx + 2, cy, col);
    fdk_surface_draw_line(s, cx + 2, cy, cx - 2, cy + 4, col);
}

void fdk__menu_view_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_menu_view *v = view_of(w);
    if (v->model == NULL || bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    bool cg = false, ag = false;
    menu_gutters(v->model, &cg, &ag);
    fdk_i32 rh = fdk__menu_row_height(v->model);

    /* Chrome: the menu surface + hairline border. */
    fdk_surface_fill_rect(surface, bounds, fdk__pal_control());
    fdk_color border = fdk__pal_border();
    fdk_surface_draw_rect(surface, bounds, border);

    fdk_i32 yy = bounds.y;
    for (size_t i = 0; i < v->model->count; i++) {
        fdk_menu_item *it = v->model->items[i];
        if (it->kind == FDK_MIK_SEPARATOR) {
            fdk_rect rule = {bounds.x + MENU_PAD_X / 2,
                             yy + MENU_SEP_H / 2,
                             bounds.width - MENU_PAD_X, 1};
            fdk_surface_fill_rect(surface, rule, border);
            yy += MENU_SEP_H;
            continue;
        }
        fdk_rect row = {bounds.x, yy, bounds.width, rh};
        bool lit = ((int)i == v->highlight || (int)i == v->key_row ||
                    (int)i == v->open_row) &&
                   it->enabled;
        if ((int)i == v->open_row && it->enabled) {
            /* The submenu-parent row stays lit while its child is
             * open — accent-tinted so it reads "active", not merely
             * hovered. */
            fdk_color accent = fdk__pal_accent();
            fdk_surface_fill_rect(
                surface, row,
                (fdk_color){accent.r, accent.g, accent.b, 0.45f});
        } else if (lit) {
            fdk_surface_fill_rect(surface, row, fdk__pal_control_hover());
        }

        fdk_i32 text_x = bounds.x + MENU_PAD_X;
        if (cg) {
            text_x += MENU_GUTTER;
            fdk_i32 cx = bounds.x + MENU_PAD_X + MENU_GUTTER / 2;
            fdk_i32 cy = yy + rh / 2;
            fdk_color col = it->enabled ? fdk__pal_accent()
                                        : fdk__pal_text_disabled();
            if (it->kind == FDK_MIK_CHECK && it->checked) {
                view_paint_check(surface, cx, cy, col);
            } else if (it->kind == FDK_MIK_RADIO) {
                view_paint_radio(surface, cx, cy, col);
                if (it->checked) {
                    view_paint_check(surface, cx, cy, col);
                }
            }
        }
        if (ag) {
            if (it->submenu != NULL) {
                fdk_color col = it->enabled ? fdk__pal_text()
                                            : fdk__pal_text_disabled();
                view_paint_arrow(surface,
                                 bounds.x + bounds.width -
                                     MENU_ARROW_GUTTER / 2 - MENU_PAD_X / 2,
                                 yy + rh / 2, col);
            }
        }
        if (v->model->font != NULL && it->text != NULL) {
            fdk_color col = it->enabled ? fdk__pal_text()
                                        : fdk__pal_text_disabled();
            fdk_i32 baseline =
                fdk__center_baseline(v->model->font, yy, rh);
            fdk__draw_text(surface, v->model->font, it->text, col,
                           text_x, baseline);
            if (it->shortcut != NULL) {
                fdk_i32 sw = 0;
                fdk__text_extent(v->model->font, it->shortcut, &sw, NULL);
                fdk_i32 sx = bounds.x + bounds.width - MENU_PAD_X - sw;
                if (ag) {
                    sx -= MENU_ARROW_GUTTER;
                }
                fdk__draw_text(surface, v->model->font, it->shortcut,
                               fdk__pal_text_disabled(), sx, baseline);
            }
        }
        yy += rh;
    }
}

/* ---- view-side session entry points (implemented below) ---- */

static void session_open_submenu(fdk_menu_session *s, fdk_menu_view *parent,
                                 int row);
static void session_close_above(fdk_menu_session *s, int level);
static void session_switch_bar(fdk_menu_session *s, int dir);
static void session_bar_hover(fdk_menu_session *s, fdk_i32 x, fdk_i32 y);

/* Fires (once) and clears the session's closed hook — every path
 * that flips a session inactive ends here. */
static void menu_session_fire_closed(fdk_menu_session *s) {
    void (*fn)(void *) = s->closed;
    void *user = s->closed_user;
    s->closed = NULL;
    s->closed_user = NULL;
    if (fn != NULL) {
        fn(user);
    }
}

/* Fires the item's callback: per-item, else the model fallback. */
static void menu_fire(fdk_menu *m, fdk_menu_item *it) {
    if (it->on_activate != NULL) {
        it->on_activate(it, it->on_activate_user);
    } else if (m->on_activate != NULL) {
        m->on_activate(it, m->on_activate_user);
    }
}

/* Activates a row: flips check/radio state, fires the app callback,
 * THEN closes the chain. The callback runs first because closed
 * hooks may free the MODEL (combo dropdowns own theirs) — menu_fire
 * must not touch freed items. The post-callback close is id-guarded:
 * the callback may itself have closed/recycled the session (a
 * callback that opens another menu, destroys the bar, or kills the
 * window). */
static void view_activate(fdk_menu_view *v, int row) {
    fdk_menu *m = v->model;
    fdk_menu_item *it = m->items[row];
    if (it->kind == FDK_MIK_CHECK) {
        it->checked = !it->checked;
    } else if (it->kind == FDK_MIK_RADIO && !it->checked) {
        for (size_t i = 0; i < m->count; i++) {
            if (m->items[i]->kind == FDK_MIK_RADIO) {
                m->items[i]->checked = ((int)i == row);
            }
        }
    }
    fdk_menu_session *s = v->session;
    uint64_t sid = (s != NULL) ? s->id : 0;
    menu_fire(m, it);
    if (s != NULL && s->active && s->id == sid) {
        v->session = NULL; /* the view dies with the chain */
        fdk__menu_session_close_all(s);
    }
}

static int view_next_row(fdk_menu_view *v, int from, int dir) {
    if (v->model == NULL || v->model->count == 0) {
        return -1;
    }
    int i = from;
    for (size_t guard = 0; guard < v->model->count + 1; guard++) {
        i += dir;
        if (i < 0) {
            i = (int)v->model->count - 1;
        }
        if ((size_t)i >= v->model->count) {
            i = 0;
        }
        fdk_menu_item *it = v->model->items[i];
        if (it->kind != FDK_MIK_SEPARATOR && it->enabled) {
            return i;
        }
        if (i == from) {
            break;
        }
    }
    return -1;
}

bool fdk__menu_view_handle_event(fdk_widget *w,
                                 const fdk_widget_event *ev) {
    fdk_menu_view *v = view_of(w);
    if (v->model == NULL) {
        return false;
    }
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION: {
        int row = fdk__menu_row_at(w, ev->position.y);
        if (row == v->highlight) {
            return true;
        }
        v->highlight = row;
        /* Hovering a different row closes any submenu this view
         * opened (the classic menu behavior). */
        if (v->session != NULL && v->open_row != -1 && row != v->open_row) {
            session_close_above(v->session, v->level + 1);
            v->open_row = -1;
        }
        fdk_widget_invalidate(w);
        if (row >= 0 && v->session != NULL) {
            fdk_menu_item *it = v->model->items[row];
            if (it->enabled && it->submenu != NULL &&
                it->submenu->count > 0) {
                session_open_submenu(v->session, v, row);
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_DOWN: {
        int row = fdk__menu_row_at(w, ev->pointer.position.y);
        if (row < 0) {
            return true; /* inside the menu, between rows: swallow */
        }
        fdk_menu_item *it = v->model->items[row];
        if (it->kind == FDK_MIK_SEPARATOR || !it->enabled) {
            return true; /* separators and disabled rows swallow */
        }
        if (it->submenu != NULL) {
            if (it->submenu->count == 0) {
                return true;
            }
            if (v->open_row == row) {
                return true; /* already open (hover did it) */
            }
            if (v->session != NULL) {
                session_open_submenu(v->session, v, row);
            }
            return true;
        }
        view_activate(v, row);
        return true;
    }
    case FDK_WIDGET_POINTER_UP:
        return true; /* consumed: no click-through to anything under */
    case FDK_WIDGET_POINTER_LEAVE:
        if (v->highlight != -1) {
            v->highlight = -1;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_KEY_DOWN: {
        fdk_scancode key = ev->key.scancode;
        if (key == FDK_KEY_UP || key == FDK_KEY_DOWN) {
            int from = (v->key_row != -1) ? v->key_row
                                          : ((key == FDK_KEY_DOWN) ? -1 : 0);
            int next = view_next_row(
                v, from, (key == FDK_KEY_DOWN) ? 1 : -1);
            v->key_row = next;
            if (v->highlight != -1 && v->highlight != next) {
                v->highlight = -1;
            }
            fdk_widget_invalidate(w);
            return true;
        }
        if (key == FDK_KEY_HOME || key == FDK_KEY_END) {
            int next = view_next_row(
                v, (key == FDK_KEY_HOME) ? (int)v->model->count : -1,
                (key == FDK_KEY_HOME) ? 1 : -1);
            v->key_row = next;
            fdk_widget_invalidate(w);
            return true;
        }
        if (key == FDK_KEY_ENTER || key == FDK_KEY_SPACE) {
            if (v->key_row >= 0) {
                fdk_menu_item *it = v->model->items[v->key_row];
                if (it->enabled && it->submenu != NULL &&
                    it->submenu->count > 0 && v->session != NULL) {
                    session_open_submenu(v->session, v, v->key_row);
                    return true;
                }
                if (it->enabled) {
                    view_activate(v, v->key_row);
                }
            }
            return true;
        }
        if (key == FDK_KEY_RIGHT) {
            if (v->key_row >= 0) {
                fdk_menu_item *it = v->model->items[v->key_row];
                if (it->submenu != NULL && it->submenu->count > 0 &&
                    v->session != NULL) {
                    session_open_submenu(v->session, v, v->key_row);
                    return true;
                }
            }
            if (v->level == 0 && v->session != NULL &&
                v->session->bar != NULL) {
                session_switch_bar(v->session, 1);
            }
            return true;
        }
        if (key == FDK_KEY_LEFT) {
            if (v->level > 0 && v->session != NULL) {
                session_close_above(v->session, v->level);
                return true;
            }
            if (v->level == 0 && v->session != NULL &&
                v->session->bar != NULL) {
                session_switch_bar(v->session, -1);
            }
            return true;
        }
        if (key == FDK_KEY_TAB) {
            return true; /* focus walk stays inside the menu chain */
        }
        return true; /* the menu grabbed the keyboard: it consumes */
    }
    default:
        return false;
    }
}

/* =====================================================================
 * The session (operations — the struct lives near the top)
 * ===================================================================== */

static fdk_menu_session *session_new(fdk_widget *bar) {
    fdk_menu_session *oldest = NULL;
    bool free_slot = false;
    for (int i = 0; i < FDK_MENU_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) {
            oldest = &g_sessions[i];
            free_slot = true;
            break;
        }
        if (oldest == NULL || g_sessions[i].depth < oldest->depth) {
            oldest = &g_sessions[i];
        }
    }
    if (!free_slot) {
        /* All slots busy: close the shallowest session (warned —
         * four simultaneous menu chains is already exotic). */
        FDK_WARN("menu: %d sessions active; closing one to fit another",
                 FDK_MENU_MAX_SESSIONS);
        fdk__menu_session_close_all(oldest);
    }
    oldest->active = true;
    oldest->id = g_session_next_id++;
    oldest->bar = bar;
    oldest->bar_index = -1;
    oldest->closed = NULL;
    oldest->closed_user = NULL;
    oldest->depth = 0;
    return oldest;
}

/* The popup windows' destroy-notify: a popup in our chain is being
 * destroyed by ANYONE (session teardown, parent-window sweep,
 * shutdown). Drop the reference; never destroy anything here. */
static void menu_popup_destroyed(fdk_window *window, void *user) {
    fdk_menu_session *s = user;
    if (!s->active) {
        return;
    }
    int at = -1;
    for (int i = 0; i < s->depth; i++) {
        if (s->chain[i].popup == window) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return;
    }
    /* Everything at and above this level is going away with it (the
     * window-layer sweep destroys popup children topmost-first). */
    for (int i = at; i < s->depth; i++) {
        s->chain[i].popup = NULL;
        s->chain[i].view = NULL;
        s->chain[i].model = NULL;
    }
    s->depth = at;
    if (at == 0) {
        /* The whole chain is gone: end the session (unhighlight the
         * bar if we still can) and fire the one-shot closed hook. */
        fdk_widget *bar = s->bar;
        if (bar != NULL) {
            fdk__menu_bar_session_ended(bar, s);
        }
        s->bar = NULL;
        s->bar_index = -1;
        s->active = false;
        menu_session_fire_closed(s);
    }
}

/* The popup windows' event callback: CLOSE_REQUEST dismisses this
 * popup and everything above it (Escape in a submenu, a click
 * outside under a grab, the compositor's popup_done). Out-of-bounds
 * MOTION (X11 grabs report everything against the grab window)
 * keeps bar hover-switching alive: motion above the level-0 popup
 * is over the bar itself. */
static void menu_popup_event(fdk_window *window, const fdk_event_data *ev,
                             void *user) {
    fdk_menu_session *s = user;
    if (!s->active) {
        return;
    }
    int at = -1;
    for (int i = 0; i < s->depth; i++) {
        if (s->chain[i].popup == window) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return;
    }
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        session_close_above(s, at);
        return;
    }
    if (ev->type == FDK_EVENT_POINTER_MOTION && at == 0 &&
        s->bar != NULL) {
        /* Popup-local coordinates; y < 0 = over the bar (the popup
         * hangs directly below it). */
        session_bar_hover(s, (fdk_i32)ev->pointer.position.x,
                          (fdk_i32)ev->pointer.position.y);
    }
    if (ev->type == FDK_EVENT_POINTER_MOTION && at > 0) {
        /* Hover left a submenu back onto its parent menu: the motion
         * reports against the (still-grabbed) submenu window with
         * out-of-bounds coordinates. Closing ourselves returns the
         * parent to normal hover. */
        if (ev->pointer.position.x < 0.0f &&
            ev->pointer.position.y >= 0.0f) {
            session_close_above(s, at);
        }
    }
}

/* Internal (menu_internal.h): closes a session's whole chain. Called
 * from the activation path (before the app's callback runs) and from
 * slot reuse. */
void fdk__menu_session_close_all(fdk_menu_session *s) {
    if (s == NULL || !s->active) {
        return;
    }
    session_close_above(s, 0);
}

static void session_close_above(fdk_menu_session *s, int level) {
    if (!s->active || level < 0) {
        return;
    }
    /* Destroy topmost-first (the Wayland xdg_popup rule: only the
     * topmost popup may be destroyed; X11 children die with parents
     * anyway — this order satisfies both). */
    for (int i = s->depth - 1; i >= level; i--) {
        fdk_window *pop = s->chain[i].popup;
        s->chain[i].popup = NULL;
        s->chain[i].view = NULL;
        s->chain[i].model = NULL;
        if (pop != NULL) {
            fdk_window_destroy(pop); /* destroy-notify sees NULLs */
        }
    }
    if ((int)s->depth > level) {
        s->depth = level;
    }
    if (level == 0) {
        if (s->bar != NULL) {
            fdk__menu_bar_session_ended(s->bar, s);
        }
        s->bar = NULL;
        s->bar_index = -1;
        s->active = false;
        menu_session_fire_closed(s);
        return;
    }
    /* The new topmost popup regains input: X11 needs an explicit
     * re-grab (grabs do not stack); Wayland returned it already. */
    if (s->depth > 0 && s->chain[s->depth - 1].popup != NULL) {
        fdk__window_regrab(s->chain[s->depth - 1].popup);
    }
    fdk_widget *view = (s->depth > 0) ? s->chain[s->depth - 1].view : NULL;
    if (view != NULL) {
        fdk_widget_focus(view);
        fdk_widget_invalidate(view);
    }
    /* Parent view's open_row must clear. */
    if (s->depth > 0 && s->chain[s->depth - 1].view != NULL) {
        fdk_menu_view *pv =
            (fdk_menu_view *)(void *)s->chain[s->depth - 1].view;
        if (pv->open_row != -1) {
            pv->open_row = -1;
            fdk_widget_invalidate(&pv->base);
        }
    }
}

/* Opens `model` as level `level` of the session, parented to
 * `parent_win` at its client-relative (x, y). */
static fdk_result session_open_level(fdk_menu_session *s, fdk_menu *model,
                                     fdk_window *parent_win, fdk_i32 x,
                                     fdk_i32 y, fdk_i32 min_width,
                                     int level) {
    if (model == NULL || model->count == 0 || parent_win == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (level >= FDK_MENU_MAX_CHAIN) {
        FDK_WARN("menu: chain depth cap (%d) reached", level);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_i32 w = 0, h = 0;
    fdk__menu_measure(model, min_width, &w, &h);

    fdk_window *pop = NULL;
    fdk_result r =
        fdk_window_create_popup(parent_win->ctx, parent_win, x, y, w, h,
                                &pop);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk__window_set_auto_paint(pop, true);
    fdk__window_set_destroy_notify(pop, menu_popup_destroyed, s);
    fdk_window_set_event_callback(pop, menu_popup_event, s);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(pop, &root);
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        return r;
    }
    fdk_widget *view_w = NULL;
    r = fdk_widget_create(root, &fdk_menu_view_class_def,
                          (fdk_rect){0, 0, w, h}, &view_w);
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        return r;
    }
    fdk_menu_view *v = view_of(view_w);
    v->model = model;
    v->session = s;
    v->level = level;
    v->highlight = -1;
    v->key_row = -1;
    v->open_row = -1;

    s->chain[level].popup = pop;
    s->chain[level].view = view_w;
    s->chain[level].model = model;
    if (level + 1 > s->depth) {
        s->depth = level + 1;
    }
    fdk_widget_set_can_focus(view_w, true);
    fdk_window_show(pop);
    fdk_widget_focus(view_w);
    return FDK_OK;
}

static void session_open_submenu(fdk_menu_session *s, fdk_menu_view *parent,
                                 int row) {
    fdk_menu_item *it = parent->model->items[row];
    if (it->submenu == NULL || it->submenu->count == 0) {
        return;
    }
    /* Close anything already open at deeper levels, then open. */
    if (s->depth > parent->level + 1) {
        session_close_above(s, parent->level + 1);
    }
    fdk_window *pw = s->chain[parent->level].popup;
    fdk_i32 w = 0, h = 0;
    fdk__menu_measure(it->submenu, 0, &w, &h);
    fdk_i32 row_top = view_row_top(parent, row);
    fdk_i32 rh = fdk__menu_row_height(parent->model);
    fdk_result r = session_open_level(
        s, it->submenu, pw, w - 6, row_top - 2, 0, parent->level + 1);
    if (!fdk_ok(r)) {
        return;
    }
    parent->open_row = row;
    fdk_widget_invalidate(&parent->base);
    /* Start the child's keyboard cursor on its first enabled row so
     * Right-then-Enter flows naturally. */
    fdk_menu_view *child =
        (fdk_menu_view *)(void *)s->chain[parent->level + 1].view;
    if (child != NULL) {
        child->key_row =
            view_next_row(child, -1, 1);
        (void)rh;
    }
}

static void session_switch_bar(fdk_menu_session *s, int dir) {
    if (s->bar == NULL) {
        return;
    }
    size_t n = fdk__menu_bar_count(s->bar);
    if (n == 0) {
        return;
    }
    /* Close the current chain but KEEP the session alive for the
     * switch. */
    int old = s->bar_index;
    for (int i = s->depth - 1; i >= 0; i--) {
        fdk_window *pop = s->chain[i].popup;
        s->chain[i].popup = NULL;
        s->chain[i].view = NULL;
        s->chain[i].model = NULL;
        if (pop != NULL) {
            fdk_window_destroy(pop);
        }
    }
    s->depth = 0;
    if (old < 0) {
        old = 0;
    }
    int next = (old + (int)dir) % (int)n;
    if (next < 0) {
        next += (int)n;
    }
    if (!fdk__menu_bar_open_index(s->bar, s, next)) {
        /* Open failed: end the session cleanly. */
        session_close_above(s, 0);
    }
}

/* Bar hover-switching from out-of-bounds motion under the X11 grab:
 * (mx, my) are popup-local; my < 0 means the pointer is above the
 * popup — i.e. over the bar. */
static void session_bar_hover(fdk_menu_session *s, fdk_i32 mx, fdk_i32 my) {
    if (s->bar == NULL || my >= 0) {
        return;
    }
    /* Popup-local (mx, my) → bar-local: the popup's top-left sits at
     * the open title's bottom-left. fdk__menu_bar_hit maps window
     * coordinates to a title index; the bar tracks that anchor. */
    fdk_i32 bx = 0, by = 0;
    if (!fdk__menu_bar_popup_anchor(s->bar, &bx, &by)) {
        return;
    }
    fdk_i32 wx = bx + mx;
    fdk_i32 wy = by + my;
    int hit = fdk__menu_bar_hit(s->bar, wx, wy);
    if (hit >= 0 && hit != s->bar_index) {
        session_switch_bar(s, (hit > s->bar_index) ? 1 : -1);
    }
}

/* ---- public context-menu entry point ---- */

fdk_result fdk__menu_popup_open_full(fdk_menu *menu, fdk_widget *anchor,
                                     fdk_i32 x, fdk_i32 y, fdk_i32 min_width,
                                     void (*on_closed)(void *),
                                     void *closed_user) {
    if (menu == NULL || anchor == NULL || menu->count == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_window *win =
        fdk__window_of_owner(fdk__widget_window_owner(anchor));
    if (win == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* standalone tree: no window */
    }
    /* Window-relative anchor: the widget's absolute bounds + (x, y). */
    fdk_rect abs = fdk_widget_get_absolute_bounds(anchor);
    fdk_menu_session *s = session_new(NULL);
    s->closed = on_closed;
    s->closed_user = closed_user;
    return session_open_level(s, menu, win, abs.x + x, abs.y + y,
                              min_width, 0);
}

fdk_result fdk__menu_popup_open(fdk_menu *menu, fdk_widget *anchor,
                                fdk_i32 x, fdk_i32 y, fdk_i32 min_width) {
    return fdk__menu_popup_open_full(menu, anchor, x, y, min_width, NULL,
                                     NULL);
}

fdk_result fdk_menu_popup_at(fdk_menu *menu, fdk_widget *anchor,
                             fdk_i32 x, fdk_i32 y) {
    return fdk__menu_popup_open(menu, anchor, x, y, 0);
}

/* =====================================================================
 * The MenuBar widget
 * ===================================================================== */

typedef struct fdk_menu_bar_title {
    char *title;    /* owned */
    fdk_menu *menu; /* borrowed */
    fdk_rect rect;  /* bar-local layout slot */
} fdk_menu_bar_title;

typedef struct fdk_menu_bar {
    fdk_widget base;
    fdk_font *font; /* borrowed */
    fdk_menu_bar_title *titles;
    size_t count;
    size_t cap;
    int hover;        /* -1 none */
    int open_index;   /* -1 none; mirrors the session while open */
    int key_cursor;   /* keyboard title cursor, -1 none */
    fdk_menu_session *session; /* borrowed slot while a chain is open */
    fdk_i32 anchor_x; /* window x of the open popup's top-left   */
    fdk_i32 anchor_y; /* window y of the open popup's top-left   */
} fdk_menu_bar;

static fdk_menu_bar *bar_of(fdk_widget *w) {
    return (fdk_menu_bar *)(void *)w;
}

extern const fdk_widget_class fdk_menu_bar_class_def;

static void bar_layout(fdk_widget *w) {
    fdk_menu_bar *b = bar_of(w);
    fdk_i32 x = BAR_LEFT;
    for (size_t i = 0; i < b->count; i++) {
        fdk_i32 tw = 0;
        if (b->font != NULL && b->titles[i].title != NULL) {
            fdk__text_extent(b->font, b->titles[i].title, &tw, NULL);
        }
        if (tw < 8) {
            tw = 8;
        }
        b->titles[i].rect =
            (fdk_rect){x, 0, tw + BAR_PAD_X * 2, w->bounds.height};
        x += b->titles[i].rect.width;
    }
}

static void bar_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = 32;
    out->height = fdk__menu_row_height(NULL);
}

static void bar_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    bar_layout(w);
}

static void bar_paint(fdk_widget *w, fdk_surface *surface, fdk_rect bounds,
                      fdk_rect clip) {
    (void)clip;
    fdk_menu_bar *b = bar_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_surface_fill_rect(surface, bounds, fdk__pal_track());
    fdk_rect rule = {bounds.x, bounds.y + bounds.height - 1,
                     bounds.width, 1};
    fdk_surface_fill_rect(surface, rule, fdk__pal_border());
    if (b->font == NULL) {
        return;
    }
    for (size_t i = 0; i < b->count; i++) {
        fdk_rect r = b->titles[i].rect;
        r.x += bounds.x;
        r.y = bounds.y;
        bool lit = ((int)i == b->hover || (int)i == b->open_index ||
                    (int)i == b->key_cursor);
        if (lit) {
            fdk_surface_fill_rect(surface, r, fdk__pal_control_hover());
        }
        fdk_i32 tw = 0;
        fdk__text_extent(b->font, b->titles[i].title, &tw, NULL);
        fdk_i32 baseline =
            fdk__center_baseline(b->font, bounds.y, bounds.height);
        fdk__draw_text(surface, b->font, b->titles[i].title,
                       fdk__pal_text(), r.x + (r.width - tw) / 2, baseline);
    }
}

static void bar_destroy(fdk_widget *w) {
    fdk_menu_bar *b = bar_of(w);
    /* An open chain dies with the bar (popups are owned by the
     * session slot, which the destroy-notify unwinds). */
    if (b->session != NULL && b->session->active) {
        fdk_menu_session *s = b->session;
        b->session = NULL;
        session_close_above(s, 0);
    }
    for (size_t i = 0; i < b->count; i++) {
        fdk_free(b->titles[i].title);
    }
    fdk_free(b->titles);
    b->titles = NULL;
    b->count = 0;
}

static const fdk_a11y_class menu_bar_a11y = {
    .role = FDK_A11Y_ROLE_MENU_BAR,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_menu_bar_class_def = {
    .size = sizeof(fdk_menu_bar),
    .name = "menu-bar",
    .handle_event = fdk__menu_bar_handle_event,
    .paint = bar_paint,
    .measure = bar_measure,
    .arrange = bar_arrange,
    .destroy = bar_destroy,
    .a11y = &menu_bar_a11y,
};

/* Internal helpers shared with the session (menu_internal.h). */

size_t fdk__menu_bar_count(fdk_widget *w) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return 0;
    }
    return bar_of(w)->count;
}

/* Opens bar title `index` under session `s` (switching keeps the
 * session alive across a close). Returns success. */
bool fdk__menu_bar_open_index(fdk_widget *w, fdk_menu_session *s,
                              int index) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return false;
    }
    fdk_menu_bar *b = bar_of(w);
    if (index < 0 || (size_t)index >= b->count) {
        return false;
    }
    fdk_menu *m = b->titles[index].menu;
    if (m == NULL || m->count == 0) {
        return false;
    }
    fdk_window *win = fdk__window_of_owner(fdk__widget_window_owner(w));
    if (win == NULL) {
        return false; /* standalone tree: nothing to anchor to */
    }
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_rect tr = b->titles[index].rect;
    fdk_i32 x = abs.x + tr.x;
    fdk_i32 y = abs.y + w->bounds.height;
    fdk_result r = session_open_level(s, m, win, x, y, 0, 0);
    if (!fdk_ok(r)) {
        return false;
    }
    b->open_index = index;
    b->session = s;
    s->bar = w;
    s->bar_index = index;
    /* Remember the popup's anchor for bar hover-switching. */
    b->anchor_x = x;
    b->anchor_y = y;
    fdk_widget_invalidate(w);
    return true;
}

/* The session's chain fully closed (or switched away): unhighlight. */
void fdk__menu_bar_session_ended(fdk_widget *w, fdk_menu_session *s) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return;
    }
    fdk_menu_bar *b = bar_of(w);
    if (b->session == s) {
        b->session = NULL;
        b->open_index = -1;
        fdk_widget_invalidate(w);
    }
}

/* Window coordinates of the open popup's top-left (the hover-switch
 * mapping). False when no chain is open. */
bool fdk__menu_bar_popup_anchor(fdk_widget *w, fdk_i32 *out_x,
                                fdk_i32 *out_y) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return false;
    }
    fdk_menu_bar *b = bar_of(w);
    if (b->session == NULL || !b->session->active) {
        return false;
    }
    *out_x = b->anchor_x;
    *out_y = b->anchor_y;
    return true;
}

/* Window-coordinate hit test over titles. */
int fdk__menu_bar_hit(fdk_widget *w, fdk_i32 x, fdk_i32 y) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return -1;
    }
    fdk_menu_bar *b = bar_of(w);
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_i32 lx = x - abs.x;
    fdk_i32 ly = y - abs.y;
    if (ly < 0 || ly >= w->bounds.height) {
        return -1;
    }
    for (size_t i = 0; i < b->count; i++) {
        fdk_rect r = b->titles[i].rect;
        if (lx >= r.x && lx < r.x + r.width) {
            return (int)i;
        }
    }
    return -1;
}

bool fdk__menu_bar_handle_event(fdk_widget *w,
                                const fdk_widget_event *ev) {
    fdk_menu_bar *b = bar_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION: {
        /* Motion positions are BAR-LOCAL; map to window coords for
         * the shared hit helper. */
        fdk_rect abs = fdk_widget_get_absolute_bounds(w);
        int hit = fdk__menu_bar_hit(w, abs.x + (fdk_i32)ev->position.x,
                                    abs.y + (fdk_i32)ev->position.y);
        if (hit != b->hover) {
            b->hover = hit;
            fdk_widget_invalidate(w);
        }
        return true;
    }
    case FDK_WIDGET_POINTER_DOWN: {
        fdk_rect abs = fdk_widget_get_absolute_bounds(w);
        int hit = fdk__menu_bar_hit(w, abs.x + (fdk_i32)ev->pointer.position.x,
                                    abs.y + (fdk_i32)ev->pointer.position.y);
        if (hit < 0) {
            return true; /* press on the bar's padding: swallow */
        }
        if (b->session != NULL && b->session->active &&
            b->open_index == hit) {
            /* Clicking the open title closes it (toggle). */
            fdk_menu_session *s = b->session;
            session_close_above(s, 0);
        } else if (b->session != NULL && b->session->active) {
            /* Another title while a chain is open: switch. */
            session_switch_bar(b->session,
                               (hit > b->open_index) ? 1 : -1);
        } else {
            fdk_menu_session *s = session_new(w);
            if (!fdk__menu_bar_open_index(w, s, hit)) {
                /* Nothing to open: end the fresh session. */
                s->active = false;
                s->bar = NULL;
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_LEAVE:
        if (b->hover != -1) {
            b->hover = -1;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_KEY_DOWN: {
        fdk_scancode key = ev->key.scancode;
        if (key == FDK_KEY_LEFT || key == FDK_KEY_RIGHT) {
            int n = (int)b->count;
            if (n == 0) {
                return true;
            }
            int cur = (b->key_cursor != -1) ? b->key_cursor : 0;
            cur += (key == FDK_KEY_RIGHT) ? 1 : -1;
            if (cur < 0) {
                cur = n - 1;
            }
            if (cur >= n) {
                cur = 0;
            }
            b->key_cursor = cur;
            fdk_widget_invalidate(w);
            return true;
        }
        if (key == FDK_KEY_DOWN || key == FDK_KEY_ENTER ||
            key == FDK_KEY_SPACE) {
            if (b->key_cursor >= 0 && b->session == NULL) {
                fdk_menu_session *s = session_new(w);
                if (!fdk__menu_bar_open_index(w, s, b->key_cursor)) {
                    s->active = false;
                    s->bar = NULL;
                }
            }
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

/* ---- MenuBar public API ---- */

fdk_result fdk_menu_bar_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_bar) {
    if (out_bar == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_menu_bar_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_menu_bar *b = bar_of(w);
    b->font = font;
    b->titles = NULL;
    b->count = 0;
    b->cap = 0;
    b->hover = -1;
    b->open_index = -1;
    b->key_cursor = -1;
    b->session = NULL;
    b->anchor_x = 0;
    b->anchor_y = 0;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_bar = w;
    return FDK_OK;
}

size_t fdk_menu_bar_count(fdk_widget *bar) {
    return fdk__menu_bar_count(bar);
}

fdk_result fdk_menu_bar_append(fdk_widget *bar, const char *title,
                               fdk_menu *menu) {
    if (bar == NULL || bar->klass != &fdk_menu_bar_class_def ||
        title == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_bar *b = bar_of(bar);
    if (b->count == b->cap) {
        size_t ncap = b->cap * 2 + 4;
        fdk_menu_bar_title *nt =
            fdk_realloc(b->titles, ncap * sizeof(fdk_menu_bar_title));
        if (nt == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        b->titles = nt;
        b->cap = ncap;
    }
    char *copy = fdk__strdup(title);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    b->titles[b->count].title = copy;
    b->titles[b->count].menu = menu;
    b->titles[b->count].rect = (fdk_rect){0, 0, 0, 0};
    b->count++;
    bar_layout(bar);
    fdk_widget_invalidate(bar);
    return FDK_OK;
}

fdk_result fdk_menu_bar_remove(fdk_widget *bar, size_t index) {
    if (bar == NULL || bar->klass != &fdk_menu_bar_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_bar *b = bar_of(bar);
    if (index >= b->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Removing the OPEN title closes the chain first. */
    if (b->session != NULL && b->session->active &&
        (int)index == b->open_index) {
        session_close_above(b->session, 0);
    }
    fdk_free(b->titles[index].title);
    memmove(&b->titles[index], &b->titles[index + 1],
            (b->count - index - 1) * sizeof(fdk_menu_bar_title));
    b->count--;
    if (b->key_cursor >= (int)b->count) {
        b->key_cursor = (int)b->count - 1;
    }
    bar_layout(bar);
    fdk_widget_invalidate(bar);
    return FDK_OK;
}

void fdk_menu_bar_close(fdk_widget *bar) {
    if (bar == NULL || bar->klass != &fdk_menu_bar_class_def) {
        return;
    }
    fdk_menu_bar *b = bar_of(bar);
    if (b->session != NULL && b->session->active) {
        session_close_above(b->session, 0);
    }
}
