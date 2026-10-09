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

/* Text color (default: the palette's text color — get_color
 * reports the same resolution). */
void fdk_label_set_color(fdk_widget *label, fdk_color color);
/* The effective text color: the explicit one when set, else the
 * theme's text color (the paint-time resolution, reported). */
fdk_color fdk_label_get_color(fdk_widget *label);

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

/* Replaces the label (copied; re-measures). */
fdk_result fdk_button_set_text(fdk_widget *button, const char *text);
/* The button's current label (toolkit-owned copy; valid until the
 * next set_text/destroy; NULL when it has no text). */
const char *fdk_button_get_text(fdk_widget *button);

/* Fires when the button activates: pointer release inside the widget
 * after a press on it, or Space/Enter while focused. May fire from
 * inside event dispatch — the usual reentrancy rules from fdk_widget.h
 * apply (destroying the button in the callback is safe). */
typedef void (*fdk_button_activate_fn)(fdk_widget *button,
                                       void *user_data);
/* Sets the activation callback (see the typedef above). */
void fdk_button_set_on_activate(fdk_widget *button,
                                fdk_button_activate_fn on_activate,
                                void *user_data);

/* 1.4.0 — the button's visual role (GTK/Qt action styles): NORMAL
 * paints with the control family; SUGGESTED paints accent-filled
 * (the "Open"/"Save" button of a dialog — the one action that makes
 * the dialog happen); DESTRUCTIVE paints danger-filled (the
 * irreversible action). Roles are paint-only: same size, same
 * activation, same focus behavior. Default NORMAL. */
typedef enum fdk_button_role {
    FDK_BUTTON_ROLE_NORMAL = 0,
    FDK_BUTTON_ROLE_SUGGESTED = 1,
    FDK_BUTTON_ROLE_DESTRUCTIVE = 2,
    /* 1.4.1 append — the link-styled button (GTK LinkButton / Qt
     * flat link): no fill, link-colored text (the FDK_TK_LINK token),
     * an underline while hovered, the regular focus ring. Paint-only,
     * like every role. */
    FDK_BUTTON_ROLE_LINK = 3,
} fdk_button_role;

/* Sets the role (repaints; unknown enum values are ignored). */
void fdk_button_set_role(fdk_widget *button, fdk_button_role role);
/* The current role (FDK_BUTTON_ROLE_NORMAL for non-buttons). */
fdk_button_role fdk_button_get_role(fdk_widget *button);

/* 1.4.0 — the toggle-button state (a Button that stays "pressed in"
 * when checked, like GTK's GtkToggleButton / Qt's checkable QPushButton).
 * A checked NORMAL button paints the pressed fill persistently; a
 * checked SUGGESTED/DESTRUCTIVE button paints the accent/danger
 * pressed fill. Paint-only: activation and focus are unchanged, and
 * unlike Toggle there is no on-changed callback — buttons with state
 * are usually driven by app state (set it when that state changes). */
void fdk_button_set_checked(fdk_widget *button, bool checked);
/* The current checked state (false for non-buttons). */
bool fdk_button_is_checked(fdk_widget *button);

/* ---- Toggle ---- */

/* On/off switch with optional trailing label. Natural size covers
 * the track + label. Focusable. */
fdk_result fdk_toggle_create(fdk_widget *parent, fdk_font *font,
                             const char *text, fdk_widget **out_toggle);

/* Sets the checked state (clamped to exactly true/false). Programmatic
 * changes fire on_change too. Repaints. */
void fdk_toggle_set_checked(fdk_widget *toggle, bool checked);
/* The current checked state. */
bool fdk_toggle_is_checked(fdk_widget *toggle);

typedef void (*fdk_toggle_changed_fn)(fdk_widget *toggle, bool checked,
                                     void *user_data);
void fdk_toggle_set_on_changed(fdk_widget *toggle,
                              fdk_toggle_changed_fn on_change,
                              void *user_data);

/* ---- Checkbox ---- */

/* Same semantics as Toggle, box-and-check visuals. */
fdk_result fdk_checkbox_create(fdk_widget *parent, fdk_font *font,
                               const char *text,
                               fdk_widget **out_checkbox);
/* Same setter contract as Toggle. */
void fdk_checkbox_set_checked(fdk_widget *checkbox, bool checked);
/* The current checked state. */
bool fdk_checkbox_is_checked(fdk_widget *checkbox);
typedef void (*fdk_checkbox_changed_fn)(fdk_widget *checkbox,
                                       bool checked, void *user_data);
