/*
 * fdk_event.h — Faded Dream ToolKit event system
 *
 * FDK delivers events through per-window callbacks registered with
 * fdk_window_set_event_callback(). They are invoked synchronously
 * from fdk_run()'s loop, or from fdk_pump_events() in applications
 * that drive their own event loop (the standard shape for rendered,
 * animated applications — see fdk_core.h).
 *
 * All event structures here are backend-neutral: no XEvent, no
 * wl_* type ever appears in fdk_event or in a callback's arguments.
 * The platform layer's job is translating backend-specific events
 * into these before your callback ever sees them.
 */

#ifndef FDK_EVENT_H
#define FDK_EVENT_H

#include "fdk_types.h"
#include "fdk_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fdk_event_type {
    /* The compositor/window manager has assigned the window a new
     * size (in response to a resize request, an interactive resize
     * by the user, or the window's initial mapping). `configure.size`
     * holds the new size. Applications should treat this as the
     * authoritative current size — see fdk_window_resize()'s doc
     * comment on why a requested size isn't guaranteed. */
    FDK_EVENT_WINDOW_CONFIGURE = 1,

    /* The user or platform has requested the window be closed (title
     * bar close button, Alt+F4, WM close request, etc). FDK does NOT
     * destroy the window automatically on this event — the
     * application decides (e.g. show an "unsaved changes" prompt) and
     * calls fdk_window_destroy() itself if it wants to proceed. */
    FDK_EVENT_WINDOW_CLOSE_REQUEST = 2,

    /* The window gained or lost input focus. `focus.focused` is
     * nonzero if the window is now focused. */
    FDK_EVENT_WINDOW_FOCUS = 3,

    /* A previously-covered or newly-visible region of the window
     * needs repainting (X11 Expose). Applications that render with
     * fdk_surface (see fdk_surface.h) should re-present their surface
     * in response — on X11 the window's content is NOT retained by
     * the server when covered. `expose.area` is the region reported
     * by the platform; repainting the whole surface is always
     * correct and is what FDK's own examples do.
     *
     * The Wayland backend emits this event in exactly ONE case: a
     * configure arriving with a new size after the deferred first
     * frame committed (compositors retain the last committed buffer,
     * so ordinary exposure never needs a repaint — but a buffer
     * committed at the pre-configure size would present stretched,
     * so the resize path re-drives the first paint through EXPOSE). */
    FDK_EVENT_WINDOW_EXPOSE = 4,

    /* The window's maximized/minimized/fullscreen state changed —
     * because the application called fdk_window_maximize/unmaximize/
     * minimize/restore/fullscreen/unfullscreen, or because the
     * platform reported a change (a window manager acting on its
     * own, a taskbar un-minimize, an xdg-shell configure).
     * `state.maximized` / `state.minimized` / `state.fullscreen`
     * hold the NEW state as FDK knows it; compare against
     * fdk_window_is_maximized() calls you made earlier, or just
     * re-read the flags. Emitted only on actual changes. */
    FDK_EVENT_WINDOW_STATE = 5,

    /* The compositor overrode FDK's decoration request (Wayland
     * xdg-decoration only): FDK asked to draw its own title band and
     * the compositor answered SERVER-SIDE — drawing the band anyway
     * would give the window two title bars, so FDK removes its own
     * decorations before delivering this event. `decoration.
     * client_side` is 0 in this case. When FDK's request is honored
     * no event is sent (the band drawn by fdk_window_set_decorated
     * already is the correct outcome). The X11 backend never emits
     * this event: _MOTIF_WM_HINTS is honored or ignored, and a WM
     * that ignores it still lets FDK draw its band without
     * double-decorating. */
    FDK_EVENT_WINDOW_DECORATION = 6,

    /* A key was pressed or released. See fdk_key_event. */
    FDK_EVENT_KEY_DOWN = 10,
    FDK_EVENT_KEY_UP   = 11,

    /* Pointer (mouse) motion within the window. See fdk_pointer_event. */
    FDK_EVENT_POINTER_MOTION = 20,

    /* Pointer button press/release. See fdk_pointer_button_event. */
    FDK_EVENT_POINTER_BUTTON_DOWN = 21,
    FDK_EVENT_POINTER_BUTTON_UP   = 22,

    /* Pointer entered/left the window's surface. */
    FDK_EVENT_POINTER_ENTER = 23,
    FDK_EVENT_POINTER_LEAVE = 24,

    /* Scroll/wheel input. See fdk_scroll_event. */
    FDK_EVENT_POINTER_SCROLL = 25,

    /* ---- Drag and drop (1.2.0) ----
     *
     * Delivered to the window the pointer is over during a drag, to
     * windows that registered acceptance via fdk_window_set_drop_formats
     * (fdk_dnd.h). ENTER/MOTION/LEAVE carry only position + the
     * offered format mask; DROP additionally carries the transferred
     * data, valid ONLY for the duration of the callback (FDK frees
     * it after). The widget tree does not consume drag events —
     * they always reach the application's window callback, which
     * hit-tests its own widgets (v1 DnD is window-level; widget-
     * level drop targets are future work, documented honestly).
     *
     * 1.4.19: DROP is delivered even when the payload transfer
     * itself failed (owner refused, timed out, or served nothing
     * readable) — with both data pointers NULL. The gesture landing
     * is the application's signal; a drop that vanishes silently
     * with no event at all is the failure mode this closes. */
    FDK_EVENT_DRAG_ENTER = 30,
    FDK_EVENT_DRAG_MOTION = 31,
    FDK_EVENT_DRAG_LEAVE = 32,
    FDK_EVENT_DRAG_DROP  = 33,

    /* ---- Touch (1.1.0, M-3) ----
     *
     * Direct-touch contact tracking (touchscreens; trackpads report
     * two-finger scrolling as ordinary FDK_EVENT_POINTER_SCROLL via
     * the platform's own gesture recognition). Each contact is a
     * SLOT: `id` identifies it from TOUCH_DOWN to TOUCH_UP/CANCEL,
     * `position` is window-local LOGICAL coordinates like every
     * pointer event. Multiple slots are simultaneously active (two
     * fingers = two slots with distinct ids).
     *
     * FDK synthesizes on top of raw slots (the policy lives in the
     * core, identical on both backends — see docs/platform-input.md):
     *   - TAP: one slot, down-to-up within FDK_TOUCH_TAP_MS and
     *     FDK_TOUCH_TAP_SLOP logical pixels of travel, synthesizes a
     *     full left POINTER_BUTTON_DOWN + POINTER_BUTTON_UP pair at
     *     the tap position (widgets and applications that only
     *     handle pointer events work on touch for free).
     *   - TWO-FINGER SCROLL: while exactly two slots are active, the
     *     CENTROID's motion synthesizes FDK_EVENT_POINTER_SCROLL with
     *     NATURAL-scrolling deltas (fingers drag down, content
     *     follows: delta = -(centroid travel); the platform's
     *     natural-scroll users get the flip they expect, the
     *     conventional users invert in their settings like they
     *     already do for trackpads).
     * Raw TOUCH_* events are delivered alongside the synthesis —
     * gesture-aware applications read them directly; plain
     * widget applications never need to.
     *
     * Wayland: wl_touch translated 1:1 (surface-local coords are
     * already logical). X11: XI2 touch (XI_TouchBegin/Update/End)
     * through dlopen'd libXi, the GL lane's load discipline — absent
     * libXi or XI < 2.2 means no touch events on that display, never
     * an init failure. */
    FDK_EVENT_TOUCH_DOWN   = 40,
    FDK_EVENT_TOUCH_UP     = 41,
    FDK_EVENT_TOUCH_MOTION = 42,
    FDK_EVENT_TOUCH_CANCEL = 43, /* compositor aborted the whole
                                    * gesture: all slots die, nothing
                                    * is synthesized afterwards */

    /* ---- IME (1.1.0, M-1) ----
     *
     * Input-method events: the composition (preedit) text an IME
     * builds before committing, and the committed text it finally
     * delivers. `text` is UTF-8, owned by FDK for the duration of
     * the callback ONLY (the drag-payload contract). PREEDIT with
     * an empty string clears the composition; COMMIT never carries
     * an empty string.
     *
     * Delivery: FDK routes both events at the FOCUSED text widget
     * first — an Entry or TextView with keyboard focus consumes them
     * (preedit renders inline at the caret via the same machinery
     * fdk_entry_set_preedit drives; commit inserts the text). When
     * nothing focused consumes them they reach the application's
     * window callback — custom text surfaces implement their own
     * composition handling there.
     *
     * A single-character commit ALSO arrives as the ordinary
     * KEY_DOWN's codepoint (backwards compatibility: simple dead-key
     * and compose sequences keep working for codepoint-only
     * consumers); multi-character commits (the CJK/indic case an IME
     * exists for) arrive ONLY as FDK_EVENT_IME_COMMIT.
     *
     * Per backend: Wayland text-input-v3 (enable on keyboard focus,
     * surrounding text, the enable/disable/commit state machine);
     * X11 XIM with XIMPreeditCallbacks (the per-IC composition
     * buffer maintained from the draw/done callbacks; commits flow
     * Xutf8LookupString). Committed text > 1 codepoint dispatches
     * IME_COMMIT; the preedit buffer dispatches IME_PREEDIT on
     * every change. */
    FDK_EVENT_IME_COMMIT   = 50, /* the IME delivered final text   */
    FDK_EVENT_IME_PREEDIT  = 51, /* composition text changed       */
} fdk_event_type;

