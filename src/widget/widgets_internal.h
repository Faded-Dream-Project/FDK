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
#include "fdk/fdk_dialog.h" /* 1.4.4: the colorbutton test seam    */
#include "fdk/fdk_animation.h" /* 1.4.1: the Expander's reveal anim */
#include "fdk/fdk_core.h"      /* 1.4.1: fdk_timer (the Spinner)    */

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
extern const fdk_widget_class fdk_spinner_class_def;   /* 1.4.1 */
extern const fdk_widget_class fdk_paned_class_def;     /* 1.4.1 */
extern const fdk_widget_class fdk_expander_class_def;  /* 1.4.1 */
extern const fdk_widget_class fdk_statusbar_class_def; /* 1.4.2 */
extern const fdk_widget_class fdk_levelbar_class_def; /* 1.4.2 */
extern const fdk_widget_class fdk_revealer_class_def; /* 1.4.2 */
extern const fdk_widget_class fdk_stack_class_def;     /* 1.4.2 */
extern const fdk_widget_class fdk_stackswitcher_class_def; /* 1.4.2 */

/* toolbar.c: relayout hook for the layout notifier (box.c). */
void fdk__toolbar_layout_changed(fdk_widget *w);

/* paned.c / expander.c: the same relayout hooks for the 1.4.1
 * containers — the notifier (box.c) calls them when a child is
 * added to (or re-measured inside) one of these. */
void fdk__paned_layout_changed(fdk_widget *w);
void fdk__expander_layout_changed(fdk_widget *w);

/* revealer.c / stack.c (1.4.2): the same relayout hooks for the
 * reveal container and the page stack. */
void fdk__revealer_layout_changed(fdk_widget *w);
void fdk__stack_layout_changed(fdk_widget *w);

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
/* 1.4.0 modern-face family (statics.c, same paint-time resolution). */
fdk_color fdk__pal_selection(void);
fdk_color fdk__pal_focus_ring(void);
fdk_color fdk__pal_sidebar(void);
fdk_color fdk__pal_menu_bg(void);
fdk_color fdk__pal_accent_hover(void);
fdk_color fdk__pal_accent_pressed(void);
fdk_color fdk__pal_accent_text(void);
fdk_color fdk__pal_link(void);
fdk_color fdk__pal_entry(void);
fdk_color fdk__pal_entry_border(void);
fdk_color fdk__pal_row_hover(void);

/* ---- 1.4.1: the shared symbolic row glyphs (statics.c) ----------
 *
 * Font-independent vector strokes shared by the List (1.4.1) and
 * the Tree (1.4.2): a 16-px glyph box, 6-px gap to the text. */

/* The glyph box + gap a row must reserve for `icon` (0 for NONE). */
fdk_i32 fdk__row_icon_advance(fdk_row_icon icon);

/* Paints the glyph with its box's top-left at (x, cy) — the SAME
 * (x, cy) convention as the list's 1.4.1 call sites. */
void fdk__row_icon_paint(fdk_surface *surface, fdk_row_icon icon,
                         fdk_i32 x, fdk_i32 cy, bool disabled);

/* ---- shared instance structs ---- */

/* Label. `lines` is the display cache: the text broken into the
 * lines that fit `built_width` (NOWRAP: one full line; WRAP: the
 * greedy word-wrap; ELLIPSIZE: one line capped by the ellipsis
 * pass). Rebuilt on arrange (width changed) and lazily at paint when
 * dirty — see statics.c. */
typedef struct fdk_label {
    fdk_widget base;
    fdk_font *font;    /* borrowed */
    char *text;        /* owned, may be NULL (markup: the PLAIN text) */
    fdk_color color;   /* text color                          */
    bool color_set;    /* false: resolve the theme's text color
                       * at paint time (the default) — the getter
                       * reports the SAME resolution (1.3.0)      */
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
    /* 1.4.11 markup: the parsed attribute spans (owned; NULL for a
     * plain set_text label) and the flattened-run cache the paint
     * path builds alongside the line cache (rebuilt on the same
     * dirty/width triggers, freed with the label). The run type is
     * forward-declared below — text_internal.h carries the text
     * layer's vendored includes, which the widget translation units
     * deliberately do not see. */
    fdk_span *spans;
    size_t span_count;
    struct fdk__text_run *runs; /* owned flatten cache, may be NULL */
    size_t run_count;
} fdk_label;