/* Same callback contract as Toggle. */
void fdk_checkbox_set_on_changed(fdk_widget *checkbox,
                                fdk_checkbox_changed_fn on_change,
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
/* The current checked state. */
bool fdk_radio_is_checked(fdk_widget *radio);

typedef void (*fdk_radio_changed_fn)(fdk_widget *radio, bool checked,
                                    void *user_data);
/* Same callback contract as Toggle. */
void fdk_radio_set_on_changed(fdk_widget *radio,
                             fdk_radio_changed_fn on_change,
                             void *user_data);

/* ---- ProgressBar ---- */

/* Determinate progress bar. No natural text size — give it a size
 * request (fdk_widget_set_natural_size / expand) and layout stretches
 * it. Not interactive, not focusable. */
fdk_result fdk_progress_create(fdk_widget *parent,
                               fdk_widget **out_progress);

/* Sets the fraction, clamped to [0, 1]. Repaints. */
void fdk_progress_set_fraction(fdk_widget *progress, fdk_f32 fraction);
/* The current fraction, as last set (already clamped). */
fdk_f32 fdk_progress_get_fraction(fdk_widget *progress);
/* Indeterminate ("busy") mode (1.3.1): an accent block sweeps the
 * track on the timer clock (~1.9s per traversal) — the visual answer
 * to "work is happening, no fraction exists". Entering it clears the
 * fraction; any set_fraction leaves it (a known fraction beats
 * busy). The animation needs a window's event loop; detached trees
 * show a static block. The a11y value interface reports "busy"
 * instead of a number while active. */
void fdk_progress_set_indeterminate(fdk_widget *progress,
                                    bool indeterminate);
/* Whether busy mode is active. */
bool fdk_progress_is_indeterminate(fdk_widget *progress);

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

/* Replaces the title (copied; NULL clears). Re-measures. */
fdk_result fdk_frame_set_title(fdk_widget *frame, const char *title);
/* The frame's current title (toolkit-owned; valid until the next
 * set_title/destroy; NULL when there is none). */
const char *fdk_frame_get_title(fdk_widget *frame);

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

/* Creates the entry with initial text (copied; NULL = empty). */
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
/* Moves the caret (collapsing the selection). */
fdk_result fdk_entry_set_cursor(fdk_widget *entry, size_t byte_offset);

/* Selection as [anchor, caret) byte offsets. anchor == caret means
 * no selection. Either endpoint may be the earlier one — the range
 * is the span between them. select_range refuses off-boundary
 * offsets; select_all is the whole buffer. */
fdk_result fdk_entry_get_selection(fdk_widget *entry, size_t *anchor,
                                   size_t *caret);
/* Sets the selection (and the caret to its far end). */
fdk_result fdk_entry_select_range(fdk_widget *entry, size_t anchor,
                                  size_t caret);
/* Selects the whole buffer. */
void fdk_entry_select_all(fdk_widget *entry);

/* IME groundwork: display `preedit` inline at the caret, underlined;
 * NULL or "" clears it. Does not touch the buffer or the selection
 * and fires no callbacks. */
fdk_result fdk_entry_set_preedit(fdk_widget *entry, const char *preedit);

/* 1.4.0 — PLACEHOLDER TEXT, shown in the disabled-text color while
 * the buffer is empty (and no preedit is showing). It is never
 * selectable, never copied, never in the buffer, and disappears the
 * moment text exists. Copied; NULL clears it. The placeholder has
 * no effect on the entry's natural size (it may ellipsize if longer
 * than the field). */
void fdk_entry_set_placeholder(fdk_widget *entry, const char *text);
/* The current placeholder (toolkit-owned; NULL when unset). */
const char *fdk_entry_get_placeholder(fdk_widget *entry);

/* PASSWORD MODE: renders one bullet per cluster instead of the text.
 * The buffer, the caret, the selection, and the hit-testing all keep
 * working in the same byte/cluster space — only the rendering (and
 * its geometry) changes. The text is still readable through
 * fdk_entry_get_text and the accessibility value interface (masking
 * there is a bridge decision, not the toolkit's). */
void fdk_entry_set_password(fdk_widget *entry, bool password);
/* Whether password (bullet) rendering is on. */
bool fdk_entry_is_password(fdk_widget *entry);

/* READ-ONLY: selection and copy keep working (the reader contract);
 * typing, Backspace/Delete, cut, and paste are consumed and ignored.
 * Programmatic fdk_entry_set_text still works. */
void fdk_entry_set_read_only(fdk_widget *entry, bool read_only);
/* Whether typing is refused (selection + copy still work). */
bool fdk_entry_is_read_only(fdk_widget *entry);

/* EDITABLE CAP in bytes (0 = the 64 KiB hard limit). Typing, paste,
 * and fdk_entry_set_text all refuse to exceed it; selection and
 * reading are unaffected. Shrinking below the current length does
 * NOT truncate the existing text — the cap applies to growth. */
void fdk_entry_set_max_length(fdk_widget *entry, size_t max_bytes);
/* The byte cap (0 = unlimited). */
size_t fdk_entry_get_max_length(fdk_widget *entry);

/* ---- UNDO / REDO (1.3.3) ----
 *
 * Every user edit — typed characters, Backspace/Delete runs, cut,
 * paste, selection-deletes — is recorded onto an undo history the
 * entry owns (an fdk_undo_stack; the first edit allocates it). The
 * built-in keys are Ctrl+Z (undo), Ctrl+Shift+Z and Ctrl+Y (redo),
 * matched on the PRINTED key like every Entry editing binding (see
 * fdk_shortcut_parse for the physical-key contrast).
 *
 * Coalescing, stated exactly (also in entry.c's contract block):
 * consecutive single-codepoint inserts at the advancing typing
 * frontier merge into one undo step; consecutive single-codepoint
 * deletions that extend one range contiguously (hold Backspace, or
 * hold Delete) merge too. Pastes, cuts, selection-deletes, and
 * multi-codepoint edits are atomic steps. No time component — the
 * rule is deterministic.
 *
 * Undo restores the text AND the pre-edit caret/selection (a
 * restored deletion arrives SELECTED, the GTK behavior). Redo
 * re-applies with the caret after the re-inserted text. Both fire
 * on_changed / a11y notifications like any edit.
 *
 * fdk_entry_set_text CLEARS the history (a programmatic overwrite
 * is a mode change, not an edit — the documented rule). Read-only
 * entries report can_undo/can_redo == false and refuse undo/redo.
 * FDK_ERR_INVALID_STATE from the programmatic calls means exactly
 * that (nothing recorded yet). */
bool fdk_entry_can_undo(fdk_widget *entry);
bool fdk_entry_can_redo(fdk_widget *entry);
fdk_result fdk_entry_undo(fdk_widget *entry);
fdk_result fdk_entry_redo(fdk_widget *entry);

/* Buffer-mutation callback (see the typedef above). */
void fdk_entry_set_on_changed(fdk_widget *entry,
                              fdk_entry_changed_fn on_changed,
                              void *user_data);
/* Enter-press callback. */
void fdk_entry_set_on_activate(fdk_widget *entry,
                               fdk_entry_activate_fn on_activate,
                               void *user_data);

/* ---- Entry icon slots (1.4.4) -------------------------------------
 *
 * The Entry's generalized prefix/suffix surface — the 1.4.2 search
 * preset's magnifier and clear zones promoted to a public, symmetric
 * API. Each slot (LEADING = before the text, TRAILING = after it)
 * reserves a 16-px glyph box + 6-px gap when an icon is set: the
 * entry's natural width, hit-testing, text scrolling, and paint all
 * route through the same two zone widths, exactly like the search
 * preset did (the caret and the glyph never fight).
 *
 * Built-in glyphs are the toolkit's vector language (strokes in the
 * theme's text ink — no bitmaps, no icon theme): the SEARCH lens,
 * the CLEAR X, and the four symbolic row glyphs (FOLDER/HOME/DRIVE/
 * FILE — see fdk_row_icon; a path-style entry wears FOLDER well).
 * A custom paint callback replaces the built-in glyph entirely: it
 * receives the box's top-left and the 16-px extent, and may draw
 * anything (including loaded images via the surface API).
 *
 * Interaction: a left press inside a slot fires the slot's
 * on_icon_press callback. The CLEAR icon is SPECIAL — it carries the
 * search preset's honest clear semantics everywhere it appears: a
 * press empties the field through the UNDOABLE splice path (records
 * undo, fires on_changed) unless the entry is read-only or the slot
 * was marked insensitive, and its zone EXISTS ONLY WHILE THE FIELD
 * HAS TEXT (the button appears with the first character — the 1.4.2
 * rule, now every entry's rule). A leading icon that is decorative
 * (no callback) lets presses fall through to caret placement; an
 * interactive slot (callback set, or CLEAR) consumes its presses.
 * Hover shows the soft pill behind interactive, sensitive slots.
 *
 * The search PRESET is now built on the slots (leading SEARCH,
 * trailing CLEAR) — its Esc-ladder and debounced search-changed stay
 * preset-specific behavior, geometry identical to 1.4.2. */

typedef enum fdk_entry_icon {
    FDK_ENTRY_ICON_NONE = 0,  /* no icon: the slot reserves nothing  */
    FDK_ENTRY_ICON_SEARCH,    /* the stroked lens + handle           */
    FDK_ENTRY_ICON_CLEAR,     /* the X; press = undoable clear       */
    FDK_ENTRY_ICON_FOLDER,    /* the symbolic row-glyph family       */
    FDK_ENTRY_ICON_HOME,
    FDK_ENTRY_ICON_DRIVE,
    FDK_ENTRY_ICON_FILE,
} fdk_entry_icon;

typedef enum fdk_entry_slot {
    FDK_ENTRY_SLOT_LEADING  = 0, /* before the text (the prefix)     */
    FDK_ENTRY_SLOT_TRAILING = 1, /* after the text (the suffix)      */
} fdk_entry_slot;

/* Slot press callback. Fires for CLEAR after the clear itself. */
typedef void (*fdk_entry_icon_fn)(fdk_widget *entry,
                                  fdk_entry_slot slot, void *user_data);

/* Custom glyph painter: draw within the box whose top-left is (x, y)
 * and whose extent is `box` (16) px. `enabled` folds together the
 * entry's enabled flag and the slot's sensitivity (dim your ink). */
typedef void (*fdk_entry_icon_paint_fn)(fdk_widget *entry,
                                        fdk_surface *surface,
                                        fdk_i32 x, fdk_i32 y,
                                        fdk_i32 box, bool enabled,
                                        void *user_data);

/* Sets the slot's built-in glyph (NONE clears it — a custom painter
 * set for the slot also clears back to the built-in when it is
 * replaced by passing fn == NULL). Relayouts the parent container:
 * the entry's natural size changes with the zone. */
fdk_result fdk_entry_set_icon(fdk_widget *entry, fdk_entry_slot slot,
                              fdk_entry_icon icon);
/* The slot's current built-in glyph (NONE when a custom painter owns
 * the slot). */
fdk_entry_icon fdk_entry_get_icon(fdk_widget *entry, fdk_entry_slot slot);
/* Replaces the slot's glyph with a custom painter (NULL reverts to
 * the built-in glyph set by fdk_entry_set_icon). */
fdk_result fdk_entry_set_icon_paint(fdk_widget *entry,
                                    fdk_entry_slot slot,
                                    fdk_entry_icon_paint_fn fn,
                                    void *user_data);
/* Dims the slot's glyph and disables its interaction (a decorative
 * state: the CLEAR icon stops clearing, presses stop firing). */
fdk_result fdk_entry_set_icon_sensitive(fdk_widget *entry,
                                        fdk_entry_slot slot,
                                        bool sensitive);
bool fdk_entry_icon_is_sensitive(fdk_widget *entry, fdk_entry_slot slot);
/* The slot's press callback. */
void fdk_entry_set_on_icon_press(fdk_widget *entry,
                                 fdk_entry_slot slot,
                                 fdk_entry_icon_fn fn, void *user_data);

/* ---- ScrollView (Phase 9) ----
 *
 * A scrolling container: exactly one content child, clipped to the
 * viewport, with edge scrollbars that appear only when their axis
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

/* ---- Bar modes (1.4.4) -------------------------------------------
 *
 * CLASSIC (the default, the historic behavior): the bars are
 * LAYOUT-OWNED — an overflowing axis reserves its strip, the
 * viewport shrinks by every visible bar, and the bars are always
 * fully opaque and interactive (trough paging included).
 *
 * OVERLAY: the bars are TRANSIENT GUESTS — the viewport is the FULL
 * bounds (nothing is reserved; the content renders underneath),
 * the bars are thinner (themed metric scrollbar_overlay_width,
 * 4..12, default 6) and paint only their thumb over the content.
 * After ~800 ms of idleness a bar fades out (~300 ms, the 1.4.3
 * paint-group engine doing the blending) and becomes input-
 * transparent; any scrolling on its axis, the pointer approaching
 * its strip, a hover, or a thumb drag pops it back at full opacity
 * with the idle clock re-armed. Standalone/detached trees keep the
 * bars visible (no window context -> no idle clock — the animation
 * layer's headless-honesty rule, one more time).
 *
 * Switching modes re-arranges immediately (the viewport changes in
 * overlay) and re-clamps the scroll offsets. */

typedef enum fdk_scroll_bar_mode {
    FDK_SCROLL_BARS_CLASSIC = 0, /* edge strips, layout-owned       */
    FDK_SCROLL_BARS_OVERLAY,    /* thin, transient, fades when idle */
} fdk_scroll_bar_mode;

fdk_result fdk_scrollview_set_bar_mode(fdk_widget *scrollview,
                                       fdk_scroll_bar_mode mode);
fdk_scroll_bar_mode fdk_scrollview_get_bar_mode(fdk_widget *scrollview);

/* Adopts `content` as the scrollable child (reparented, replacing
 * and destroying any previous content). NULL clears (and destroys)
 * the current content. */
fdk_result fdk_scrollview_set_content(fdk_widget *scrollview,
                                      fdk_widget *content);

/* Absolute scroll offsets; clamped to [0, content - viewport] on
 * both axes. get returns the clamped truth. */
fdk_result fdk_scrollview_scroll_to(fdk_widget *scrollview, fdk_i32 x,
                                    fdk_i32 y);
/* Relative scroll, clamped like scroll_to. */
void fdk_scrollview_scroll_by(fdk_widget *scrollview, fdk_i32 dx,
                              fdk_i32 dy);
/* The clamped current offsets. */
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

/* SINGLE selection by default. */
fdk_result fdk_list_create(fdk_widget *parent, fdk_font *font,
                           fdk_widget **out_list);

/* Switches modes; clears the selection. */
void fdk_list_set_selection_mode(fdk_widget *list,
                                 fdk_list_selection_mode mode);

/* 1.4.0 — the row height rows are actually placed at (the theme's
 * list_row_height floor or the font's metrics, whichever is taller;
 * 20 for a fontless list). For mouse-to-row math over a list and
 * "how many rows fit" planning. 0 for a non-list. */
fdk_i32 fdk_list_get_row_height(fdk_widget *list);

/* Row CRUD. append writes the new row's index to *out_index when
 * non-NULL. Row text is copied. */
fdk_result fdk_list_append(fdk_widget *list, const char *text,
                           size_t *out_index);
/* Inserts before `index` (== count appends). */
fdk_result fdk_list_insert(fdk_widget *list, size_t index,
                           const char *text);
/* Removes the row; later rows shift down. */
fdk_result fdk_list_remove(fdk_widget *list, size_t index);
/* Removes every row. */
void fdk_list_clear(fdk_widget *list);
/* The number of rows. */
size_t fdk_list_row_count(fdk_widget *list);
const char *fdk_list_row_text(fdk_widget *list, size_t row);
/* Replaces the row text (copied). */
fdk_result fdk_list_set_row_text(fdk_widget *list, size_t row,
                                 const char *text);

/* Selection queries and programmatic select (which follows the
 * current mode: cleared-then-one, honoring MULTIPLE's additive
 * contract). */
fdk_i64 fdk_list_get_selected(fdk_widget *list);
/* Whether the row is selected. */
bool fdk_list_is_selected(fdk_widget *list, size_t row);
/* How many rows are selected. */
size_t fdk_list_selected_count(fdk_widget *list);
/* The position-th selected row, in row order. */
fdk_result fdk_list_selected_at(fdk_widget *list, size_t position,
                                size_t *out_row);
/* Programmatic select per the current mode. */
fdk_result fdk_list_select(fdk_widget *list, size_t row);
/* Clears every selected row (any mode). Fires on_selection_changed
 * once when anything was actually selected — the List's counterpart
 * of fdk_tree_select(FDK_TREE_NODE_NONE) / fdk_combo_set_active(-1). */
void fdk_list_clear_selection(fdk_widget *list);

/* Fires once per settled change. */
void fdk_list_set_on_selection_changed(fdk_widget *list,
                                       fdk_list_selection_fn fn,
                                       void *user_data);

/* ---- Bulk-mutation batching (1.3.0) ----
 *
 * Between begin_batch and end_batch, row mutations (append / insert /
 * remove / clear / set_row_text) skip their per-mutation O(N)
 * relayout and their selection-changed fire. end_batch performs ONE
 * relayout at the batch's final state and fires on_selection_changed
 * at most once (only when the selection actually changed during the
 * batch). Filling a list of N rows inside a batch is O(N); without
 * one, every append re-places the whole list (O(N^2) for a fill —
 * measurable at directory-listing sizes). Batches nest
 * (depth-counted); the outermost end_batch settles. Unbalanced
 * end_batch calls are ignored. Queries (row_count, selected_count,
 * row_text, ...) stay valid at every point; only GEOMETRY settles
 * late — do not hit-test or paint a list mid-batch (a synchronous
 * fill never does). */
void fdk_list_begin_batch(fdk_widget *list);
void fdk_list_end_batch(fdk_widget *list);

/* ---- Row activation (1.2.0) ----
 *
 * The "open this" gesture: double-click on a row, or Enter on the
 * keyboard cursor's row, fires the callback once with that row's
 * index. Selection changes still flow through on_selection_changed
 * exactly as before — activation is an additional, higher-level
 * signal (the file manager enters a directory on it; a file dialog
 * accepts on it). */
typedef void (*fdk_list_row_activate_fn)(fdk_widget *list, size_t row,
                                         void *user_data);

void fdk_list_set_on_row_activate(fdk_widget *list,
                                  fdk_list_row_activate_fn fn,
                                  void *user_data);

/* ---- Single-click activation (1.4.4) --------------------------------
 *
 * The sidebar rhythm: when enabled, a single left PRESS on a row
 * fires on_row_activate at once (GtkListBox's
 * activate-on-single-click) — one click navigates a places sidebar.
 * The double-click branch stays quiet in this mode (the first
 * press already fired; a doubled click must not fire twice). Off
 * by default: file rows keep the classic double-click/Enter
 * gesture. */
void fdk_list_set_activate_on_single_click(fdk_widget *list,
                                           bool single);
bool fdk_list_get_activate_on_single_click(fdk_widget *list);

/* ---- Row icons (1.4.1) ----
 *
 * A row may carry a small vector glyph (drawn by the toolkit —
 * the same font-independent stroke language as the disclosure
 * chevrons and title-bar glyphs) before its text: the symbolic
 * folder/home/drive/file set file sidebars are built from. Icons
 * are paint-only: they widen the row's content measurement by
 * 22 px (16 glyph + 6 gap) and are skipped (no shift) when NONE. */
typedef enum fdk_row_icon {
    FDK_ROW_ICON_NONE = 0,
    FDK_ROW_ICON_FOLDER,  /* the places-sidebar classic            */
    FDK_ROW_ICON_HOME,    /* house + door                          */
    FDK_ROW_ICON_DRIVE,   /* rounded slab + activity LED           */
    FDK_ROW_ICON_FILE,    /* page with folded corner               */
    FDK_ROW_ICON_RECENT,  /* clock face (1.4.4: the recents place) */
} fdk_row_icon;

/* Sets/clears the row's icon (unknown enum values are ignored as
 * NONE). Re-measures the row. Out-of-range rows are
 * FDK_ERR_INVALID_ARGUMENT. */
fdk_result fdk_list_row_set_icon(fdk_widget *list, size_t row,
                                 fdk_row_icon icon);
/* The row's icon (NONE for non-lists / out-of-range rows). */
fdk_row_icon fdk_list_row_get_icon(fdk_widget *list, size_t row);

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
 * Node handles (fdk_tree_node) index the internal node store, which
 * NEVER reuses an index: handles stay stable while their nodes are
 * alive, and a removed node's handle simply becomes invalid (every
 * OTHER handle keeps pointing at the same node). fdk_tree_clear is
 * the one exception — it resets the whole store, invalidating every
 * handle at once. FDK_TREE_NODE_NONE means "no node" (root parent,
 * no selection).
 */