typedef struct fdk_configure_event {
    fdk_size size;
} fdk_configure_event;

typedef struct fdk_focus_event {
    int focused; /* nonzero = gained focus, zero = lost focus */
} fdk_focus_event;

typedef struct fdk_expose_event {
    /* Window-local region reported as needing repaint. Repainting
     * more (e.g. the whole surface) is always safe. */
    fdk_rect area;
} fdk_expose_event;

typedef struct fdk_state_event {
    int maximized; /* nonzero = window is now maximized */
    int minimized; /* nonzero = window is now minimized/iconic */
    int fullscreen; /* 1.4.10: nonzero = window is now fullscreen */
} fdk_state_event;

typedef struct fdk_decoration_event {
    /* Nonzero while CLIENT-SIDE decorations apply (FDK or the app
     * draws the title bar). Currently only ever delivered as 0 —
     * see the FDK_EVENT_WINDOW_DECORATION comment above. */
    int client_side;
} fdk_decoration_event;

/* Physical key identity, independent of keyboard layout — use this
 * for shortcuts/bindings you want stable across layouts (e.g. "the W
 * key" for WASD movement regardless of what letter is printed on it).
 * Values follow the Linux evdev scancode numbering, since both the
 * X11 and Wayland backends ultimately derive keycodes from that
 * numbering space on Linux — see docs/platform-input.md. */