/* The shared hover-fade state (1.4.1): the visual blend a control
 * paints with while the pointer's enter/leave transition animates
 * (the SEMANTIC hover flag flips immediately; only the paint
 * blends — press feedback stays instant by design). One flight
 * at a time per widget: arming cancels the running fade and
 * departs from the CURRENT blend (retarget-from-live, the
 * animator's own compose rule). */
typedef struct fdk_hover_fade {
    fdk_f32 t;              /* current blend, 0..1                    */
    fdk_f32 from;           /* the flight's departure blend           */
    fdk_f32 target;         /* 0 (resting) or 1 (hovered)             */
    fdk_animation *anim;    /* the running fade, or NULL              */
} fdk_hover_fade;

/* statics.c: arms (or retargets) a shared hover fade toward 1
 * (entering) / 0 (leaving) — the 1.4.1 Button/check machinery,
 * shared by the StackSwitcher's pills and the menu rows since
 * 1.4.2. Cancels the running flight first (retarget-from-live). */
void fdk__hover_fade_arm(fdk_widget *w, fdk_hover_fade *f,
                         bool entering);

/* Button. */
typedef struct fdk_button {
    fdk_widget base;
    fdk_font *font;    /* borrowed */
    char *text;        /* owned, may be NULL (markup: the PLAIN text) */
    fdk_button_activate_fn on_activate;
    void *on_activate_data;
    bool pressed;      /* pointer down inside */
    bool hovering;
    fdk_button_role role; /* 1.4.0 paint role (default NORMAL) */
    bool checked;      /* 1.4.0 toggle-button state */
    fdk_hover_fade fade; /* 1.4.1: hover paint blend */
    /* 1.4.11 markup: parsed attribute spans (owned; NULL when the
     * text was set with fdk_button_set_text). Buttons are single-
     * line: measure/paint flatten per call (no cache to keep
     * coherent with anything). */
    fdk_span *spans;
    size_t span_count;
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
    fdk_hover_fade fade; /* 1.4.1: hover paint blend */
    void (*on_change)(fdk_widget *w, bool checked, void *user);
    void *on_change_data;
} fdk_check_widget;