typedef size_t fdk_tree_node;
#define FDK_TREE_NODE_NONE ((size_t)-1)

typedef void (*fdk_tree_selection_fn)(fdk_widget *tree,
                                      void *user_data);

/* Creates an empty tree. */
fdk_result fdk_tree_create(fdk_widget *parent, fdk_font *font,
                           fdk_widget **out_tree);

/* Adds `text` as the last child of `parent` (FDK_TREE_NODE_NONE =
 * a root-level node). Writes the new handle to *out_node when
 * non-NULL. */
fdk_result fdk_tree_node_add(fdk_widget *tree, fdk_tree_node parent,
                             const char *text,
                             fdk_tree_node *out_node);
/* Replaces the node text (copied). */
fdk_result fdk_tree_node_set_text(fdk_widget *tree,
                                  fdk_tree_node node,
                                  const char *text);
const char *fdk_tree_node_text(fdk_widget *tree, fdk_tree_node node);

/* Expand/collapse (parents only; leaves return
 * FDK_ERR_INVALID_ARGUMENT). */
/* Removes the node AND its whole subtree (the FDK container rule:
 * removing a parent takes its children). The node's handle — and
 * every handle into the removed subtree — becomes invalid; all other
 * handles stay stable (the store never reuses indices). Removing the
 * selected node (or its ancestor) clears the selection and fires
 * on_selection_changed once. */
