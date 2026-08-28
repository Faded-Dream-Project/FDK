/* fdk_a11y.h — FDK accessibility abstraction (Phase 10).
 *
 * The accessibility tree IS the widget tree: every widget can be
 * described (role, accessible name, states, value, bounds) without
 * running a display, enumerated preorder through the ordinary
 * fdk_widget_parent/child_at API, observed (change notifications
 * for children/states/names/bounds/values), and DRIVEN
 * programmatically (fdk_a11y_perform — the same code paths real
 * input takes, which makes this API the UI automation seam as
 * well).
 *
 * There is deliberately NO platform dependency here: no AT-SPI, no
 * D-Bus. This header is the toolkit-side abstraction a platform
 * bridge (a future AT-SPI2 bridge process, an embedded screen
 * reader, a test driver) sits ON TOP of — exactly how the widget
 * layer is backend-neutral and the X11/Wayland backends sit under
 * it. The bridge seam is the query + notification + action API
 * below; nothing in it requires a display, so the whole layer is
 * verifiable headless.
 *
 * Roles and states follow the WAI-ARIA / ATK naming where a
 * concept exists there, so a future bridge maps 1:1.
 *
 * See docs/roadmap.md's Phase 10 entry for scope and the honest
 * list of what is NOT here yet.
 */

#ifndef FDK_A11Y_H
#define FDK_A11Y_H

#include "fdk_types.h"
#include "fdk_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Roles -------------------------------------------------------- */

typedef enum fdk_a11y_role {
    FDK_A11Y_ROLE_UNKNOWN = 0,   /* unbound or plain container       */

    /* Toplevels */
    FDK_A11Y_ROLE_WINDOW = 1,    /* a window's root widget           */
    FDK_A11Y_ROLE_DIALOG = 2,    /* a dialog window's content root   */
    FDK_A11Y_ROLE_APPLICATION = 3, /* the outermost root, if the app
                                      marks one                      */

    /* Containers */
    FDK_A11Y_ROLE_PANEL = 4,     /* generic grouping (Frame, boxes)  */
    FDK_A11Y_ROLE_SCROLL_AREA = 5,
    FDK_A11Y_ROLE_SCROLL_BAR = 6,
    FDK_A11Y_ROLE_LIST = 7,
    FDK_A11Y_ROLE_TREE = 8,
    FDK_A11Y_ROLE_TOOLBAR = 9,
    FDK_A11Y_ROLE_MENU_BAR = 10,
    FDK_A11Y_ROLE_MENU = 11,     /* a dropdown/context/popup menu    */
    FDK_A11Y_ROLE_TAB_LIST = 12,
    FDK_A11Y_ROLE_GROUP = 13,    /* a labeled group (titled Frame)   */

    /* Items */
    FDK_A11Y_ROLE_LIST_ITEM = 20,
    FDK_A11Y_ROLE_TREE_ITEM = 21,
    FDK_A11Y_ROLE_MENU_ITEM = 22,
    FDK_A11Y_ROLE_CHECK_MENU_ITEM = 23,
    FDK_A11Y_ROLE_RADIO_MENU_ITEM = 24,
    FDK_A11Y_ROLE_TAB = 25,
    FDK_A11Y_ROLE_OPTION = 26,   /* a combo dropdown row             */

    /* Controls */
    FDK_A11Y_ROLE_BUTTON = 30,
    FDK_A11Y_ROLE_TOGGLE_BUTTON = 31,
    FDK_A11Y_ROLE_CHECK_BOX = 32,
    FDK_A11Y_ROLE_RADIO_BUTTON = 33,
    FDK_A11Y_ROLE_LABEL = 34,
    FDK_A11Y_ROLE_ENTRY = 35,
    FDK_A11Y_ROLE_SLIDER = 36,
    FDK_A11Y_ROLE_SPIN_BUTTON = 37,
    FDK_A11Y_ROLE_COMBO_BOX = 38,
    FDK_A11Y_ROLE_PROGRESS_BAR = 39,
    FDK_A11Y_ROLE_SEPARATOR = 40,
    FDK_A11Y_ROLE_CANVAS = 41,   /* custom-drawn region              */
    FDK_A11Y_ROLE_STATUS_BAR = 42,
} fdk_a11y_role;

/* Stable, human-readable name ("button", "check menu item"). Never
 * NULL; "unknown" for out-of-range values. */
const char *fdk_a11y_role_name(fdk_a11y_role role);