/* ProgressBar. */
typedef struct fdk_progress {
    fdk_widget base;
    fdk_f32 fraction; /* [0,1], determinate mode only */
    /* Indeterminate mode (1.3.1): an animated block sweeping the
     * track, driven by the timer queue ("busy" — work is happening
     * with no known fraction). NULL timer when detached from any
     * window: the block parks at phase 0 (headless tests see a
     * static block, never an animation). */
    bool indeterminate;
    fdk_timer *pulse_timer;
    fdk_f32 pulse_phase; /* block origin, [0, 1 + PROGRESS_BLOCK) */
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

/* Spinner (1.4.1, spinner.c): the busy indicator. Phase in RADIANS
 * [0, 2*pi), advanced by the repeating tick while spinning; the arc
 * parks at the last phase when stopped (GTK semantics — restart
 * continues, it does not reset). NULL timer when not spinning or
 * detached (no context): the arc paints statically at the parked
 * phase. */
typedef struct fdk_spinner {
    fdk_widget base;
    bool spinning;
    fdk_timer *tick_timer;
    fdk_f32 phase;        /* radians, the arc head's angle */
} fdk_spinner;

/* Paned (1.4.1, paned.c): the two-pane splitter. `position` is the
 * divider's offset from the pane-1 edge in paned-local px;
 * `position_set` false = auto split (natural sizes, leftover even).
 * `dragging` rides the implicit grab from a press inside the divider
 * band; `grab_offset` is pointer-to-divider-origin at grab (the
 * drag retargets, it never teleports under the pointer). */
typedef struct fdk_paned {
    fdk_widget base;
    fdk_orientation orientation;
    bool position_set;
    fdk_i32 position;
    bool dragging;
    fdk_i32 grab_offset;
    bool div_hovering;   /* divider band under the pointer          */
} fdk_paned;

/* Expander (1.4.1, expander.c): the disclosure section. Header =
 * triangle + label; the ONE child is the content. `reveal` is the
 * animated 0..1 blend of the content's extent (0 = collapsed); the
 * child is FDK_WF_VISIBLE-off while fully collapsed so it is
 * input-transparent and skipped by paints. `anim` is the running
 * reveal animation (NULL when idle); standalone trees snap (no
 * clock to tick). */
typedef struct fdk_expander {
    fdk_widget base;
    fdk_font *font;      /* borrowed */
    char *label;         /* owned, may be NULL */
    bool expanded;
    bool hovering;       /* header band under pointer               */
    bool pressed;        /* header press visual                     */
    fdk_f32 reveal;      /* 0..1 content extent blend               */
    fdk_f32 reveal_from; /* the flight's departure blend            */
    fdk_animation *anim; /* running reveal animation or NULL        */
    void (*on_changed)(fdk_widget *w, bool expanded, void *user);
    void *on_changed_data;
} fdk_expander;

/* Statusbar (1.4.2, statusbar.c): the context-scoped message bar.
 * `msgs` is the global message stack in GTK order — the TOP of the
 * stack is what paints, pop(context) removes the topmost message OF
 * THAT context, remove(context, id) removes one by handle. Each
 * entry owns its text; `next_id` mints message handles (0 reserved:
 * "no such message"). */
typedef struct fdk_statusbar_msg {
    fdk_u32 context;
    fdk_u32 id;
    char *text;         /* owned */
} fdk_statusbar_msg;

typedef struct fdk_statusbar {
    fdk_widget base;
    fdk_font *font;         /* borrowed */
    fdk_statusbar_msg *msgs;
    size_t count;
    size_t capacity;
    fdk_u32 next_id;
} fdk_statusbar;

/* LevelBar (1.4.2, levelbar.c): the segmented value readout. */
typedef struct fdk_levelbar {
    fdk_widget base;
    double min;
    double max;
    double value;
    fdk_levelbar_mode mode;   /* DISCRETE (default) / CONTINUOUS */
    size_t segments;          /* DISCRETE mode's block count */
} fdk_levelbar;

/* Revealer (1.4.2, revealer.c): the expander's flight generalized
 * to any child. `reveal` is the animated 0..1 blend of the child's
 * extent along the transition axis; the child is FDK_WF_VISIBLE-off
 * at exactly 0. `anim` is the running flight (NULL when idle);
 * standalone trees snap (no clock to tick). */
typedef struct fdk_revealer {
    fdk_widget base;
    fdk_revealer_transition transition;
    fdk_u32 duration_ms;      /* the flight's duration (default 160) */
    bool revealed;            /* the TARGET state */
    fdk_f32 reveal;           /* current blend, 0..1 */
    fdk_f32 reveal_from;      /* the flight's departure blend */
    fdk_animation *anim;      /* running flight or NULL */
    void (*on_revealed)(fdk_widget *w, bool revealed, void *user);
    void *on_revealed_data;
} fdk_revealer;

/* Stack (1.4.2, stack.c): named pages, exactly one visible. Pages
 * are reparented in (the notebook's adoption model); `current` is
 * the shown page index. */
typedef struct fdk_stack_page {
    fdk_widget *widget;   /* owned via the tree */
    char *name;           /* owned; page's unique key */
    char *title;          /* owned; the switcher's pill label */
} fdk_stack_page;

typedef struct fdk_stack {
    fdk_widget base;
    fdk_stack_page *pages;
    size_t count;
    size_t capacity;
    size_t current;       /* index of the shown page */
    void (*on_changed)(fdk_widget *stack, size_t index,
                       const char *name, void *user);
    void *on_changed_data;
} fdk_stack;

/* StackSwitcher (1.4.2, stackswitcher.c): the pill row bound to a
 * stack. `stack_watch` is a persistent reentrancy watch on the
 * bound stack: paint/arrange re-check it, so a stack destroyed by
 * another hand cleanly unbinds (the switcher paints its empty row,
 * it never dereferences a dangling pointer). `hover_pill` rides
 * MOTION like the notebook's hover_tab; pill blends fade on the
 * 1.4.1 hover-fade machinery. */
typedef struct fdk_stackswitcher {
    fdk_widget base;
    fdk_font *font;         /* borrowed */
    fdk_widget *stack;      /* borrowed, NULL when unbound */
    fdk_widget_watch stack_watch;
    int hover_pill;         /* -1 when none */
    fdk_hover_fade *fades;  /* per-pill paint blend (fade_count) */
    size_t fade_count;      /* fades array length (cached count) */
    char **titles;          /* owned pill titles, fade_count long  */
    char **names;           /* owned page keys, fade_count long    */
} fdk_stackswitcher;

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
static inline fdk_spinner *spinner_of(fdk_widget *w) {
    return (fdk_spinner *)w;
}
static inline fdk_paned *paned_of(fdk_widget *w) {
    return (fdk_paned *)w;
}
static inline fdk_expander *expander_of(fdk_widget *w) {
    return (fdk_expander *)w;
}
static inline fdk_statusbar *statusbar_of(fdk_widget *w) {
    return (fdk_statusbar *)w;
}
static inline fdk_levelbar *levelbar_of(fdk_widget *w) {
    return (fdk_levelbar *)w;
}
static inline fdk_revealer *revealer_of(fdk_widget *w) {
    return (fdk_revealer *)w;
}
static inline fdk_stack *stack_of(fdk_widget *w) {
    return (fdk_stack *)w;
}
static inline fdk_stackswitcher *switcher_of(fdk_widget *w) {
    return (fdk_stackswitcher *)w;
}

/* ---- File dialog scan seam (1.2.0, file_dialog.c) ----
 *
 * The directory scan/sort is pure logic the headless suite pins
 * (tests/test_file_dialog_logic.c) without a display: hidden-file
 * filtering, dirs-only filtering, dirs-first ordering, the entry
 * cap, and the ownership contract of the entries array. The 1.2.3
 * additions (glob matching, filesystem discovery, path helpers)
 * live under the same seam for the same reason. */
typedef struct fdk_fd_entry {
    char *name;   /* owned, basename only */
    bool dir;
    bool hidden;
} fdk_fd_entry;

typedef struct fdk_fd_entries {
    fdk_fd_entry *v;
    size_t count;
} fdk_fd_entries;

/* Splits a ";"-separated filter string into an owned array of owned
 * pattern strings (empty entries skipped). Returns the count; *out
 * is NULL when the input is NULL/empty (count 0 — no filtering). */
size_t fdk__file_dialog_parse_filters(const char *filters, char ***out);
void fdk__file_dialog_free_filters(char **patterns, size_t count);

/* Case-insensitive glob match ('*' = any run, '?' = one char, else
 * literal — byte-wise, locale-independent). */
bool fdk__file_dialog_glob_match(const char *pattern, const char *name);

/* Scans `dir` (opendir/readdir + stat fallback for DT_UNKNOWN).
 * Returns 0 on success (empty listing included), -1 when the
 * directory cannot be opened. Fully owned by the caller. `patterns`
 * (count > 0) filters FILES case-insensitively; directories are
 * never filtered. */
int fdk__file_dialog_scan(const char *dir, bool dirs_only,
                          bool show_hidden,
                          char **patterns, size_t pattern_count,
                          fdk_fd_entries *out);
void fdk__file_dialog_entries_free(fdk_fd_entries *entries);

/* ---- Filesystem discovery seam (1.2.3, file_dialog.c) ----
 *
 * The places sidebar's data source: pure POSIX (getenv + stat +
 * /proc/self/mounts + one-level scans of /media and /mnt) — no udev,
 * no D-Bus, per the toolkit's no-bus policy. Every place returned
 * is stat()-verified to be an existing directory at discovery time
 * and is deduplicated by canonical path. Returns 0 with *out/count
 * set (count >= 1 always: $HOME or / survives), -1 on OOM. */
typedef struct fdk_fs_place {
    char *label;  /* owned; short display name ("Home", "boot")   */
    char *path;   /* owned; absolute, canonical-ish, existing dir */
} fdk_fs_place;

int fdk__fs_discover_places(fdk_fs_place **out, size_t *count);
void fdk__fs_places_free(fdk_fs_place *places, size_t count);

/* Bounded, allocated path join (a/b with exactly one '/'; a == "/"
 * does not double the slash). NULL on OOM. Caller frees (fdk_free). */
char *fdk__path_join(const char *dir, const char *name);

/* Trims trailing slashes (but keeps the root "/"); returns an owned
 * copy or NULL on OOM/empty input. */
char *fdk__path_normalize_dir(const char *dir);

/* SAVE name validation, shared with the headless tests: 0 = valid,
 * 1 = empty, 2 = contains '/', 3 = "." or "..", 4 = longer than 255
 * bytes (NAME_MAX-safe), 5 = whitespace-only. */
int fdk__save_name_validate(const char *name);

/* "~" and "~/" expansion against $HOME. Returns an owned copy of
 * `path` when no expansion applies (NULL on OOM). */
char *fdk__path_expand_tilde(const char *path);

/* ---- Recents seam (1.4.4, file_dialog.c) ---------------------------
 *
 * The XDG recently-used.xbel surface as pure logic the headless
 * suite pins (tests/test_file_dialog_logic.c): a text-level XBEL
 * scanner (no XML library — the theme/prefs parser discipline) and
 * a text-splice writer. Timestamps convert via a civil-days epoch
 * algorithm (no timegm dependency). */
typedef struct fdk_fd_recent {
    char *path;    /* owned; decoded absolute filesystem path */
    fdk_i64 mtime; /* seconds since epoch (the modified attr)   */
} fdk_fd_recent;

/* Writes the recents file's path into buf (the $XDG_DATA_HOME /
 * $HOME/.local/share resolution). False when no home exists. */
bool fdk__recent_file(char *buf, size_t n);

/* Parses the xbel at `xbel` into owned entries: decoded paths,
 * mtime-descending, deduped (newest wins), capped at 128 entries.
 * 0 on success (an empty list included), -1 when unreadable or
 * unparseable (out cleared either way). */
int fdk__recent_load(const char *xbel, fdk_fd_recent **out,
                     size_t *count);
void fdk__recent_free(fdk_fd_recent *v, size_t count);

/* Moves `path` to the front of the xbel at `xbel` with fresh
 * added/modified/visited stamps (percent-encoded file:// URI),
 * removing any existing bookmark with the same href, capping the
 * entry count at 128, creating the file (and its data directory,
 * best-effort) when missing, and writing ATOMICALLY (tmp +
 * rename). 0 on success, -1 on failure (nothing written). */
int fdk__recent_touch(const char *xbel, const char *path);

/* ColorButton test seam (choosers.c, 1.4.4): drives the acceptance
 * path exactly as the chooser dialog's done callback would — the
 * token hop between the dialog and a possibly-destroyed button is
 * the only difference, and the X11 GUI group covers that half. */
void fdk__colorbutton_apply_result(fdk_widget *button,
                                   const fdk_color_dialog_result *res);

#endif /* FDK_WIDGETS_INTERNAL_H */
