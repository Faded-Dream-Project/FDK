/*
 * fdk_event.h — Faded Dream ToolKit event system
 *
 * FDK delivers events through per-window callbacks registered with
 * fdk_window_set_event_callback(), invoked synchronously during
 * fdk_run()'s dispatch of platform events. There is no separate
 * "pump events yourself" API in Phase 2 — fdk_run() owns the loop.
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
} fdk_event_type;

typedef struct fdk_configure_event {
    fdk_size size;
} fdk_configure_event;

typedef struct fdk_focus_event {
    int focused; /* nonzero = gained focus, zero = lost focus */
} fdk_focus_event;

/* Physical key identity, independent of keyboard layout — use this
 * for shortcuts/bindings you want stable across layouts (e.g. "the W
 * key" for WASD movement regardless of what letter is printed on it).
 * Values follow the Linux evdev scancode numbering, since both the
 * X11 and Wayland backends ultimately derive keycodes from that
 * numbering space on Linux — see docs/platform-input.md. */
typedef fdk_u32 fdk_scancode;

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
} fdk_pointer_button_event;

typedef struct fdk_scroll_event {
    fdk_pointf position;
    fdk_f32 delta_x;
    fdk_f32 delta_y;
} fdk_scroll_event;

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
        fdk_key_event key;
        fdk_pointer_event pointer;
        fdk_pointer_button_event pointer_button;
        fdk_scroll_event scroll;
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