/* ---- States ------------------------------------------------------- */
/* A bitmask; the core computes ENABLED/VISIBLE/SHOWING/FOCUSABLE/
 * FOCUSED from the widget's own state (see fdk_a11y_describe), the
 * class descriptor contributes the semantic ones (CHECKED, SELECTED,
 * ...). SHOWING means visible AND every ancestor visible — what a
 * screen reader would actually announce as on-screen. */

typedef enum fdk_a11y_state_flag {
    FDK_A11Y_FOCUSABLE        = 1u << 0,
    FDK_A11Y_FOCUSED          = 1u << 1,
    FDK_A11Y_ENABLED          = 1u << 2,
    FDK_A11Y_VISIBLE          = 1u << 3,  /* widget's own flag        */
    FDK_A11Y_SHOWING          = 1u << 4,  /* visible up to the root   */
    FDK_A11Y_CHECKED          = 1u << 5,
    FDK_A11Y_PRESSED          = 1u << 6,
    FDK_A11Y_SELECTED         = 1u << 7,
    FDK_A11Y_EXPANDED         = 1u << 8,
    FDK_A11Y_EDITABLE         = 1u << 9,
    FDK_A11Y_READ_ONLY        = 1u << 10,
    FDK_A11Y_MULTI_SELECTABLE = 1u << 11,
    FDK_A11Y_HAS_POPUP        = 1u << 12, /* opens a popup when
                                             activated               */
    FDK_A11Y_MODAL            = 1u << 13,
    FDK_A11Y_BUSY             = 1u << 14,
    FDK_A11Y_REQUIRED         = 1u << 15,
    FDK_A11Y_INVALID          = 1u << 16,
} fdk_a11y_state_flag;

typedef fdk_u32 fdk_a11y_state_set;

/* ---- The snapshot -------------------------------------------------- */

typedef struct fdk_a11y_info {
    fdk_a11y_role role;
    char *name;            /* owned copy; accessible name (see below);
                              NULL when the widget has none          */
    char *description;     /* owned copy; NULL when unset             */
    fdk_a11y_state_set states;
    fdk_rect bounds;       /* root/window-absolute (same space the
                              paint walk uses)                        */
    bool has_value;        /* value interface present (sliders,
                              progress, spin, scroll fractions...)    */
    double value_current;  /* meaningful when has_value               */
    double value_min;
    double value_max;
    char *value_text;      /* owned human rendering ("42", "75%");
                              NULL when none                          */
} fdk_a11y_info;

/* Fills *out with a complete snapshot. `name` resolution order:
 * (1) the widget's accessible-name override (never changes on its
 * own), (2) the class descriptor's computed name (a Label's text, a
 * Button's label, a MenuItem's text), (3) NULL. Zeroes *out first;
 * free the owned strings with fdk_a11y_info_free even on failure
 * (out is left zeroed). */
fdk_result fdk_a11y_describe(const fdk_widget *widget, fdk_a11y_info *out);

/* Releases the owned strings and zeroes the struct. NULL-safe. */
void fdk_a11y_info_free(fdk_a11y_info *info);

/* ---- Per-widget overrides ----------------------------------------- */

/* Sets the accessible name/description, overriding the class's
 * computed name (a Label's text, a Button's label). NULL clears the
 * override. Copies the string. Fires NAME_CHANGED / DESCRIPTION_
 * CHANGED notifications when the effective value changes. */
void fdk_widget_set_accessible_name(fdk_widget *widget, const char *name);
void fdk_widget_set_accessible_description(fdk_widget *widget,
                                           const char *description);

/* ---- Change notifications ------------------------------------------ */

typedef enum fdk_a11y_event_kind {
    FDK_A11Y_CHILDREN_CHANGED = 1,  /* a child was added/removed;
                                       removed events carry a detached
                                       widget (parent == NULL);
                                       ALSO fired by painted-row
                                       containers when their virtual
                                       children change            */
    FDK_A11Y_STATE_CHANGED    = 2,  /* .state_flag toggled; the new
                                       value is readable from the
                                       widget at receipt time        */
    FDK_A11Y_NAME_CHANGED     = 3,
    FDK_A11Y_DESCRIPTION_CHANGED = 4,
    FDK_A11Y_BOUNDS_CHANGED   = 5,
    FDK_A11Y_VALUE_CHANGED    = 6,
    FDK_A11Y_RELATIONS_CHANGED = 7, /* a relation edge was added or
                                       removed (see the relations
                                       section below)                */
} fdk_a11y_event_kind;

typedef struct fdk_a11y_event {
    fdk_a11y_event_kind kind;
    fdk_widget *widget;         /* the subject                          */
    fdk_a11y_state_flag state_flag; /* STATE_CHANGED only              */
} fdk_a11y_event;