fdk_result fdk_tree_node_remove(fdk_widget *tree, fdk_tree_node node);
/* Removes every node and RESETS the store: every handle from before
 * the clear is invalid (the empty tree hands out fresh indices from
 * zero again). Fires on_selection_changed once when something was
 * selected. */
void fdk_tree_clear(fdk_widget *tree);
fdk_result fdk_tree_node_expand(fdk_widget *tree, fdk_tree_node node,
                                bool expanded);
/* Expansion state (leaves report false). */
bool fdk_tree_node_is_expanded(fdk_widget *tree,
                               fdk_tree_node node);
/* Direct children of the node. */
size_t fdk_tree_node_child_count(fdk_widget *tree,
                                 fdk_tree_node node);

fdk_tree_node fdk_tree_get_selected(fdk_widget *tree);
/* Selects the node (NODE_NONE clears). */
fdk_result fdk_tree_select(fdk_widget *tree, fdk_tree_node node);
/* Rows a walk would visit right now. */
size_t fdk_tree_visible_count(fdk_widget *tree);

/* Fires on selection changes. */
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

/* Creates the slider over [min, max] at value. */
fdk_result fdk_slider_create(fdk_widget *parent, double min,
                             double max, double value,
                             fdk_widget **out_slider);
/* Re-ranges; the value clamps into the new interval. */
void fdk_slider_set_range(fdk_widget *slider, double min, double max);
/* Quantizes value changes (0 = continuous, the default). */
void fdk_slider_set_step(fdk_widget *slider, double step);
/* Sets (clamped, quantized); fires on_changed. */
void fdk_slider_set_value(fdk_widget *slider, double value);
/* The current value. */
double fdk_slider_get_value(fdk_widget *slider);
/* Fires on every value change. */
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

/* Creates the spin over [min, max] at value. */
fdk_result fdk_spin_create(fdk_widget *parent, fdk_font *font,
                           double min, double max, double value,
                           fdk_widget **out_spin);
/* Re-ranges; the value clamps into the new interval. */
void fdk_spin_set_range(fdk_widget *spin, double min, double max);
/* Stepper/keyboard step (default 1). */
void fdk_spin_set_step(fdk_widget *spin, double step);
/* Commits (clamped); rewrites the buffer. */
void fdk_spin_set_value(fdk_widget *spin, double value);
/* The last committed value. */
double fdk_spin_get_value(fdk_widget *spin);
/* The raw buffer (delegated to the embedded entry). */
const char *fdk_spin_get_text(fdk_widget *spin);
/* Fires on every commit. */
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
/* Appends a flat button; out_button may be NULL. */
fdk_result fdk_toolbar_add_button(fdk_widget *toolbar,
                                  const char *text,
                                  fdk_button_activate_fn on_activate,
                                  void *user_data,
                                  fdk_widget **out_button);
/* Appends a separator. */
fdk_result fdk_toolbar_add_separator(fdk_widget *toolbar);

/* ---- Notebook / TabView (Phase 9) ----
 *
 * A tab strip over a page area. Pages are ordinary widgets the
 * notebook ADOPTS (append_page reparents; exactly one visible at a
 * time — invisible pages are input-transparent and skipped by the
 * paint walk). Tab clicks switch; the switch callback fires after
 * the switch settles. Pages can be removed (remove_page destroys the
 * page widget — the notebook owns what it adopts, like every FDK
 * container). Tab DRAG REORDERING ships since 1.4.4 (see
 * fdk_notebook_reorder_page); close-button chrome stays parked. */

typedef void (*fdk_notebook_switch_fn)(fdk_widget *notebook,
                                       size_t page, void *user_data);

/* Fired once after a DRAG settles (or a programmatic reorder_page)
 * with the page's ORIGINAL and FINAL slot indices. Not fired for a
 * press that never crossed the drag threshold. */
typedef void (*fdk_notebook_reorder_fn)(fdk_widget *notebook,
                                        size_t from_index,
                                        size_t to_index, void *user_data);

/* Creates an empty notebook. */
fdk_result fdk_notebook_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_notebook);
/* Adopts the page (reparented); first page becomes current. */
fdk_result fdk_notebook_append_page(fdk_widget *notebook,
                                    fdk_widget *page,
                                    const char *label);
/* The number of pages. */
size_t fdk_notebook_page_count(fdk_widget *notebook);
/* Switches (clamped, no-op when unchanged). */
fdk_result fdk_notebook_set_current_page(fdk_widget *notebook,
                                         size_t index);
/* The current page index (0 when empty). */
size_t fdk_notebook_get_current_page(fdk_widget *notebook);
fdk_widget *fdk_notebook_get_page(fdk_widget *notebook,
                                  size_t index);
/* The page's tab label (toolkit-owned; valid until the page is
 * removed or the notebook destroyed). NULL on a bad index. */
const char *fdk_notebook_page_label(fdk_widget *notebook,
                                    size_t index);
/* Removes the page at `index` — the tab disappears and the PAGE
 * WIDGET IS DESTROYED (the notebook owns its pages: append_page
 * reparented them in, and removal is the standard FDK
 * parent-owns-children teardown, like fdk_list_remove). Removing the
 * current page switches to the page that shifted into its slot (the
 * next page, or the last when removing the tail) and fires
 * on_switch; removing an earlier page keeps the current page shown.
 * An empty notebook afterwards is legal (page_count 0,
 * get_current_page 0). */
fdk_result fdk_notebook_remove_page(fdk_widget *notebook,
                                    size_t index);
/* Fires after a switch settles. */
void fdk_notebook_set_on_switch(fdk_widget *notebook,
                                fdk_notebook_switch_fn fn,
                                void *user_data);

/* ---- Tab reordering (1.4.4) --------------------------------------
 *
 * Tabs reorder by drag: press a tab (it switches pages, the existing
 * behavior), move past ~4 px and the tab FOLLOWS THE POINTER — the
 * strip's other tabs swap LIVE as the dragged tab's center crosses
 * their midpoints (the model order changes during the drag; the
 * dragged tab stays glued to the pointer by the swap-time offset
 * correction). Releasing settles the tab into its final slot; the
 * shown PAGE never changes (the current page keeps its identity —
 * only its index moves with it). The drag stays inside the strip: a
 * pointer far outside clamps to the strip's ends (tab tearing is
 * deliberately not a thing — pages are adopted children).
 *
 * The programmatic twin of the gesture: move the page at `index` to
 * `new_index` (new_index clamps to the page count; a no-op returns
 * OK). The current page stays CURRENT (the identity rule above),
 * the switch callback does NOT fire, the reorder callback does, and
 * the a11y tab list re-describes from the new order. */
fdk_result fdk_notebook_reorder_page(fdk_widget *notebook,
                                     size_t index, size_t new_index);
/* The page's current slot (the page-count when absent/foreign). */
size_t fdk_notebook_page_index(fdk_widget *notebook,
                               const fdk_widget *page);
/* Fired after a drag settles or a programmatic reorder (from/to are
 * the ORIGINAL and FINAL slot indices of the moved page). */
