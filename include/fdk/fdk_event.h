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
     * The Wayland backend never emits this event: compositors retain
     * the last committed buffer, so there is nothing to repaint. */
    FDK_EVENT_WINDOW_EXPOSE = 4,

    /* The window's maximized/minimized state changed — because the
     * application called fdk_window_maximize/unmaximize/minimize/
     * restore, or because the platform reported a change (a window
     * manager acting on its own, a taskbar un-minimize, an xdg-shell
     * configure). `state.maximized` / `state.minimized` hold the NEW
     * state as FDK knows it; compare against fdk_window_is_maximized()
     * calls you made earlier, or just re-read the flags. Emitted only
     * on actual changes. */
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
     * level drop targets are future work, documented honestly). */
    FDK_EVENT_DRAG_ENTER = 30,
    FDK_EVENT_DRAG_MOTION = 31,
    FDK_EVENT_DRAG_LEAVE = 32,
    FDK_EVENT_DRAG_DROP  = 33,
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

/* Drag-and-drop payload. `offered_formats` is the OR of every
 * fdk_drag_format the SOURCE offers (fdk_dnd.h); `accepted_formats`
 * is its intersection with the formats this window registered —
 * FDK only dispatches ENTER/MOTION/LEAVE/DROP at all when the
 * intersection is nonzero, so a drop the window cannot read is
 * never delivered (the drag visually passes over the window
 * instead). On DROP exactly one of the data pointers is non-NULL
 * for each accepted format bit (text for TEXT, uris for URI_LIST),
 * and both are invalid the moment the callback returns. URIs
 * arrive as POSIX paths when the source offered file:// URIs
 * (percent-decoding applied); non-file URIs pass through as-is. */
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