typedef void (*fdk_a11y_notify_fn)(const fdk_a11y_event *event,
                                   void *user_data);

/* Subscribes to notifications for `scope`'s subtree (the widget
 * itself and all current and future descendants). Scope may be NULL
 * to observe EVERYTHING (all roots — the bridge pattern). The same
 * (fn, user, scope) triple subscribes once; duplicate calls are
 * no-ops. Callbacks run on the thread that mutated the tree, in the
 * mutation's call stack; a callback may query the tree freely and
 * may destroy widgets OTHER than `event->widget` (the notification
 * walk is snapshot-based, like the theme walk). At most
 * FDK_A11Y_MAX_SUBSCRIBERS subscriptions exist; the 17th returns
 * FDK_ERR_LIMIT. */
fdk_result fdk_a11y_subscribe(fdk_widget *scope, fdk_a11y_notify_fn fn,
                              void *user_data);
/* Removes a subscription; FDK_ERR_NOT_FOUND when absent. */
fdk_result fdk_a11y_unsubscribe(fdk_widget *scope, fdk_a11y_notify_fn fn,
                                void *user_data);
#define FDK_A11Y_MAX_SUBSCRIBERS 16

/* ---- Actions -------------------------------------------------------- */
/* Programmatic driving — the same semantics a keyboard user gets,
 * through the widget's own code paths (not input synthesis). A
 * screen reader's "click", an automation framework's "set the
 * slider to 50", and the test suite all call these. */

typedef enum fdk_a11y_action {
    FDK_A11Y_ACTION_ACTIVATE  = 1u << 0, /* button click, menu item,
                                            tab select, list select  */
    FDK_A11Y_ACTION_FOCUS     = 1u << 1,
    FDK_A11Y_ACTION_INCREMENT = 1u << 2, /* slider/spin/scroll step  */
    FDK_A11Y_ACTION_DECREMENT = 1u << 3,
    FDK_A11Y_ACTION_SET_VALUE = 1u << 4, /* .value arg is the target */
    FDK_A11Y_ACTION_EXPAND    = 1u << 5, /* tree item, submenu       */
    FDK_A11Y_ACTION_COLLAPSE  = 1u << 6,
} fdk_a11y_action;

typedef fdk_u32 fdk_a11y_action_set;

/* Which actions the widget implements right now (may depend on
 * state — a collapsed tree item cannot COLLAPSE again). */
fdk_a11y_action_set fdk_a11y_actions_of(const fdk_widget *widget);

/* Performs an action. `value` is the SET_VALUE target (ignored
 * otherwise). Returns FDK_ERR_UNSUPPORTED when the widget does not
 * implement the action in its current state, FDK_ERR_INVALID_
 * ARGUMENT on NULL widget. Actions fire the same notifications the
 * equivalent input path fires. */
fdk_result fdk_a11y_perform(fdk_widget *widget, fdk_a11y_action action,
                            double value);

/* ---- Class descriptor ---------------------------------------------- */
/* Static per-widget-class accessibility descriptor, attached to the
 * widget class vtable's `.a11y` field (see fdk_widget.h). All hooks
 * are const-friendly: describe() must not mutate the widget.
 *
 * (Defined near the bottom, after the types its hooks use.) */

/* ---- Text granularity (the text interface below) ----------------- */

typedef enum fdk_a11y_text_granularity {
    FDK_A11Y_TEXT_CHAR = 0,  /* the single codepoint at offset      */
    FDK_A11Y_TEXT_WORD = 1,  /* the whitespace-delimited word        */
    FDK_A11Y_TEXT_LINE = 2,  /* the line (whole text for one-line
                              * widgets; Entry is single-line)       */
} fdk_a11y_text_granularity;

