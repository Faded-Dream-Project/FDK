/*
 * widgets_internal.h — shared internals of the Phase 6 widget
 * catalog (Label/Button/Toggle/Checkbox/Radio/ProgressBar/Separator/
 * Frame).
 *
 * Not part of the public API — never installed. The public contract
 * lives in include/fdk/fdk_widgets.h.
 *
 * Layout note: these implementation files live in src/widget/ but
 * the Frame needs the box packing, so layout_internal.h is included
 * from here (layout already depends on widget; this is the one
 * sanctioned back-edge, kept inside the toolkit's own source).
 */

#ifndef FDK_WIDGETS_INTERNAL_H
#define FDK_WIDGETS_INTERNAL_H

#include "fdk/fdk_a11y.h"
#include "fdk/fdk_widgets.h"

#include "widget_internal.h"
#include "../layout/layout_internal.h"

#include <string.h>

/* ---- class defs (defined in controls.c / statics.c) ---- */

extern const fdk_widget_class fdk_label_class_def;
extern const fdk_widget_class fdk_button_class_def;
extern const fdk_widget_class fdk_toggle_class_def;
extern const fdk_widget_class fdk_checkbox_class_def;
extern const fdk_widget_class fdk_radio_class_def;
extern const fdk_widget_class fdk_progress_class_def;
extern const fdk_widget_class fdk_separator_class_def;
extern const fdk_widget_class fdk_frame_class_def;
extern const fdk_widget_class fdk_scrollview_class_def;
extern const fdk_widget_class fdk_toolbar_class_def;

/* toolbar.c: relayout hook for the layout notifier (box.c). */
void fdk__toolbar_layout_changed(fdk_widget *w);

/* scroll.c: relayout hook the layout notifier (box.c) calls when a
 * scrollview's subtree changed (content added / natural size
 * changed) — re-runs the internal arrangement at current bounds. */
void fdk__scrollview_layout_changed(fdk_widget *w);

/* scroll.c: the scroll area's viewport size (bounds minus visible
 * bars) for keyboard page-stepping in ScrollView-based widgets. */
void fdk__scrollview_viewport(fdk_widget *w, fdk_i32 *out_w,
                              fdk_i32 *out_h);

/* ---- shared helpers ---- */

/* fdk_alloc'd copy of s (NULL -> NULL). The catalog's owned text. */
char *fdk__strdup(const char *s);

/* widget.c: the window-root class (base widget + WINDOW a11y role) —
 * window.c creates window-owned roots with it. */
const fdk_widget_class *fdk__widget_window_root_class(void);

/* a11y core (src/widget/a11y.c): fire a change notification for a
 * widget mutation (children/state/name/bounds/value). Catalog
 * setters call this AFTER the mutation is fully applied. Safe from
 * inside event callbacks (snapshot walk). */
void fdk__a11y_notify(fdk_widget *widget, fdk_a11y_event_kind kind,
                      fdk_a11y_state_flag state_flag);
/* printf-render an owned value-text string (NULL on failure). */
char *fdk__a11y_valuef(const char *fmt, double v);

/* a11y (widget.c teardown): drop every relation edge that touched a
 * destroyed widget — its own list AND the inverse copies stored on
 * the targets — so dangling relation targets cannot exist. */
void fdk__a11y_relations_destroyed(fdk_widget *widget);

/* The text's advance width and line extent (ascent + descent) in px,
 * or {0,0} when font or text is missing. Line extent (not ink) is
 * what layout wants: two labels with different glyphs align. */
void fdk__text_extent(const fdk_font *font, const char *text,
                      fdk_i32 *out_w, fdk_i32 *out_h);

/* Draws `text` at absolute (x, baseline) if font and text exist;
 * no-op otherwise. (The paint hooks receive absolute coordinates.) */
void fdk__draw_text(fdk_surface *surface, fdk_font *font,
                    const char *text, fdk_color color, fdk_i32 x,
                    fdk_i32 baseline);

/* Baseline y for vertically centering a text line of height text_h
 * within [top, top + avail_h). */
fdk_i32 fdk__center_baseline(const fdk_font *font, fdk_i32 top,
                             fdk_i32 avail_h);

