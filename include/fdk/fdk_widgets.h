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

/* How the label fits its text into the width it is allocated:
 *
 *   NOWRAP     one line, drawn from the left edge (the v1 behavior;
 *              the default). Overlong text is clipped by the label's
 *              bounds.
 *   WRAP       greedy word-wrap (see fdk_font_break_lines_utf8 in
 *              fdk_text.h) at the label's CURRENT width, rebuilt on
 *              every resize. Natural width = the natural-size request
 *              when one is set (fdk_widget_set_natural_size), else
 *              the full unwrapped advance; natural height = the line
 *              count at that width times the line pitch. v1 has no
 *              width-for-height layout: when a container allocates
 *              less width than requested, the label re-wraps taller
 *              than its allocated height and the tail clips — give
 *              wrap labels a width request and headroom.
 *   ELLIPSIZE  one line truncated with "..." (U+2026) at the current
 *              width (see fdk_font_ellipsize_utf8). Natural size is
 *              the FULL text: an ellipsized label shows everything it
 *              is given room for.
 *
 * Line pitch is the font's ascent + descent (line_gap is not added —
 * the same extent every other catalog widget uses for one line of
 * text). Multi-line labels are top-anchored. */
typedef enum fdk_label_mode {
    FDK_LABEL_NOWRAP = 0,
    FDK_LABEL_WRAP = 1,
    FDK_LABEL_ELLIPSIZE = 2,
} fdk_label_mode;

/* Sets the fitting mode. Re-measures (WRAP changes the natural
 * height) and relayouts the parent container. Unknown enum values
 * are ignored. */
void fdk_label_set_mode(fdk_widget *label, fdk_label_mode mode);
fdk_label_mode fdk_label_get_mode(fdk_widget *label);

/* Horizontal alignment of each line within the label's width:
 * START (default; FILL behaves as START), CENTER, END. Overlong
 * lines still start at the left edge rather than spilling left. */
void fdk_label_set_alignment(fdk_widget *label, fdk_align alignment);
fdk_align fdk_label_get_alignment(fdk_widget *label);

/* The number of display lines under the label's CURRENT mode and
 * width (NOWRAP/ELLIPSIZE: 1 with text, else 0; WRAP: the wrapped
 * count). Rebuilds the display cache lazily — valid immediately
 * after arrange. */
size_t fdk_label_get_line_count(fdk_widget *label);

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
 * the others. Focusable.
 *
 * Keyboard: Up/Left move selection to the previous group member and
 * Down/Right to the next (wrapping, skipping hidden/disabled
 * members); focus follows selection. Space/Enter check the focused
 * radio. A group with no other member lets the arrows bubble. */
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

/* ---- Entry (Phase 9) ----
 *
 * Single-line UTF-8 text input: caret (always on a codepoint
 * boundary), selection with shift+arrows / drag / double-click
 * (word) / triple-click (all), clipboard cut/copy/paste via
 * Ctrl+X/C/V (through the owning window's context — see
 * fdk_clipboard.h), word-wise motion with Ctrl+Left/Right, and
 * horizontal scrolling that keeps the caret visible.
 *
 * IME GROUNDWORK: fdk_entry_set_preedit renders a composition string
 * inline at the caret with an underline while a real input-method
 * layer composes. The preedit is display-only: it never enters the
 * buffer until the IME commits (the application then inserts the
 * committed text like any other input).
 *
 * Shift+click extends the selection from its anchor; shift+arrows
 * and drag do the same from the keyboard/pointer side.
 *
 * Max text length: 64 KiB (bounded input, docs/security.md); longer
 * inserts are refused with FDK_ERR_INVALID_ARGUMENT and a warning.
 */

/* Notified after every buffer mutation (typed/deleted/pasted text,
 * fdk_entry_set_text). Programmatic reads see the new text already. */
typedef void (*fdk_entry_changed_fn)(fdk_widget *entry, void *user_data);

/* Enter pressed. */
typedef void (*fdk_entry_activate_fn)(fdk_widget *entry, void *user_data);

fdk_result fdk_entry_create(fdk_widget *parent, fdk_font *font,
                            const char *text, fdk_widget **out_entry);

/* Current text (toolkit-owned, UTF-8, never NULL; "" when empty).
 * Valid until the next mutation or destroy. */
const char *fdk_entry_get_text(fdk_widget *entry);

/* Replaces the whole buffer. Resets caret+selection to the end and
 * fires on_changed. NULL is treated as "". */
fdk_result fdk_entry_set_text(fdk_widget *entry, const char *text);

