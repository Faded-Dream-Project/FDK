/*
 * fdk_widgets.h — Faded Dream ToolKit: the core widget catalog
 *
 * Phase 6's widget family, built on the Phase 4 widget foundation
 * (subclass vtable, events, focus, invalidation), the Phase 5 box
 * layout engine, and the Phase 6 text layer. Eight widgets:
 *
 *   Label         static text
 *   Button        press/keyboard-activated command button
 *   Toggle        on/off switch with label
 *   Checkbox      on/off box with label
 *   RadioButton   one-of-many within its parent widget
 *   ProgressBar   determinate progress indicator
 *   Separator     horizontal/vertical rule
 *   Frame         titled vertical container (a box: add children
 *                 directly, layout arranges them below the title)
 *
 * Conventions:
 *
 *  - FONTS ARE BORROWED, TEXT IS COPIED. Widgets never own or destroy
 *    the fdk_font you pass; keep it alive as long as the widget
 *    lives. The string you pass to create/set is copied into
 *    toolkit-owned memory (UTF-8); get_text() returns a pointer to
 *    that copy, valid until the next set call or destroy.
 *
 *  - A NULL font is legal everywhere: the widget renders without its
 *    text (indicators and backgrounds still draw, buttons still
 *    activate) and measures text as zero-size. Useful for
 *    icon-adjacent builds and font-less tests.
 *
 *  - Type-checked handles. Every fdk_*_set_/get_ function takes the
 *    generic fdk_widget* and returns FDK_ERR_INVALID_ARGUMENT when
 *    the widget isn't of that exact class (no subclass coercion in
 *    v1 — applications subclassing the catalog is an ABI-freeze
 *    question, see fdk_widget.h).
 *
 *  - Colors are v1 built-ins (a dark palette consistent with the
 *    examples). The Phase 7 theme engine replaces them; the setters
 *    below are the only intentional color hooks until then.
 *
 *  - Interaction model: controls activate on pointer release INSIDE
 *    the widget after a press (the Phase 4 implicit grab keeps the
 *    release even if the pointer left), and on Space/Enter when
 *    focused. State visuals (hover/pressed/disabled) are automatic.
 *    Disabled widgets are input-transparent (Phase 4) and dimmed.
 */

#ifndef FDK_WIDGETS_H
#define FDK_WIDGETS_H

#include "fdk_types.h"
#include "fdk_error.h"
#include "fdk_widget.h"
#include "fdk_layout.h"
#include "fdk_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Label ---- */

/* Static text. Natural size = the text's measured size (a measure
 * hook), so a Label in a box sizes itself. Not focusable, not
 * interactive. text may be NULL (empty label). */
fdk_result fdk_label_create(fdk_widget *parent, fdk_font *font,
                            const char *text, fdk_widget **out_label);

/* Replaces the text (copied). Re-measures and relayouts the parent
 * container. NULL or "" clears the text. */
fdk_result fdk_label_set_text(fdk_widget *label, const char *text);

/* Text color (default: the palette's text color). */
void fdk_label_set_color(fdk_widget *label, fdk_color color);

/* The label's current text (toolkit-owned copy; valid until the next
 * set_text/destroy). NULL when the label has no text. */
const char *fdk_label_get_text(fdk_widget *label);

/* ---- Button ---- */

/* Command button with centered text. Natural size = text + padding.
 * Focusable. */
fdk_result fdk_button_create(fdk_widget *parent, fdk_font *font,
                             const char *text, fdk_widget **out_button);

fdk_result fdk_button_set_text(fdk_widget *button, const char *text);

/* Fires when the button activates: pointer release inside the widget
 * after a press on it, or Space/Enter while focused. May fire from
 * inside event dispatch — the usual reentrancy rules from fdk_widget.h
 * apply (destroying the button in the callback is safe). */
typedef void (*fdk_button_activate_fn)(fdk_widget *button,
                                       void *user_data);