typedef struct fdk_a11y_class {
    /* The role every instance of this class reports. */
    fdk_a11y_role role;

    /* Optional dynamic describe hook: append class-specific states /
     * value / name to the snapshot AFTER the core filled the core
     * states and BEFORE the per-widget overrides are applied. May
     * allocate out->name / out->description / out->value_text with
     * fdk_alloc (fdk_a11y_info_free releases them). NULL = static
     * role only. */
    void (*describe)(const fdk_widget *widget, fdk_a11y_info *out);

    /* Which actions instances currently implement (may vary by state
     * — e.g. a tree item reports EXPAND only while collapsed). */
    fdk_a11y_action_set (*actions)(const fdk_widget *widget);

    /* Performs an action. Returns true if handled, false otherwise
     * (the caller translates false to FDK_ERR_UNSUPPORTED). Must go
     * through the widget's own public semantics — the same code path
     * the equivalent input takes. NULL = no actions. */
    bool (*perform)(fdk_widget *widget, fdk_a11y_action action,
                    double value);

    /* ---- Virtual children (painted-row containers) -----
     *
     * Some containers DRAW their rows instead of parenting widget
     * rows (a menu's items, a menubar's titles, a combo's options,
     * a notebook's tabs). The walker cannot see painted rows — so
     * the class enumerates them here, with the same describe /
     * actions / perform shape real widgets get. A bridge walks a
     * container's REAL children first, then its virtual children.
     *
     * Virtual describe fills role/name/states/bounds itself (bounds
     * are root-absolute, the same space real widgets report); the
     * container fires FDK_A11Y_CHILDREN_CHANGED on itself whenever
     * the virtual set changes (rows added/removed/reordered).
     *
     * All four hooks are optional; a class that provides
     * virtual_describe without virtual_count is a bug (count
     * defaults to 0 = none). NULL = the class paints no addressable
     * rows. */

    size_t (*virtual_count)(const fdk_widget *container);

    void (*virtual_describe)(const fdk_widget *container, size_t index,
                             fdk_a11y_info *out);

    fdk_a11y_action_set (*virtual_actions)(const fdk_widget *container,
                                           size_t index);

    bool (*virtual_perform)(fdk_widget *container, size_t index,
                            fdk_a11y_action action, double value);

    /* ---- Text interface (text-bearing widgets) -----
     *
     * Character/word/line access to a widget's text with caret and
     * selection — the ATK Text shape. Implementers today: Entry
     * (full, including caret/selection mutators), Label and
     * SpinButton (read-only length/at-offset; the spin delegates
     * to its embedded Entry). NULL = the class bears no text. */

    /* Byte length of the text. */
    size_t (*text_length)(const fdk_widget *widget);

    /* Caret offset in bytes (0 when the class has no caret). */
    size_t (*text_caret)(const fdk_widget *widget);

    /* Selection as [anchor, caret); false when none/the class has
     * no selection. */
    bool (*text_selection)(const fdk_widget *widget, size_t *anchor,
                           size_t *caret);

    /* Writes the run containing byte `offset` at `granularity` into
     * buf (NUL-terminated, truncated to fit — the run's full byte
     * range is still reported through out_start/out_end). Returns
     * false when the class has no text. */
    bool (*text_at)(const fdk_widget *widget, size_t offset,
                    fdk_a11y_text_granularity granularity, char *buf,
                    size_t cap, size_t *out_start, size_t *out_end);

    /* Caret/selection mutators (editors only). Return false when
     * the class is read-only. */
    bool (*text_set_caret)(fdk_widget *widget, size_t offset);
    bool (*text_set_selection)(fdk_widget *widget, size_t anchor,
                               size_t caret);
} fdk_a11y_class;

/* ---- Virtual children (public queries) --------------------------------
 *
 * The painted-row enumeration every bridge uses. Index order is the
 * paint order (top-to-bottom rows; left-to-right tabs/titles).
 * Bounds are the container's root-absolute space, exactly like real
 * widgets — a menu popup's items report coordinates in the popup
 * window's space because that is the tree they belong to. */

size_t fdk_a11y_virtual_count(const fdk_widget *container);

/* Describe virtual child #index. Same zero-then-fill / free-with-
 * fdk_a11y_info_free contract as fdk_a11y_describe. Fails with
 * FDK_ERR_INVALID_ARGUMENT on a NULL container or out-of-range
 * index (no partial snapshot). */
fdk_result fdk_a11y_virtual_describe(const fdk_widget *container,
                                     size_t index, fdk_a11y_info *out);

fdk_a11y_action_set fdk_a11y_virtual_actions(const fdk_widget *container,
                                             size_t index);

/* Performs an action on a virtual child (same contract as perform). */
fdk_result fdk_a11y_virtual_perform(fdk_widget *container, size_t index,
                                    fdk_a11y_action action, double value);

/* ---- Text interface (public queries) ---------------------------------- */

/* True when the widget's class implements the text interface. */
bool fdk_a11y_has_text_interface(const fdk_widget *widget);

/* Byte length of the text (0 for no-text widgets). */
size_t fdk_a11y_text_length(const fdk_widget *widget);

/* Caret offset in bytes (0 for no-caret classes). For editors this
 * is the same byte offset fdk_entry_get_cursor reports. */