typedef fdk_u32 fdk_scancode;

/* Named scancodes for the keys the toolkit itself references (focus
 * traversal, activation) and applications most commonly bind. These are
 * Linux evdev constants — the same numbering space fdk_scancode uses on
 * both backends — given FDK_ names so applications (and FDK's own
 * widget layer) never hardcode bare integers. The list is deliberately
 * small: it is not a keyboard enumeration, just the keys with
 * toolkit-level meaning. */
#define FDK_KEY_TAB        ((fdk_scancode)15)
#define FDK_KEY_BACKSPACE  ((fdk_scancode)14)
#define FDK_KEY_ENTER      ((fdk_scancode)28)
#define FDK_KEY_ESC        ((fdk_scancode)1)
#define FDK_KEY_SPACE      ((fdk_scancode)57)
#define FDK_KEY_DELETE     ((fdk_scancode)111)
#define FDK_KEY_HOME       ((fdk_scancode)102)
#define FDK_KEY_END        ((fdk_scancode)107)
#define FDK_KEY_PAGE_UP    ((fdk_scancode)104)
#define FDK_KEY_PAGE_DOWN  ((fdk_scancode)109)
#define FDK_KEY_LEFT       ((fdk_scancode)105)
#define FDK_KEY_RIGHT      ((fdk_scancode)106)
#define FDK_KEY_UP         ((fdk_scancode)103)
#define FDK_KEY_DOWN       ((fdk_scancode)108)