void fdk_button_set_on_activate(fdk_widget *button,
                                fdk_button_activate_fn on_activate,
                                void *user_data);

/* ---- Toggle ---- */

/* On/off switch with optional trailing label. Natural size covers
 * the track + label. Focusable. */
fdk_result fdk_toggle_create(fdk_widget *parent, fdk_font *font,
                             const char *text, fdk_widget **out_toggle);

/* Sets the checked state (clamped to exactly true/false). Programmatic
 * changes fire on_change too. Repaints. */
void fdk_toggle_set_checked(fdk_widget *toggle, bool checked);
bool fdk_toggle_is_checked(fdk_widget *toggle);

typedef void (*fdk_toggle_change_fn)(fdk_widget *toggle, bool checked,
                                     void *user_data);
void fdk_toggle_set_on_change(fdk_widget *toggle,
                              fdk_toggle_change_fn on_change,
                              void *user_data);

/* ---- Checkbox ---- */

/* Same semantics as Toggle, box-and-check visuals. */
fdk_result fdk_checkbox_create(fdk_widget *parent, fdk_font *font,
                               const char *text,
                               fdk_widget **out_checkbox);
void fdk_checkbox_set_checked(fdk_widget *checkbox, bool checked);
bool fdk_checkbox_is_checked(fdk_widget *checkbox);
typedef void (*fdk_checkbox_change_fn)(fdk_widget *checkbox,
                                       bool checked, void *user_data);
void fdk_checkbox_set_on_change(fdk_widget *checkbox,
                                fdk_checkbox_change_fn on_change,
                                void *user_data);

/* ---- RadioButton ---- */

/* One-of-many selector. THE GROUP IS THE PARENT: every RadioButton
 * sharing a parent widget is one group; checking any member unchecks
 * the others. Focusable. */
fdk_result fdk_radio_create(fdk_widget *parent, fdk_font *font,
                            const char *text, fdk_widget **out_radio);

/* Checks this radio and unchecks its siblings (their on_change
 * callbacks fire with false; this one's fires with true). Checking
 * an already-checked radio is a no-op. Unchecking a radio directly
 * is allowed (leaves the group with no selection). */
void fdk_radio_set_checked(fdk_widget *radio, bool checked);
bool fdk_radio_is_checked(fdk_widget *radio);

typedef void (*fdk_radio_change_fn)(fdk_widget *radio, bool checked,
                                    void *user_data);
void fdk_radio_set_on_change(fdk_widget *radio,
                             fdk_radio_change_fn on_change,
                             void *user_data);

/* ---- ProgressBar ---- */

/* Determinate progress bar. No natural text size — give it a size
 * request (fdk_widget_set_natural_size / expand) and layout stretches
 * it. Not interactive, not focusable. */
fdk_result fdk_progress_create(fdk_widget *parent,
                               fdk_widget **out_progress);

/* Sets the fraction, clamped to [0, 1]. Repaints. */
void fdk_progress_set_fraction(fdk_widget *progress, fdk_f32 fraction);
fdk_f32 fdk_progress_get_fraction(fdk_widget *progress);

/* ---- Separator ---- */

/* A 1-px rule. Horizontal separators want expand-on-x in a vertical
 * box (and vice versa); the natural size is 1 px on the cross axis
 * and 0 on the along axis, so layout stretches them. */
fdk_result fdk_separator_create(fdk_widget *parent,
                                fdk_orientation orientation,
                                fdk_widget **out_separator);

/* ---- Frame ---- */

/* A titled vertical container. It IS a box (children added with
 * fdk_widget_create / other catalog widgets are arranged below the
 * title band automatically); the border and title draw in the frame's
 * paint pass, under the children. Set a background to fill it. */
fdk_result fdk_frame_create(fdk_widget *parent, fdk_font *font,
                            const char *title, fdk_widget **out_frame);

fdk_result fdk_frame_set_title(fdk_widget *frame, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* FDK_WIDGETS_H */