size_t fdk_a11y_text_caret(const fdk_widget *widget);

/* Current selection as [anchor, caret) byte offsets — the same pair
 * fdk_entry_get_selection reports; false when there is no selection
 * or no selection support. */
bool fdk_a11y_text_selection(const fdk_widget *widget, size_t *anchor,
                             size_t *caret);

/* The run containing byte `offset` (offset is clamped into the text;
 * off-boundary offsets are fine — the run containing them is well
 * defined). Writes the run into buf (truncating to cap-1 with a NUL;
 * out_start/out_end always report the FULL run, even when the text
 * had to be truncated). Returns FDK_ERR_UNSUPPORTED when the class
 * has no text interface, FDK_ERR_INVALID_ARGUMENT on bad pointers. */
fdk_result fdk_a11y_text_at_offset(const fdk_widget *widget,
                                   size_t offset,
                                   fdk_a11y_text_granularity granularity,
                                   char *buf, size_t cap,
                                   size_t *out_start, size_t *out_end);

/* Caret/selection mutators (editors only; read-only classes return
 * FDK_ERR_UNSUPPORTED). Off-boundary offsets fail with
 * FDK_ERR_INVALID_ARGUMENT — the same contract as
 * fdk_entry_set_cursor / fdk_entry_select_range. These are the
 * screen-reader "move the caret" / automation selection APIs. */
fdk_result fdk_a11y_text_set_caret(fdk_widget *widget, size_t offset);
/* Sets the selection through the editor's own semantics. */
fdk_result fdk_a11y_text_set_selection(fdk_widget *widget, size_t anchor,
                                       size_t caret);

/* ---- Relationships ----------------------------------------------------
 *
 * Explicit cross-references between widgets the tree topology does
 * not imply: a Label naming an Entry (LABELLED_BY — how a screen
 * reader finds "Username" for the text field), a description
 * widget, a controller and what it controls. The pairs ATK calls
 * relations; ARIA calls them properties.
 *
 * Storage is per-widget and BOUNDED: at most FDK_A11Y_MAX_RELATIONS
 * edges per widget (the inverse edges count against the other
 * widget's budget), then FDK_ERR_LIMIT.
 *
 * Symmetry: adding (A, LABEL_FOR, B) also inserts (B, LABELLED_BY,
 * A); removing either removes both. The four pairs are
 * LABEL_FOR/LABELLED_BY, DESCRIPTION_FOR/DESCRIBED_BY, and
 * CONTROLLER_FOR/CONTROLLED_BY. Self-relations and duplicates are
 * rejected (FDK_ERR_INVALID_ARGUMENT / no-op respectively).
 *
 * Lifetime: destroying a widget removes every edge that touched it
 * (both directions) — dangling relation targets cannot exist. */

typedef enum fdk_a11y_relation_type {
    FDK_A11Y_RELATION_LABEL_FOR     = 1, /* A provides B's name     */
    FDK_A11Y_RELATION_LABELLED_BY   = 2, /* B's name comes from A   */
    FDK_A11Y_RELATION_DESCRIPTION_FOR = 3,
    FDK_A11Y_RELATION_DESCRIBED_BY  = 4,
    FDK_A11Y_RELATION_CONTROLLER_FOR = 5, /* A drives B           */
    FDK_A11Y_RELATION_CONTROLLED_BY  = 6, /* B is driven by A     */
} fdk_a11y_relation_type;

#define FDK_A11Y_MAX_RELATIONS 16

/* Adds the edge AND its inverse (see the symmetry note above). */
fdk_result fdk_a11y_add_relation(fdk_widget *from,
                                 fdk_a11y_relation_type type,
                                 fdk_widget *to);

/* Removes the edge (and its inverse). FDK_ERR_NOT_FOUND when absent. */
fdk_result fdk_a11y_remove_relation(fdk_widget *from,
                                    fdk_a11y_relation_type type,
                                    fdk_widget *to);

/* Enumerate `from`'s edges of `type` in insertion order. */
size_t fdk_a11y_relation_count(const fdk_widget *from,
                               fdk_a11y_relation_type type);
/* The index-th target of that relation type. */
fdk_result fdk_a11y_relation_at(const fdk_widget *from,
                                fdk_a11y_relation_type type,
                                size_t index, fdk_widget **out_to);

/* True when (from, type, to) exists. */
bool fdk_a11y_has_relation(const fdk_widget *from,
                           fdk_a11y_relation_type type,
                           const fdk_widget *to);

#ifdef __cplusplus
}
#endif

#endif /* FDK_A11Y_H */