typedef struct fdk_key_event {
    fdk_scancode scancode;

    /* The layout-resolved Unicode codepoint this key produces, given
     * current modifier state (e.g. Shift+A -> 'A'). 0 if the key
     * produces no textual codepoint (e.g. a bare modifier key, F-keys,
     * arrow keys). Use this for text entry; use `scancode` for
     * shortcuts. */
    fdk_u32 codepoint;

    /* Bitmask of currently-held modifiers, see fdk_key_modifier. */
    fdk_u32 modifiers;

    /* Nonzero if this KEY_DOWN is an auto-repeat from the key being
     * held, rather than an initial press. Always 0 for KEY_UP. */
    int is_repeat;
} fdk_key_event;

typedef enum fdk_key_modifier {
    FDK_MOD_SHIFT = 1 << 0,
    FDK_MOD_CTRL  = 1 << 1,
    FDK_MOD_ALT   = 1 << 2,
    FDK_MOD_SUPER = 1 << 3, /* "Windows"/"Command"/"Meta" key */
} fdk_key_modifier;

/* ---- Shortcut specifications (1.3.3) --------------------------------
 *
 * A shortcut spec is a human string — "Ctrl+S", "ctrl+shift+z",
 * "F5", "Alt+F4" — parsed into the (modifier bitmask, scancode) pair
 * a key event carries. Grammar and semantics:
 *
 *   spec    := key | modifier { "+" modifier } { "+" key }
 *   modifier:= ctrl|control|shift|alt|super|meta (case-insensitive;
 *             surrounding spaces are tolerated)
 *   key     := A-Z | 0-9 | F1-F12 | tab enter return esc escape space
 *             backspace delete del insert ins home end pageup pgup
 *             pagedown pgdn left right up down plus minus comma
 *             period dot slash
 *
 * A spec binds the PHYSICAL key (the scancode, layout-stable — "the
 * S position"), per the fdk_scancode contract above. Garbage
 * (empty string, bare modifier, "Ctrl+", "Ctrl+Ctrl+S", two key
 * segments, unknown names) is FDK_ERR_INVALID_ARGUMENT — parse
 * failures are loud, never guesses.
 *
 * Where specs are CONSUMED: fdk_window_add_shortcut() registers
 * application shortcuts on a window, and menu items' shortcut
 * labels (fdk_menu_item_set_shortcut) act as live accelerators for
 * as long as the menu hangs under that window's menu bar. Both
 * match with exact modifier equality: "Ctrl+S" does not fire while
 * Shift is also held, and fires on auto-repeat presses too (hold
 * the key, the handler repeats — same rule the Entry widget's own
 * keys follow).
 */
fdk_result fdk_shortcut_parse(const char *spec, fdk_u32 *out_modifiers,
                              fdk_scancode *out_key);

/* Canonical composition helpers: the lowercase spec a parser round-
 * trips a (modifiers, scancode) pair back to. NULL when the pair has
 * no name in the grammar (an unmapped scancode). Useful for conflict
 * diagnostics ("Ctrl+S is already bound") and for serializing a
 * binding table. Writes at most FDK_SHORTCUT_SPEC_MAX bytes incl.
 * the NUL. */
#define FDK_SHORTCUT_SPEC_MAX 32
bool fdk_shortcut_format(fdk_u32 modifiers, fdk_scancode key,
                         char *out_spec, size_t spec_max);

typedef struct fdk_pointer_event {
    fdk_pointf position; /* window-relative coordinates */
} fdk_pointer_event;

typedef enum fdk_pointer_button {
    FDK_POINTER_BUTTON_LEFT   = 1,
    FDK_POINTER_BUTTON_MIDDLE = 2,
    FDK_POINTER_BUTTON_RIGHT  = 3,
} fdk_pointer_button;

typedef struct fdk_pointer_button_event {
    fdk_pointf position;
    fdk_u32 button; /* an fdk_pointer_button value, or a higher button
                       index the platform reports (side buttons etc.) */
    fdk_u32 modifiers; /* bitmask of fdk_key_modifier held at press/
                          release (Phase 9: shift/ctrl-click selection
                          in List/Tree/Entry). Appended field — zero
                          for events synthesized by older code, and
                          safe to ignore. */
} fdk_pointer_button_event;

typedef struct fdk_scroll_event {
    fdk_pointf position;
    fdk_f32 delta_x;
    fdk_f32 delta_y;
} fdk_scroll_event;

/* ---- Touch slots (1.1.0, M-3) ---------------------------------------- */