void fdk_notebook_set_on_page_reordered(fdk_widget *notebook,
                                        fdk_notebook_reorder_fn fn,
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

/* Creates the canvas with its paint callback (may be NULL). */
fdk_result fdk_canvas_create(fdk_widget *parent,
                             fdk_canvas_paint_fn on_paint,
                             void *user_data,
                             fdk_widget **out_canvas);
/* Replaces the callback (NULL = blank). */
void fdk_canvas_set_paint_callback(fdk_widget *canvas,
                                   fdk_canvas_paint_fn on_paint,
                                   void *user_data);
/* Requests a repaint of the whole canvas. */
void fdk_canvas_invalidate(fdk_widget *canvas);

/* ---- Menu (Phase 9) ----
 *
 * A menu MODEL (fdk_menu: an item list, no presentation) plus the
 * two presentations built on it: a MenuBar widget (dropdowns under
 * clickable titles) and context menus (fdk_menu_popup_at pops any
 * menu at a widget-relative position). ComboBox dropdowns ride the
 * same machinery.
 *
 * The model is APPLICATION-OWNED: create it, attach submenus
 * (fdk_menu_item_set_submenu borrows — the submenu is not owned),
 * hand it to a MenuBar or pop it, and destroy it with
 * fdk_menu_destroy when you are done (after destroying the bar).
 * Items are stable handles for the menu's lifetime.
 *
 * Presentation contract: popup windows are TOOLKIT-OWNED — created,
 * painted (auto-paint on every event's damage), grabbed, and
 * destroyed by FDK. The application's loop only pumps events; it
 * never learns these windows exist. Dismissal is universal: click
 * outside, Escape (one level per press in a submenu chain), or
 * activating an item closes the chain.
 *
 * Items: NORMAL activates and closes; SEPARATOR is a rule; CHECK
 * toggles its checked state (vector-glyph checkmark) and closes;
 * RADIO checks itself and unchecks the OTHER radios of the same
 * menu (all radios in one menu are one group) and closes. Disabled
 * items highlight nothing and swallow their clicks.
 *
 * Keyboard (delivered to the open menu via the popup's input grab):
 * Up/Down/Home/End move the cursor skipping separators and disabled
 * rows, Enter/Space activate, Right opens the cursor row's submenu
 * (or walks to the next bar menu at the top level), Left closes the
 * current submenu (or walks to the previous bar menu), Escape
 * closes one level. The bar itself, when focused, walks titles with
 * Left/Right and opens with Down/Enter.
 *
 * ACCELERATORS (1.3.3): an item's `shortcut` string ("Ctrl+S" —
 * see fdk_shortcut_parse's grammar) is not just the right-aligned
 * label: for as long as the item's menu hangs under a window's
 * MENU BAR, the binding is LIVE. The window's KEY_DOWN dispatch
 * scans its tree's bars (and their submenus, recursively) on every
 * keypress and activates a matching ENABLED item exactly as if it
 * had been clicked — check/radio state flips, callbacks fire, no
 * menu opens. The scan is live: appending, retitling, re-shortcut
 * or destroying a menu takes effect on the very next keypress,
 * because nothing is cached. Items of CONTEXT menus (popped via
 * fdk_menu_popup_at) have no persistent presence in a window tree
 * and therefore no global accelerator — their shortcut strings are
 * display-only, and applications wanting both register
 * fdk_window_add_shortcut alongside. Application shortcuts run
 * BEFORE bar accelerators when both match (see fdk_window.h).
 *
 * Mnemonics (1.3.6): label text may carry "&X" markers — the next
 * character becomes the item's (or bar title's) underlined mnemonic
 * letter. Alt+X opens the matching bar title; with a menu open, X
 * alone (or Alt+X) activates the matching item — first
 * enabled match wins, deterministically. "&&" renders a literal
 * '&'; a dangling '&' is literal; the first marker in a label wins.
 * Matching is case-insensitive on the letter. Explicit
 * accelerators beat a same-key title mnemonic (dispatch order).
 * fdk_menu_item_text() returns the DISPLAY string (markers
 * stripped); fdk_menu_item_get_mnemonic() queries the letter.
 *
 * Honest v1 limits, documented rather than faked: menus do not
 * scroll (a menu taller than the screen clips at the screen edge);
 * hover-to-switch bar titles only reacts where the platform's popup
 * grab reports out-of-bounds motion (X11: yes; Wayland: compositor
 * dependent); duplicate mnemonic letters within one menu do not
 * cycle (the first enabled match activates — the app owns
 * uniqueness). */

typedef struct fdk_menu fdk_menu;
typedef struct fdk_menu_item fdk_menu_item;

typedef enum fdk_menu_item_type {
    FDK_MENU_ITEM_NORMAL = 0,
    FDK_MENU_ITEM_SEPARATOR = 1,
    FDK_MENU_ITEM_CHECK = 2,
    FDK_MENU_ITEM_RADIO = 3,
} fdk_menu_item_type;

/* Fires when the item activates (click or keyboard). Runs AFTER the
 * popup chain has closed and any check/radio state has flipped. */
typedef void (*fdk_menu_activate_fn)(fdk_menu_item *item,
                                     void *user_data);

/* Creates an empty menu model (no widget yet). */
fdk_result fdk_menu_create(fdk_font *font, fdk_menu **out_menu);
/* Destroys the model (after any bar using it). */
void fdk_menu_destroy(fdk_menu *menu);
/* The number of rows (separators included). */
size_t fdk_menu_item_count(fdk_menu *menu);

/* Appends a normal item. */
fdk_result fdk_menu_append(fdk_menu *menu, const char *text,
                           fdk_menu_item **out_item);
/* Appends a separator row. */
fdk_result fdk_menu_append_separator(fdk_menu *menu);
/* Appends a check item at the given state. */
fdk_result fdk_menu_append_check(fdk_menu *menu, const char *text,
                                 bool checked, fdk_menu_item **out_item);
/* Appends a radio item at the given state. */
fdk_result fdk_menu_append_radio(fdk_menu *menu, const char *text,
                                 bool checked, fdk_menu_item **out_item);

/* Replaces the label (copied; NULL clears). */
fdk_result fdk_menu_item_set_text(fdk_menu_item *item, const char *text);
const char *fdk_menu_item_text(fdk_menu_item *item);

/* The item's mnemonic letter (lowercased), 0 when the label carries
 * no "&X" marker. Set through the label at append/set_text time. */
fdk_u32 fdk_menu_item_get_mnemonic(fdk_menu_item *item);
fdk_menu_item_type fdk_menu_item_get_type(fdk_menu_item *item);
/* Disabled items dim and refuse activation. */
void fdk_menu_item_set_enabled(fdk_menu_item *item, bool enabled);
/* The item's enabled flag. */
bool fdk_menu_item_is_enabled(fdk_menu_item *item);
/* Sets the check/radio state (no-op on other kinds). */
void fdk_menu_item_set_checked(fdk_menu_item *item, bool checked);
/* The current state (false on other kinds). */
bool fdk_menu_item_is_checked(fdk_menu_item *item);
/* The item's shortcut spec — the string last set (NULL when none).
 * While the menu hangs under a window's menu bar this is a LIVE
 * ACCELERATOR (see the contract block above); the label is drawn
 * right-aligned in the row. Grammar: fdk_shortcut_parse. */
fdk_result fdk_menu_item_set_shortcut(fdk_menu_item *item,
                                      const char *shortcut);
/* The current shortcut spec (NULL when none). */
const char *fdk_menu_item_get_shortcut(fdk_menu_item *item);

/* Attaches `submenu` (borrowed — not owned, not destroyed with the
 * item), replacing any previous one. The submenu opens on hover /
 * Right / click of this item. Only NORMAL items take submenus.
 * Refuses with FDK_ERR_INVALID_ARGUMENT when the wiring would
 * create a CYCLE (the submenu already reaches this item's own menu
 * by any chain of submenus) — an infinite menu tree would hang
 * every model walker, so the setter is where it stops. */
fdk_result fdk_menu_item_set_submenu(fdk_menu_item *item,
                                     fdk_menu *submenu);
/* The item's current submenu (NULL when none). */
fdk_menu *fdk_menu_item_get_submenu(fdk_menu_item *item);

/* Per-item activation callback (fires for this item only). */
void fdk_menu_item_set_on_activate(fdk_menu_item *item,
                                   fdk_menu_activate_fn on_activate,
                                   void *user_data);
/* Menu-wide fallback: fires for any item without its own callback. */
void fdk_menu_set_on_activate(fdk_menu *menu,
                              fdk_menu_activate_fn on_activate,
                              void *user_data);

/* ---- MenuBar ----
 *
 * A horizontal bar of menu titles. Clicking (or focusing and
 * pressing Down/Enter on) a title opens its menu below it as a
 * toolkit-owned popup chain. The bar is a widget: it lays its
 * titles out, paints its chrome, and follows the theme
 * (menu_item_height metric, like every menu row).
 *
 * Menus are BORROWED: destroying a menu still attached to a live
 * bar is a use-after-free — destroy menus after the bar (the
 * conventional order). */

fdk_result fdk_menu_bar_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_bar);
/* Appends a title bound to `menu` (borrowed). */
fdk_result fdk_menu_bar_append(fdk_widget *bar, const char *title,
                               fdk_menu *menu);
/* The number of titles. */
size_t fdk_menu_bar_count(fdk_widget *bar);
/* Removes the title (closing its open menu first). */
fdk_result fdk_menu_bar_remove(fdk_widget *bar, size_t index);
/* Closes any open chain (as if Escape-dismissed from the top). */
void fdk_menu_bar_close(fdk_widget *bar);

/* ---- Context menu ----
 *
 * Pops `menu` at (x, y) relative to `anchor`'s top-left (the widget
 * must live in a window's tree — standalone trees have no window to
 * anchor a popup to). Dismissal and keyboard behavior as above.
 * The menu stays yours: destroy it whenever the popup is closed
 * (activating an item closes it synchronously, so after the
 * on_activate callback returns, destroying the menu is safe). */
fdk_result fdk_menu_popup_at(fdk_menu *menu, fdk_widget *anchor,
                             fdk_i32 x, fdk_i32 y);