/* ---- themed palette accessors (Phase 7) ----
 *
 * Resolved against the current default theme at PAINT time (see
 * statics.c) - fdk_theme_set_default() needs no cache flush. */


fdk_color fdk__pal_text(void);
fdk_color fdk__pal_text_disabled(void);
fdk_color fdk__pal_control(void);
fdk_color fdk__pal_control_hover(void);
fdk_color fdk__pal_control_pressed(void);
fdk_color fdk__pal_control_disabled(void);
fdk_color fdk__pal_accent(void);
fdk_color fdk__pal_track(void);
fdk_color fdk__pal_border(void);

/* ---- shared instance structs ---- */

/* Label. `lines` is the display cache: the text broken into the
 * lines that fit `built_width` (NOWRAP: one full line; WRAP: the
 * greedy word-wrap; ELLIPSIZE: one line capped by the ellipsis
 * pass). Rebuilt on arrange (width changed) and lazily at paint when
 * dirty — see statics.c. */
typedef struct fdk_label {
    fdk_widget base;
    fdk_font *font;    /* borrowed */
    char *text;        /* owned, may be NULL */
    fdk_color color;   /* text color */
    fdk_label_mode mode;    /* NOWRAP / WRAP / ELLIPSIZE        */
    fdk_align align;        /* horizontal, FILL treated as START */
    fdk_text_line *lines;   /* owned cache, may be NULL           */
    size_t line_count;
    size_t lines_cap;
    fdk_i32 built_width;    /* width the cache was built for      */
    bool lines_dirty;       /* text/mode changed since last build */
    size_t ellipsis_prefix; /* ELLIPSIZE mode: fitting prefix bytes */
    fdk_i32 ellipsis_x;     /* ELLIPSIZE mode: prefix advance (pen) */
    fdk_i32 ellipsis_w;     /* ELLIPSIZE mode: the "..." run's advance */
    bool ellipsized;        /* ELLIPSIZE mode: text did not fit   */
} fdk_label;

/* Button. */
typedef struct fdk_button {
    fdk_widget base;
    fdk_font *font;    /* borrowed */
    char *text;        /* owned, may be NULL */
    fdk_button_activate_fn on_activate;
    void *on_activate_data;
    bool pressed;      /* pointer down inside */
    bool hovering;
} fdk_button;

/* Shared shape of Toggle / Checkbox / Radio: an indicator box/circle/
 * track of a fixed extent, a gap, then optional text. */
typedef struct fdk_check_widget {
    fdk_widget base;
    fdk_font *font;    /* borrowed */
    char *text;        /* owned, may be NULL */
    bool checked;
    bool pressed;      /* visual state only */
    bool hovering;
    void (*on_change)(fdk_widget *w, bool checked, void *user);
    void *on_change_data;
} fdk_check_widget;

/* ProgressBar. */
typedef struct fdk_progress {
    fdk_widget base;
    fdk_f32 fraction; /* [0,1] */
} fdk_progress;

/* Separator. */
typedef struct fdk_separator {
    fdk_widget base;
    fdk_orientation orientation;
} fdk_separator;

/* Frame: a vertical box with a title band drawn above the children
 * (box->title_inset reserves the space; the packing code accounts
 * for it). */
typedef struct fdk_frame {
    fdk_box base;      /* embeds fdk_widget */
    fdk_font *font;    /* borrowed */
    char *title;       /* owned, may be NULL */
} fdk_frame;

/* Downcasts — single-allocation subclasses, base first (see
 * fdk_widget.h's subclassing contract). */
static inline fdk_label *label_of(fdk_widget *w) {
    return (fdk_label *)w;
}
static inline fdk_button *button_of(fdk_widget *w) {
    return (fdk_button *)w;
}
static inline fdk_check_widget *check_of(fdk_widget *w) {
    return (fdk_check_widget *)w;
}
static inline fdk_progress *progress_of(fdk_widget *w) {
    return (fdk_progress *)w;
}
static inline fdk_separator *separator_of(fdk_widget *w) {
    return (fdk_separator *)w;
}
static inline fdk_frame *frame_of(fdk_widget *w) {
    return (fdk_frame *)w;
}

#endif /* FDK_WIDGETS_INTERNAL_H */