/* Tap-recognition constants (the documented policy; logical px and
 * monotonic ms — see FDK_EVENT_TOUCH_DOWN above): within these
 * bounds a single-slot contact is a TAP and synthesizes a click. */
#define FDK_TOUCH_TAP_MS 300
#define FDK_TOUCH_TAP_SLOP 10.0f

typedef struct fdk_touch_event {
    fdk_u32 id;            /* backend-assigned slot id, stable for the
                              * contact's lifetime */
    fdk_pointf position;   /* window-local, logical coordinates */
} fdk_touch_event;

/* ---- IME payloads (1.1.0, M-1) ---------------------------------------- */

typedef struct fdk_ime_event {
    /* NUL-terminated UTF-8, owned by FDK for the callback duration
     * only — copy it if it must survive (the drag-payload rule).
     * COMMIT: never empty. PREEDIT: empty clears the composition. */
    const char *text;
    /* PREEDIT: the cursor offset in UTF-8 BYTES into text (0 = start).
     * The Entry/TextView render it as the composition caret. COMMIT:
     * unused (0). */
    int cursor;
} fdk_ime_event;

/* Drag-and-drop payload. `offered_formats` is the OR of every
 * fdk_drag_format the SOURCE offers (fdk_dnd.h); `accepted_formats`
 * is its intersection with the formats this window registered —
 * FDK only dispatches ENTER/MOTION/LEAVE/DROP at all when the
 * intersection is nonzero, so a drop the window cannot read is
 * never delivered (the drag visually passes over the window
 * instead). On DROP the data pointer for each accepted format bit
 * carries the transferred payload (text for TEXT, uris for
 * URI_LIST), with one honest exception (1.4.19): when the transfer
 * itself failed — the owner refused or never answered — the DROP
 * still arrives with BOTH pointers NULL (an empty gesture, so the
 * application can react and report; the alternative, no event at
 * all, was a silent vanish). Both pointers are invalid the moment
 * the callback returns. URIs arrive as POSIX paths when the source
 * offered file:// URIs (percent-decoding applied); non-file URIs
 * pass through as-is. */
typedef struct fdk_drag_event {
    int offered_formats;    /* fdk_drag_format OR-mask from the source */
    int accepted_formats;   /* intersection with this window's mask    */
    fdk_u32 modifiers;      /* keyboard modifiers held right now       */
    fdk_pointf position;    /* window-local, like every pointer event  */
    const char *text;       /* DROP only; NULL unless TEXT accepted    */
    char **uris;            /* DROP only; NULL unless URI_LIST accepted*/
    size_t uri_count;       /* entries in uris                         */
} fdk_drag_event;

/* Tagged union of all event payloads. `type` says which member of the
 * anonymous union is valid. This whole struct is only ever handed to
 * you by-value inside a callback invocation — FDK does not expose
 * fdk_event as a heap object applications create or free themselves,
 * despite the opaque `fdk_event` type existing in fdk_types.h for use
 * by a possible future event-queue API (not present in Phase 2). */
typedef struct fdk_event_data {
    fdk_event_type type;
    union {
        fdk_configure_event configure;
        fdk_focus_event focus;
        fdk_expose_event expose;
        fdk_state_event state;
        fdk_decoration_event decoration;
        fdk_key_event key;
        fdk_pointer_event pointer;
        fdk_pointer_button_event pointer_button;
        fdk_scroll_event scroll;
        fdk_drag_event drag;
        fdk_touch_event touch;
        fdk_ime_event ime;
    };
} fdk_event_data;

/* Callback signature. `window` is the window the event occurred on;
 * `user_data` is whatever was passed to
 * fdk_window_set_event_callback(). Called synchronously from within
 * fdk_run() — do not call fdk_run() re-entrantly from within a
 * callback. */
typedef void (*fdk_event_callback_fn)(fdk_window *window,
                                       const fdk_event_data *event,
                                       void *user_data);

/* Registers (or replaces) the event callback for `window`. Pass
 * callback = NULL to stop receiving events (they will simply be
 * dropped after translation — this does not affect the platform
 * connection itself). */
void fdk_window_set_event_callback(fdk_window *window,
                                    fdk_event_callback_fn callback,
                                    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* FDK_EVENT_H */