/* ---- ComboBox (Phase 9) ----
 *
 * A one-of-many selector: a button-like field showing the active
 * row plus a dropdown chevron; clicking opens the item list as a
 * toolkit-owned popup (same machinery, dismissal, and auto-paint
 * as menus). Choosing a row makes it active and fires on_changed.
 *
 * Editable mode swaps the field for an embedded Entry (full text
 * editing, selection, clipboard): typing makes the state "custom"
 * (active index cleared, on_changed fires with FDK_COMBO_NONE),
 * and picking from the list rewrites the buffer. The dropdown
 * button (chevron) stays interactive either way. Entry completion
 * (filtering the list while typing) is parked honestly.
 *
 * The dropdown list sizes its rows like menu rows (theme
 * menu_item_height metric) and is at least as wide as the combo. */

#define FDK_COMBO_NONE ((size_t)-1)

typedef void (*fdk_combo_changed_fn)(fdk_widget *combo, size_t index,
                                     void *user_data);

/* Creates an empty, non-editable combo. */
fdk_result fdk_combo_create(fdk_widget *parent, fdk_font *font,
                            fdk_widget **out_combo);

/* Appends an option (copied). */
fdk_result fdk_combo_append(fdk_widget *combo, const char *text,
                            size_t *out_index);
/* Removes the option; active adjusts (removing it clears). */
fdk_result fdk_combo_remove(fdk_widget *combo, size_t index);
/* Removes every option. */
void fdk_combo_clear(fdk_widget *combo);
/* The number of options. */
size_t fdk_combo_count(fdk_widget *combo);
const char *fdk_combo_text(fdk_widget *combo, size_t index);

/* The active index (-1 = none). */
fdk_i64 fdk_combo_get_active(fdk_widget *combo); /* -1 = none */
/* -1 clears the active row (no on_changed for a no-op). */
fdk_result fdk_combo_set_active(fdk_widget *combo, fdk_i64 index);
/* The active row's text, or the Entry buffer when editable/custom;
 * never NULL; valid until the next change. */
const char *fdk_combo_active_text(fdk_widget *combo);
/* Editable combos embed a text Entry. */
void fdk_combo_set_editable(fdk_widget *combo, bool editable);

/* Fires when the active option changes. */
void fdk_combo_set_on_changed(fdk_widget *combo,
                              fdk_combo_changed_fn on_changed,
                              void *user_data);

/* ---- Spinner (1.4.1) ----
 *
 * A busy indicator (GTK's GtkSpinner, Qt's busy QProgressBar): a
 * continuously rotating arc that says "work is happening, no
 * fraction exists" WITHOUT occupying a bar's width. Natural size
 * 24x24 (it is square; layout stretches it like any widget, and
 * the arc scales with the bounds).
 *
 * start()/stop() drive the rotation on the timer clock (~1.2 s per
 * turn) exactly like the indeterminate ProgressBar: the animation
 * needs a window's event loop, so a detached/standalone tree shows
 * a static arc (honest about being busy without the rotation the
 * headless world has no clock for). The a11y value interface
 * reports "busy"/"idle" and the BUSY state flag while spinning.
 * Not focusable, not interactive — an indicator, not a control. */
fdk_result fdk_spinner_create(fdk_widget *parent,
                              fdk_widget **out_spinner);
/* Starts the rotation (idempotent). */
void fdk_spinner_start(fdk_widget *spinner);
/* Stops it; the arc parks where it stopped (GTK semantics — the
 * phase is not reset, restarting continues from the parked angle). */
void fdk_spinner_stop(fdk_widget *spinner);
/* Whether the rotation is active. */
bool fdk_spinner_is_spinning(fdk_widget *spinner);

/* ---- Paned (1.4.1) ----
 *
 * The two-pane splitter (GTK's GtkPaned, Qt's QSplitter): a
 * container whose FIRST child fills pane 1 and SECOND child fills
 * pane 2, separated by a draggable divider. Further children are
 * refused (FDK_ERR_INVALID_ARGUMENT from fdk_widget_create) — the
 * paned IS the layout.
 *
 * POSITION semantics. Unset (the default, and after
 * fdk_paned_unset_position): each pane gets its child's natural
 * size, the leftover is split evenly (a shrink-wrapped start that
 * fills the slot). set_position(x) pins the divider x pixels from
 * the pane-1 edge, clamped so the divider itself stays visible
 * ([0, extent - divider]); EITHER pane may be squeezed to zero —
 * GTK-style freedom (give a child a minimum via its own natural
 * size and app-side policy). Dragging and arrow keys move a pinned
 * position. The position is in paned-local pixels and
 * survives resizes proportionally ONLY through re-clamping — the
 * offset is absolute, not a ratio (GTK parity).
 *
 * The divider is keyboard-operable when the paned is focused:
 * Left/Up step toward pane 1, Right/Down toward pane 2 (8 px),
 * Home/End jump to the extremes. The a11y interface: SPLIT_PANE
 * role; the value interface reports the divider offset in pixels. */

/* The divider's visual width (also the drag band's extent). */
#define FDK_PANED_DIVIDER 6

fdk_result fdk_paned_create(fdk_widget *parent,
                            fdk_orientation orientation,
                            fdk_widget **out_paned);
/* Pins the divider at `position` px from the pane-1 edge (clamped
 * to [0, extent - FDK_PANED_DIVIDER] along the orientation).
 * Re-arranges and repaints. A negative position is refused with
 * FDK_ERR_INVALID_ARGUMENT — use unset_position for "auto". */
fdk_result fdk_paned_set_position(fdk_widget *paned, fdk_i32 position);
/* The current divider offset (0 when unset — see
 * fdk_paned_position_is_set to distinguish). */
fdk_i32 fdk_paned_get_position(fdk_widget *paned);
/* Whether a position is pinned (false = auto). */
bool fdk_paned_position_is_set(fdk_widget *paned);
/* Returns to the auto split (natural sizes + even leftover). */
void fdk_paned_unset_position(fdk_widget *paned);

/* ---- Expander (1.4.1) ----
 *
 * The disclosure section (GTK's GtkExpander, Qt's collapsible
 * group): a header row (rotating disclosure triangle + label) that
 * reveals or collapses its FIRST child. Further children are
 * refused like Paned's. The reveal is ANIMATED (the 1.3.8 animator,
 * ~160 ms) when a window clock is running; standalone trees toggle
 * instantly — the same headless-honesty rule as every animation.
 *
 * Activation: click anywhere on the header, or Space/Enter while
 * focused (the header is the focusable; the content child keeps
 * its own focusability). The a11y interface: EXPANDER role + the
 * EXPANDED state, name = the label. */

fdk_result fdk_expander_create(fdk_widget *parent, fdk_font *font,
                               const char *label,
                               fdk_widget **out_expander);
/* Replaces the label (copied; NULL clears). Re-measures. */
fdk_result fdk_expander_set_label(fdk_widget *expander,
                                  const char *label);
/* The current label (toolkit-owned; NULL when none). */
const char *fdk_expander_get_label(fdk_widget *expander);
/* Expands/collapses (animated reveal when a clock exists; fires
 * on_expanded after the state flips). */
void fdk_expander_set_expanded(fdk_widget *expander, bool expanded);
/* The current state. */
bool fdk_expander_is_expanded(fdk_widget *expander);

typedef void (*fdk_expander_changed_fn)(fdk_widget *expander,
                                        bool expanded, void *user_data);
/* Fires after every state change (user toggle or programmatic). */
void fdk_expander_set_on_changed(fdk_widget *expander,
                                 fdk_expander_changed_fn on_changed,
                                 void *user_data);

/* ---- Statusbar (1.4.2) ----
 *
 * GTK's GtkStatusbar: the bottom-edge strip for transient
 * application state. The API is the classic STACK WITH CONTEXTS:
 * get_context_id mints a stable id per context string (per
 * statusbar); push(ctx, text) stacks a message and returns its
 * message handle; the bar paints the TOP of the stack. pop(ctx)
 * removes the topmost message OF THAT CONTEXT (LIFO per context —
 * others' messages survive); remove(ctx, id) removes one exact
 * message wherever it sits (the "take that back" that stays
 * precise when other contexts pushed in between).
 *
 * An indicator: no focus, no events, no children. The a11y face is
 * the STATUS_BAR role with the current message as the name. */

fdk_result fdk_statusbar_create(fdk_widget *parent, fdk_font *font,
                                fdk_widget **out_statusbar);
/* Mints (or returns) the context id for `context`. 0 on invalid
 * input (NULL/empty) or OOM — push/remove refuse that id. Ids are
 * stable per statusbar instance, sequential from 1. */
fdk_u32 fdk_statusbar_get_context_id(fdk_widget *statusbar,
                                     const char *context);
/* Stacks `text` under `context_id`; returns the message handle (0
 * on refusal — bad id, NULL text, OOM). The top of the stack
 * paints. */
fdk_u32 fdk_statusbar_push(fdk_widget *statusbar, fdk_u32 context_id,
                           const char *text);
/* Removes the topmost message of `context_id` (a no-op when that
 * context has nothing stacked — GTK parity). */
void fdk_statusbar_pop(fdk_widget *statusbar, fdk_u32 context_id);
/* Removes the exact message `message_id` under `context_id`
 * wherever it sits in the stack. */