/* Caret position as a BYTE offset into the text. set refuses
 * off-boundary offsets (FDK_ERR_INVALID_ARGUMENT) rather than
 * silently snapping. */
size_t fdk_entry_get_cursor(fdk_widget *entry);
fdk_result fdk_entry_set_cursor(fdk_widget *entry, size_t byte_offset);

/* Selection as [anchor, caret) byte offsets. anchor == caret means
 * no selection. Either endpoint may be the earlier one — the range
 * is the span between them. select_range refuses off-boundary
 * offsets; select_all is the whole buffer. */
fdk_result fdk_entry_get_selection(fdk_widget *entry, size_t *anchor,
                                   size_t *caret);
fdk_result fdk_entry_select_range(fdk_widget *entry, size_t anchor,
                                  size_t caret);
void fdk_entry_select_all(fdk_widget *entry);

/* IME groundwork: display `preedit` inline at the caret, underlined;
 * NULL or "" clears it. Does not touch the buffer or the selection
 * and fires no callbacks. */
fdk_result fdk_entry_set_preedit(fdk_widget *entry, const char *preedit);

void fdk_entry_set_on_changed(fdk_widget *entry,
                              fdk_entry_changed_fn on_changed,
                              void *user_data);
void fdk_entry_set_on_activate(fdk_widget *entry,
                               fdk_entry_activate_fn on_activate,
                               void *user_data);

/* ---- ScrollView (Phase 9) ----
 *
 * A scrolling container: exactly one content child, clipped to the
 * viewport, with overlay scrollbars that appear only when their axis
 * overflows. Scroll with the wheel anywhere over the content, by
 * dragging the scrollbar thumbs, by clicking a trough (page), or —
 * when the scrollview itself is focused (opt in with
 * fdk_widget_set_can_focus) — with arrows/PageUp/PageDown/Home/End.
 *
 * Ownership: set_content REPARENTS the widget into the scrollview
 * (and destroys any previous content, the standard FDK
 * parent-owns-children model). Natural size = the content's natural
 * size, so a scrollview only actually scrolls once something SMALLER
 * is assigned to it (an explicit set_bounds, or a fixed slot in a
 * layout); that is the intended use. Wheel notches step 48 px;
 * themed metric scrollbar_width (6..24, default 12) sizes the bars.
 */

fdk_result fdk_scrollview_create(fdk_widget *parent,
                                 fdk_widget **out_scrollview);

/* Adopts `content` as the scrollable child (reparented, replacing
 * and destroying any previous content). NULL clears (and destroys)
 * the current content. */
fdk_result fdk_scrollview_set_content(fdk_widget *scrollview,
                                      fdk_widget *content);

/* Absolute scroll offsets; clamped to [0, content - viewport] on
 * both axes. get returns the clamped truth. */
fdk_result fdk_scrollview_scroll_to(fdk_widget *scrollview, fdk_i32 x,
                                    fdk_i32 y);
void fdk_scrollview_scroll_by(fdk_widget *scrollview, fdk_i32 dx,
                              fdk_i32 dy);
fdk_result fdk_scrollview_get_scroll_offset(fdk_widget *scrollview,
                                            fdk_i32 *out_x,
                                            fdk_i32 *out_y);

/* ---- List (Phase 9) ----
 *
 * A scrolling list of text rows with single / multiple / no
 * selection, built on the ScrollView (bars, wheel, clipping come
 * from it). Rows are left-aligned text at a fixed per-font row
 * height; the list is focusable and keyboard-navigable
 * (Up/Down/Home/End/PageUp/PageDown; shift extends in MULTIPLE
 * mode). Click selects; ctrl+click toggles; shift+click ranges —
 * the pointer event's modifier state (Phase 9) drives it.
 *
 * Selection reads: get_selected (first selected, -1 when none) for
 * SINGLE mode; selected_count + selected_at(position) enumerate
 * MULTIPLE selections in row order. The selection-changed callback
 * fires once per settled change (clicks, keyboard, programmatic).
 */

typedef enum fdk_list_selection_mode {
    FDK_LIST_SELECTION_NONE = 0,
    FDK_LIST_SELECTION_SINGLE = 1,
    FDK_LIST_SELECTION_MULTIPLE = 2,
} fdk_list_selection_mode;

/* Notified after the selection settles. */
typedef void (*fdk_list_selection_fn)(fdk_widget *list,
                                      void *user_data);

fdk_result fdk_list_create(fdk_widget *parent, fdk_font *font,
                           fdk_widget **out_list);

