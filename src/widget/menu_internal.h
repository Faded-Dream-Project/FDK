/*
 * menu_internal.h — shared internals of the Phase 9 menu machinery
 * (menu.c) used by its siblings (combo.c).
 *
 * Not part of the public API — never installed. The public contract
 * lives in include/fdk/fdk_widgets.h.
 */

#ifndef FDK_MENU_INTERNAL_H
#define FDK_MENU_INTERNAL_H

#include "fdk/fdk_widgets.h"

#include "widget_internal.h"

/* ---- class defs (menu.c) ---- */

extern const fdk_widget_class fdk_menu_view_class_def;
extern const fdk_widget_class fdk_menu_bar_class_def;

/* ---- view hooks (menu.c; referenced by the class defs) ---- */

bool fdk__menu_view_handle_event(fdk_widget *w,
                                 const fdk_widget_event *ev);
void fdk__menu_view_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip);
void fdk__menu_view_measure(fdk_widget *w, fdk_size *out);
bool fdk__menu_bar_handle_event(fdk_widget *w,
                                const fdk_widget_event *ev);

/* ---- model geometry (menu.c) ---- */

/* A model's normal-row height: max(themed menu_item_height metric,
 * font line extent + 8). font may be NULL (metric then wins). */
fdk_i32 fdk__menu_row_height(const fdk_menu *menu);

/* A model's natural popup size; min_width widens (combo dropdowns
 * match their combo's width). */
void fdk__menu_measure(const fdk_menu *menu, fdk_i32 min_width,
                       fdk_i32 *out_w, fdk_i32 *out_h);

/* Row index at view-local y (-1 outside the rows). */
int fdk__menu_row_at(fdk_widget *view, fdk_f32 y);

/* Binds a model to a standalone view (headless-test entry; the
 * session binds at popup creation internally). */
fdk_result fdk__menu_view_bind(fdk_widget *view, fdk_menu *model);

/* ---- session entry points (menu.c) ----
 *
 * Opaque session type: combo.c only passes it around. */

typedef struct fdk_menu_session fdk_menu_session;

/* Opens `model` as a standalone popup chain (context menus, combo
 * dropdowns) anchored at widget-relative (x, y) of `anchor`'s
 * top-left. Same contract as the public fdk_menu_popup_at, plus the
 * min_width hook combo.c needs. */
fdk_result fdk__menu_popup_open(fdk_menu *model, fdk_widget *anchor,
                                fdk_i32 x, fdk_i32 y, fdk_i32 min_width);

/* Same, plus a one-shot notification fired when the chain fully
 * ends (activation, dismissal, or popup death by any destroy path).
 * The combo frees its temporary dropdown model here. The callback
 * runs after the session is fully cleared and may open a new popup
 * (a fresh session slot). */
fdk_result fdk__menu_popup_open_full(fdk_menu *model, fdk_widget *anchor,
                                     fdk_i32 x, fdk_i32 y,
                                     fdk_i32 min_width,
                                     void (*on_closed)(void *),
                                     void *closed_user);

/* Closes a session's whole chain (activation path). Safe on any
 * session, active or not. */
void fdk__menu_session_close_all(fdk_menu_session *session);

/* ---- bar helpers used by the session (menu.c) ---- */

size_t fdk__menu_bar_count(fdk_widget *bar);
bool fdk__menu_bar_open_index(fdk_widget *bar, fdk_menu_session *session,
                              int index);
void fdk__menu_bar_session_ended(fdk_widget *bar,
                                 fdk_menu_session *session);
bool fdk__menu_bar_popup_anchor(fdk_widget *bar, fdk_i32 *out_x,
                                fdk_i32 *out_y);
int fdk__menu_bar_hit(fdk_widget *bar, fdk_i32 x, fdk_i32 y);

#endif /* FDK_MENU_INTERNAL_H */