void fdk_statusbar_remove(fdk_widget *statusbar, fdk_u32 context_id,
                          fdk_u32 message_id);
/* The current (top-of-stack) message, NULL when the stack is
 * empty. Toolkit-owned. */
const char *fdk_statusbar_get_text(fdk_widget *statusbar);

/* ---- SearchEntry (1.4.2) ----
 *
 * GTK's GtkSearchEntry as an Entry PRESET: fdk_search_entry_create
 * returns a regular Entry (every Entry API applies) with the search
 * mode on — a magnifier glyph before the text, a clear button that
 * appears only when there is text to clear, and the search Esc
 * ladder: the FIRST Escape clears the text (consumed); a further
 * Escape bubbles to the window (a dialog's Cancel still works).
 * The on_changed callback fires per keystroke as usual — the
 * GtkSearchEntry-style debounced "search-changed" is deliberately
 * parked (apps debounce at the source; the toolkit guessing a
 * delay has never aged well). */

fdk_result fdk_search_entry_create(fdk_widget *parent, fdk_font *font,
                                   fdk_widget **out_entry);
/* Whether the entry carries the search preset. */
bool fdk_entry_is_search(fdk_widget *entry);

/* ---- LevelBar (1.4.2) ----
 *
 * GTK's GtkLevelBar / the battery-and-volume meter: a value in
 * [min, max] as a row of discrete blocks (DISCRETE, the default — 5
 * segments) or one continuous bar (CONTINUOUS). A READOUT, not a
 * control: no focus, no events — the value arrives via
 * fdk_levelbar_set_value from the app's own state. The a11y face is
 * the LEVEL_BAR role with the value interface (a meter). */

typedef enum fdk_levelbar_mode {
    FDK_LEVELBAR_DISCRETE = 0,    /* N blocks, lit below the value   */
    FDK_LEVELBAR_CONTINUOUS = 1,  /* one accent run over the track   */
} fdk_levelbar_mode;

fdk_result fdk_levelbar_create(fdk_widget *parent, double min,
                               double max, fdk_widget **out_levelbar);
void fdk_levelbar_set_value(fdk_widget *levelbar, double value);
double fdk_levelbar_get_value(fdk_widget *levelbar);
/* max < min is refused (create documents it with
 * FDK_ERR_INVALID_ARGUMENT; the setter is void and warns). */
void fdk_levelbar_set_range(fdk_widget *levelbar, double min,
                            double max);
void fdk_levelbar_set_mode(fdk_widget *levelbar,
                           fdk_levelbar_mode mode);
fdk_levelbar_mode fdk_levelbar_get_mode(fdk_widget *levelbar);
/* DISCRETE mode only: the block count (default 5; values outside
 * [2, 32] are refused). */
void fdk_levelbar_set_segments(fdk_widget *levelbar, size_t segments);
size_t fdk_levelbar_get_segments(fdk_widget *levelbar);

/* ---- Revealer (1.4.2) ----
 *
 * GTK's GtkRevealer: shows/hides its ONE child (a second child is
 * refused) with a sliding animation instead of a pop. The
 * Expander's mechanics generalized: the door SELF-CROPS (the
 * revealer's own bounds track the reveal, so the clip stack does
 * the cropping with no parent cooperation), the child stays at its
 * FULL natural size (the reveal is a clip, never a squeeze), and a
 * fully hidden child is invisible exactly as if it were not there
 * (which is also the revealer's measured size: 0x0).
 *
 * Transitions: SLIDE_DOWN (default), SLIDE_UP, SLIDE_LEFT,
 * SLIDE_RIGHT, NONE (instant). DOWN/RIGHT anchor the origin;
 * UP/LEFT anchor the far edge (a toast pinned to a window's bottom
 * rises in place). Under a packing container the parent re-asserts
 * the slot origin per tick, so box-parented revealers grow into
 * their slot regardless of direction — the edge anchoring is what
 * plain-parented, hand-positioned revealers get. CROSSFADE is
 * deliberately absent (the paint walk is a clip-stack compositor,
 * not an alpha scene graph — an honest slide, not a half-faked
 * fade).
 *
 * The flight is ~160 ms cubic-out (settable) on the 1.3.8
 * animator; standalone trees snap (the headless-honesty rule).
 * on_revealed fires when the flight LANDS (either direction) —
 * the natural point for chained teardown. */

typedef enum fdk_revealer_transition {
    FDK_REVEAL_NONE = 0,
    FDK_REVEAL_SLIDE_DOWN = 1,
    FDK_REVEAL_SLIDE_UP = 2,
    FDK_REVEAL_SLIDE_LEFT = 3,
    FDK_REVEAL_SLIDE_RIGHT = 4,
    /* 1.4.3 — the deferred compositor-level alpha decision, shipped
     * via the paint-group machinery: the door keeps the child's FULL
     * geometry at every reveal value (no crop — a fade is opacity,
     * not size), and the revealer's subtree composites through a
     * cached ARGB offscreen with a global-alpha source-over blit.
     * A REAL per-pixel fade, not a half-faked one; at rest (reveal
     * 0 or 1) the group is bypassed and the plain walk runs free. */
    FDK_REVEAL_CROSSFADE = 5,
} fdk_revealer_transition;

fdk_result fdk_revealer_create(fdk_widget *parent,
                               fdk_widget **out_revealer);
/* The target state (animated when a window clock exists). */
void fdk_revealer_set_reveal_child(fdk_widget *revealer, bool reveal);
/* The TARGET (what was last requested). */
bool fdk_revealer_get_reveal_child(fdk_widget *revealer);
/* Where the flight actually is: true only once the child is fully
 * revealed (GTK's child-revealed distinction — chains that tear
 * down on "fully hidden" read THIS). */
bool fdk_revealer_get_child_revealed(fdk_widget *revealer);
/* Refused mid-flight (the blend's axis would change under the
 * running tick); NONE lands instantly. */
void fdk_revealer_set_transition(fdk_widget *revealer,
                                 fdk_revealer_transition transition);
fdk_revealer_transition fdk_revealer_get_transition(
    fdk_widget *revealer);
/* Flight duration in ms (default 160; 0 = snap; > 10000 refused). */
void fdk_revealer_set_transition_duration(fdk_widget *revealer,
                                          fdk_u32 duration_ms);
fdk_u32 fdk_revealer_get_transition_duration(fdk_widget *revealer);

typedef void (*fdk_revealer_revealed_fn)(fdk_widget *revealer,
                                         bool revealed, void *user_data);
/* Fires when the flight lands (either direction; also on the
 * duration-0 snap). */
void fdk_revealer_set_on_revealed(fdk_widget *revealer,
                                  fdk_revealer_revealed_fn on_revealed,
                                  void *user_data);

/* ---- Stack + StackSwitcher (1.4.2) ----
 *
 * GTK's GtkStack: NAMED pages, exactly one visible — the structure
 * settings dialogs and master-detail panes are built on. add()
 * reparents the page in (the parent-owns-children model; remove
 * DESTROYS the page, like the notebook's). Names are unique keys
 * (duplicate/NULL refused); titles are display strings the
 * StackSwitcher reads. Page switches are instant by v1 policy (no
 * crossfade — the revealer's honesty note covers why). The natural
 * size is the MAX over pages (floored 40x40), so flips never
 * resize the window.
 *
 * The StackSwitcher is the switching surface: a row of rounded
 * pills bound to the stack's pages — active pill accent-filled
 * (a checked semantic, it snaps), inactive pills hover-faded. The
 * binding is loosely coupled: a stack destroyed by another hand
 * cleanly unbinds the switcher (it never dereferences a dangling
 * pointer), and the switcher does NOT occupy the stack's
 * on_changed slot (it reconciles at paint time).
 *
 * The a11y split: the Stack is a PANEL named by the current page;
 * the Switcher is the TAB_LIST with one virtual TAB per pill
 * (ACTIVATE switches — the notebook's pattern). */

fdk_result fdk_stack_create(fdk_widget *parent,
                            fdk_widget **out_stack);
/* Reparents `child` in as a new page. `name` is the unique key
 * (duplicate or NULL/empty refused); `title` (NULL falls back to
 * the name) is the display string. The first page becomes
 * visible. */
fdk_result fdk_stack_add(fdk_widget *stack, fdk_widget *child,
                         const char *name, const char *title);
size_t fdk_stack_page_count(fdk_widget *stack);
fdk_widget *fdk_stack_get_page(fdk_widget *stack, size_t index);
fdk_widget *fdk_stack_get_child_by_name(fdk_widget *stack,
                                        const char *name);
const char *fdk_stack_page_name(fdk_widget *stack, size_t index);
const char *fdk_stack_page_title(fdk_widget *stack, size_t index);
/* NULL/empty title restores the name fallback. */
fdk_result fdk_stack_set_page_title(fdk_widget *stack, size_t index,
                                    const char *title);