void fdk_list_set_selection_mode(fdk_widget *list,
                                 fdk_list_selection_mode mode);

/* Row CRUD. append writes the new row's index to *out_index when
 * non-NULL. Row text is copied. */
fdk_result fdk_list_append(fdk_widget *list, const char *text,
                           size_t *out_index);
fdk_result fdk_list_insert(fdk_widget *list, size_t index,
                           const char *text);
fdk_result fdk_list_remove(fdk_widget *list, size_t index);
void fdk_list_clear(fdk_widget *list);
size_t fdk_list_row_count(fdk_widget *list);
const char *fdk_list_row_text(fdk_widget *list, size_t row);
fdk_result fdk_list_set_row_text(fdk_widget *list, size_t row,
                                 const char *text);

/* Selection queries and programmatic select (which follows the
 * current mode: cleared-then-one, honoring MULTIPLE's additive
 * contract). */
fdk_i64 fdk_list_get_selected(fdk_widget *list);
bool fdk_list_is_selected(fdk_widget *list, size_t row);
size_t fdk_list_selected_count(fdk_widget *list);
fdk_result fdk_list_selected_at(fdk_widget *list, size_t position,
                                size_t *out_row);
fdk_result fdk_list_select(fdk_widget *list, size_t row);

void fdk_list_set_on_selection_changed(fdk_widget *list,
                                       fdk_list_selection_fn fn,
                                       void *user_data);

/* ---- Tree (Phase 9) ----
 *
 * A hierarchical expandable tree on the ScrollView: nodes hold text,
 * children nest under indentation, parents get a stroked-triangle
 * expander (font-independent, like the title-bar glyphs). Click a
 * row to select (SINGLE selection — multi-select trees are parked
 * honestly); click the expander zone to collapse/expand. Keyboard:
 * Up/Down walk VISIBLE nodes, Left collapses-or-jumps-to-parent,
 * Right expands-or-enters-first-child, Home/End/PageUp/PageDown as
 * in List.
 *
 * Node handles (fdk_tree_node) are stable for the tree's lifetime:
 * they index the internal node store, which only ever grows.
 * FDK_TREE_NODE_NONE means "no node" (root parent, no selection).
 */

typedef size_t fdk_tree_node;
#define FDK_TREE_NODE_NONE ((size_t)-1)

typedef void (*fdk_tree_selection_fn)(fdk_widget *tree,
                                      void *user_data);

fdk_result fdk_tree_create(fdk_widget *parent, fdk_font *font,
                           fdk_widget **out_tree);

/* Adds `text` as the last child of `parent` (FDK_TREE_NODE_NONE =
 * a root-level node). Writes the new handle to *out_node when
 * non-NULL. */
fdk_result fdk_tree_node_add(fdk_widget *tree, fdk_tree_node parent,
                             const char *text,
                             fdk_tree_node *out_node);
fdk_result fdk_tree_node_set_text(fdk_widget *tree,
                                  fdk_tree_node node,
                                  const char *text);
const char *fdk_tree_node_text(fdk_widget *tree, fdk_tree_node node);

/* Expand/collapse (parents only; leaves return
 * FDK_ERR_INVALID_ARGUMENT). */
fdk_result fdk_tree_node_expand(fdk_widget *tree, fdk_tree_node node,
                                bool expanded);
bool fdk_tree_node_is_expanded(fdk_widget *tree,
                               fdk_tree_node node);
size_t fdk_tree_node_child_count(fdk_widget *tree,
                                 fdk_tree_node node);

fdk_tree_node fdk_tree_get_selected(fdk_widget *tree);
fdk_result fdk_tree_select(fdk_widget *tree, fdk_tree_node node);
size_t fdk_tree_visible_count(fdk_widget *tree);

void fdk_tree_set_on_selection_changed(fdk_widget *tree,
                                       fdk_tree_selection_fn fn,
                                       void *user_data);

/* ---- Slider (Phase 9) ----
 *
 * A draggable value picker over [min, max]: press anywhere to jump
 * the thumb there, drag with the implicit grab, step with arrows,
 * page with PageUp/PageDown, ends with Home/End. Values quantize to
 * `step` when one is set. Horizontal only in v1 (vertical parked). */

typedef void (*fdk_slider_changed_fn)(fdk_widget *slider,
                                      void *user_data);

fdk_result fdk_slider_create(fdk_widget *parent, double min,
                             double max, double value,
                             fdk_widget **out_slider);
void fdk_slider_set_range(fdk_widget *slider, double min, double max);
void fdk_slider_set_step(fdk_widget *slider, double step);
void fdk_slider_set_value(fdk_widget *slider, double value);
double fdk_slider_get_value(fdk_widget *slider);
void fdk_slider_set_on_changed(fdk_widget *slider,
                               fdk_slider_changed_fn on_changed,
                               void *user_data);

/* ---- SpinButton (Phase 9) ----
 *
 * A numeric entry: an embedded Entry (full text editing, selection,
 * clipboard) + up/down stepper chevrons. The value commits on Enter,
 * stepper presses, and focus leaving; commits clamp to [min, max]
 * and rewrite the buffer, so text and value never disagree. An
 * unparsable buffer reads as the last committed value. Up/Down/Page
 * keys step (the caret motion those keys would do is consumed). */

typedef void (*fdk_spin_changed_fn)(fdk_widget *spin,
                                    void *user_data);

fdk_result fdk_spin_create(fdk_widget *parent, fdk_font *font,
                           double min, double max, double value,
                           fdk_widget **out_spin);
void fdk_spin_set_range(fdk_widget *spin, double min, double max);
void fdk_spin_set_step(fdk_widget *spin, double step);
void fdk_spin_set_value(fdk_widget *spin, double value);
double fdk_spin_get_value(fdk_widget *spin);
/* The raw buffer (delegated to the embedded entry). */
const char *fdk_spin_get_text(fdk_widget *spin);
void fdk_spin_set_on_changed(fdk_widget *spin,
                             fdk_spin_changed_fn on_changed,
                             void *user_data);

/* ---- Toolbar (Phase 9) ----
 *
 * A horizontal action bar: flat buttons and separators in a row.
 * Buttons are stock catalog Buttons (click/hover/keyboard) — the
 * toolbar contributes the bar chrome and the row arrangement.
 * Overflow (wrap/chevron menu) is parked; narrower bars clip. */

fdk_result fdk_toolbar_create(fdk_widget *parent, fdk_font *font,
                              fdk_widget **out_toolbar);
fdk_result fdk_toolbar_add_button(fdk_widget *toolbar,
                                  const char *text,
                                  fdk_button_activate_fn on_activate,
                                  void *user_data,
                                  fdk_widget **out_button);
fdk_result fdk_toolbar_add_separator(fdk_widget *toolbar);

/* ---- Notebook / TabView (Phase 9) ----
 *
 * A tab strip over a page area. Pages are ordinary widgets the
 * notebook ADOPTS (append_page reparents; exactly one visible at a
 * time — invisible pages are input-transparent and skipped by the
 * paint walk). Tab clicks switch; the switch callback fires after
 * the switch settles. Close buttons / tab reordering parked. */

typedef void (*fdk_notebook_switch_fn)(fdk_widget *notebook,
                                       size_t page, void *user_data);

fdk_result fdk_notebook_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_notebook);
fdk_result fdk_notebook_append_page(fdk_widget *notebook,
                                    fdk_widget *page,
                                    const char *label);
size_t fdk_notebook_page_count(fdk_widget *notebook);
fdk_result fdk_notebook_set_current_page(fdk_widget *notebook,
                                         size_t index);
size_t fdk_notebook_get_current_page(fdk_widget *notebook);
fdk_widget *fdk_notebook_get_page(fdk_widget *notebook,
                                  size_t index);
void fdk_notebook_set_on_switch(fdk_widget *notebook,
                                fdk_notebook_switch_fn fn,
                                void *user_data);

/* ---- Canvas (Phase 9) ----
 *
 * An application-drawable widget: the paint callback receives the
 * surface, the widget's absolute bounds, and the effective clip at
 * paint time. Draw with the surface primitives; everything the
 * paint machinery guarantees (bounds clipping, damage-driven
 * repaints, idempotency) applies to the callback's drawing. The
 * callback runs inside the paint walk: draw only — no tree
 * mutation, no destroy, no re-entrant paints. */

typedef void (*fdk_canvas_paint_fn)(fdk_widget *canvas,
                                    fdk_surface *surface,
                                    fdk_rect bounds, fdk_rect clip,
                                    void *user_data);

fdk_result fdk_canvas_create(fdk_widget *parent,
                             fdk_canvas_paint_fn on_paint,
                             void *user_data,
                             fdk_widget **out_canvas);
void fdk_canvas_set_paint_callback(fdk_widget *canvas,
                                   fdk_canvas_paint_fn on_paint,
                                   void *user_data);
void fdk_canvas_invalidate(fdk_widget *canvas);

#ifdef __cplusplus
}
#endif

#endif /* FDK_WIDGETS_H */