/* Switch by key; FDK_ERR_NOT_FOUND when no page carries `name`. */
fdk_result fdk_stack_set_visible_name(fdk_widget *stack,
                                      const char *name);
/* Switch by page widget; NOT_FOUND when it is not a page. */
fdk_result fdk_stack_set_visible_child(fdk_widget *stack,
                                       fdk_widget *child);
const char *fdk_stack_get_visible_name(fdk_widget *stack);
fdk_widget *fdk_stack_get_visible_child(fdk_widget *stack);
/* Removes (and destroys) the page. The survivor rule is the
 * notebook's: removing the current page shows the page that
 * shifted into its slot; removing an earlier page keeps showing
 * the same page. */
fdk_result fdk_stack_remove_page(fdk_widget *stack, size_t index);

typedef void (*fdk_stack_changed_fn)(fdk_widget *stack, size_t index,
                                     const char *name, void *user_data);
/* Fires after every visible-page change (user or programmatic,
 * including the survivor switch after removing the current page). */
void fdk_stack_set_on_changed(fdk_widget *stack,
                              fdk_stack_changed_fn on_changed,
                              void *user_data);

fdk_result fdk_stackswitcher_create(fdk_widget *parent,
                                    fdk_font *font,
                                    fdk_widget **out_switcher);
/* Binds the switcher to `stack` (NULL unbinds; a non-stack widget
 * is refused). The switcher re-reads pages, titles, and the active
 * pill at every paint — mutations need no notification. */
void fdk_stackswitcher_set_stack(fdk_widget *switcher,
                                 fdk_widget *stack);
/* The bound stack, or NULL once unbound/destroyed. */
fdk_widget *fdk_stackswitcher_get_stack(fdk_widget *switcher);

/* ---- Tree row icons (1.4.2) ----
 *
 * The List's 1.4.1 symbolic glyphs, extended to the Tree: a
 * 16-px vector glyph before the node's text (folder / home / drive
 * / page), accounted in the tree's width computation. The icon
 * rides the MODEL NODE (not the row widget), so it survives
 * collapses and re-expansions. */

fdk_result fdk_tree_node_set_icon(fdk_widget *tree,
                                  fdk_tree_node node, fdk_row_icon icon);
fdk_row_icon fdk_tree_node_get_icon(fdk_widget *tree,
                                    fdk_tree_node node);

/* ---- Slider marks (1.4.2) ----
 *
 * GTK's gtk_scale_add_mark: tick marks (and optional labels) under
 * the trough. add_mark registers a mark at `value` (clamped into
 * the range; label NULL = tick only). Marks widen the slider's
 * natural HEIGHT by one label line when any mark carries a label
 * (the track centers in the remaining extent); labels center under
 * their ticks and clip at the widget's bounds (no wrap). clear_marks
 * drops them all. */

fdk_result fdk_slider_add_mark(fdk_widget *slider, double value,
                               const char *label);
void fdk_slider_clear_marks(fdk_widget *slider);
size_t fdk_slider_mark_count(fdk_widget *slider);

/* ---- Slider orientation (1.4.3) ----
 *
 * The parked vertical remainder, shipped: a vertical slider runs
 * its trough down the widget's height (min at the BOTTOM, max at
 * the top — the audio-volume convention), thumb, fill run, and
 * value-at all rotate accordingly. Up/Right raise, Down/Left lower
 * (unchanged). Marks rotate too: ticks stroke to the LEFT of the
 * trough with their labels right-aligned against the tick; labeled
 * marks widen the natural WIDTH by one label line (the mirror of
 * the horizontal's height growth). Switching orientation keeps the
 * value and re-measures. */

typedef enum fdk_slider_orientation {
    FDK_SLIDER_HORIZONTAL = 0,  /* the default since Phase 9 */
    FDK_SLIDER_VERTICAL  = 1,
} fdk_slider_orientation;

void fdk_slider_set_orientation(fdk_widget *slider,
                                fdk_slider_orientation orientation);
fdk_slider_orientation fdk_slider_get_orientation(fdk_widget *slider);

/* ---- SearchEntry debounce (1.4.3) ----
 *
 * The search preset's debounced twin of on_changed: the
 * search-changed callback fires once, `ms` milliseconds (default
 * 250) after the LAST edit — fast typists stop re-filtering on
 * every keystroke. The clear button and the Esc ladder's text
 * clear re-arm the same debounce (the sweep ends exactly once).
 * set_debounce(0) = fire immediately on the edit; the pending
 * timer is re-armed by every edit while one is outstanding.
 * Detached trees (no window) have no timers — the callback snaps
 * to immediate, the same honesty rule animations follow. */

typedef void (*fdk_search_changed_fn)(fdk_widget *entry,
                                      const char *text,
                                      void *user_data);

void fdk_search_entry_set_on_search(fdk_widget *entry,
                                    fdk_search_changed_fn on_search,
                                    void *user_data);
void fdk_search_entry_set_debounce(fdk_widget *entry,
                                    fdk_u32 debounce_ms);
bool fdk_search_entry_has_pending(fdk_widget *entry);

/* ---- MenuButton (1.4.3) ----
 *
 * GtkMenuButton's role: a button-shaped widget that pops up an
 * ATTACHED menu model when clicked (the hamburger, the "options"
 * split-button, the toolbar overflow). The model stays YOURS —
 * borrowed for the popup only, still valid (and reusable) after
 * the chain closes; destroy it whenever the button is closed. A
 * NULL model makes the button inert (it still paints, presses,
 * and reports EXPANDED=false).
 *
 * The popup anchors at the button's bottom-left, at least as wide
 * as the button, with the same dismissal/keyboard/auto-paint
 * machinery as every popup chain. The optional vector arrow
 * (default: DOWN when a model is set) rides the label's right side
 * — the GtkMenuButton chevron language. Keyboard: Enter/Space open
 * (the Button contract); Escape/dismissal behave like any popup.
 *
 * While the chain is open the button paints pressed and reports
 * the a11y EXPANDED state; the menu machinery delivers everything
 * else (item activation is the MODEL's callbacks' business — the
 * button never sees it). */

typedef enum fdk_menu_button_arrow {
    FDK_MENU_BUTTON_ARROW_NONE = 0,  /* label only                       */
    FDK_MENU_BUTTON_ARROW_DOWN = 1,  /* the default                      */
    FDK_MENU_BUTTON_ARROW_UP   = 2,
    FDK_MENU_BUTTON_ARROW_LEFT = 3,
    FDK_MENU_BUTTON_ARROW_RIGHT = 4,
} fdk_menu_button_arrow;

fdk_result fdk_menu_button_create(fdk_widget *parent, fdk_font *font,
                                  const char *label,
                                  fdk_widget **out_button);
/* The attached model (borrowed; NULL = inert). Setting a model
 * while a popup chain is open is refused (the chain's model is
 * fixed at open time). */
fdk_result fdk_menu_button_set_menu(fdk_widget *button,
                                    fdk_menu *model);
fdk_menu *fdk_menu_button_get_menu(fdk_widget *button);
void fdk_menu_button_set_arrow(fdk_widget *button,
                               fdk_menu_button_arrow arrow);
/* True while this button's popup chain is open. */
bool fdk_menu_button_is_open(fdk_widget *button);

/* ---- Tree selection mode (1.4.3) ----
 *
 * The parked multi-select tree, shipped — mirroring the List's
 * model exactly: MULTIPLE clicks select, ctrl+click toggles one
 * node, shift+click ranges the VISIBLE sequence from the anchor,
 * ctrl+shift ranges without clearing. The rubber band extends to
 * the tree: a left press on empty tree space sweeps a band over
 * the visible rows (ctrl = union), auto-scrolling when the sweep
 * chases the viewport edge like the List's. on_selection_changed
 * fires on the sweep's press and release (one gesture), and on
 * every click/keyboard move as before. SINGLE keeps the classic
 * behavior (a ctrl/shift click in SINGLE behaves like a plain
 * click); NONE ignores selection input entirely. */

typedef enum fdk_tree_selection_mode {
    FDK_TREE_SELECTION_SINGLE   = 0, /* the default since Phase 9 */
    FDK_TREE_SELECTION_NONE     = 1,
    FDK_TREE_SELECTION_MULTIPLE = 2,
} fdk_tree_selection_mode;

void fdk_tree_set_selection_mode(fdk_widget *tree,
                                 fdk_tree_selection_mode mode);
fdk_tree_selection_mode fdk_tree_get_selection_mode(fdk_widget *tree);
/* Every currently-selected node (VISIBLE or collapsed-under —
 * the model holds the state, the walk only shows it). The caller
 * provides the array; the return is the count written (capped at
 * max_nodes). Order: model index order. */
size_t fdk_tree_get_selected_nodes(fdk_widget *tree,
                                   fdk_tree_node *out_nodes,
                                   size_t max_nodes);

#ifdef __cplusplus
}
#endif

#endif /* FDK_WIDGETS_H */
