#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/log_internal.h"

#include <sys/mman.h>
#include <unistd.h>

/* ---- Keyboard ---- */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                             uint32_t format, int32_t fd, uint32_t size) {
    (void)keyboard;
    fdk_platform_connection *conn = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        FDK_WARN("compositor offered non-XKB-v1 keymap format (%u), ignoring", format);
        close(fd);
        return;
    }

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) {
        FDK_ERROR("mmap of keymap fd failed");
        close(fd);
        return;
    }

    struct xkb_keymap *new_keymap = xkb_keymap_new_from_string(
        conn->xkb_context, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    close(fd);

    if (new_keymap == NULL) {
        FDK_ERROR("xkb_keymap_new_from_string failed");
        return;
    }

    struct xkb_state *new_state = xkb_state_new(new_keymap);
    if (new_state == NULL) {
        FDK_ERROR("xkb_state_new failed");
        xkb_keymap_unref(new_keymap);
        return;
    }

    if (conn->xkb_state) xkb_state_unref(conn->xkb_state);
    if (conn->xkb_keymap) xkb_keymap_unref(conn->xkb_keymap);
    conn->xkb_keymap = new_keymap;
    conn->xkb_state = new_state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                            struct wl_surface *surface, struct wl_array *keys) {
    (void)keyboard;
    (void)serial;
    (void)keys;
    fdk_platform_connection *conn = data;

    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i]->surface == surface) {
            conn->keyboard_focus = conn->windows[i];
            fdk_event_data event = { .type = FDK_EVENT_WINDOW_FOCUS };
            event.focus.focused = 1;
            conn->dispatch(conn->windows[i], &event, conn->dispatch_user_data);
            return;
        }
    }
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                            struct wl_surface *surface) {
    (void)keyboard;
    (void)serial;
    (void)surface;
    fdk_platform_connection *conn = data;

    if (conn->keyboard_focus != NULL) {
        fdk_event_data event = { .type = FDK_EVENT_WINDOW_FOCUS };
        event.focus.focused = 0;
        conn->dispatch(conn->keyboard_focus, &event, conn->dispatch_user_data);
    }
    conn->keyboard_focus = NULL;
}

static fdk_u32 xkb_modifiers_to_fdk(struct xkb_state *state) {
    fdk_u32 mods = 0;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= FDK_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= FDK_MOD_CTRL;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= FDK_MOD_ALT;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= FDK_MOD_SUPER;
    return mods;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                          uint32_t time, uint32_t key, uint32_t state) {
    (void)keyboard;
    (void)serial;
    (void)time;
    fdk_platform_connection *conn = data;

    if (conn->keyboard_focus == NULL || conn->xkb_state == NULL) {
        return; /* no focused window or no keymap yet; nothing to report */
    }

    fdk_event_data event;
    event.type = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? FDK_EVENT_KEY_DOWN : FDK_EVENT_KEY_UP;
    /* wl_keyboard reports evdev keycodes directly — no +8 offset the
     * way X11 needs (see x11_events.c's x11_keycode_to_scancode
     * comment). This is exactly why fdk_event.h documents fdk_scancode
     * as evdev-numbered: both backends land on the same space, just
     * via different arithmetic. */
    event.key.scancode = key;
    event.key.modifiers = xkb_modifiers_to_fdk(conn->xkb_state);
    event.key.is_repeat = 0; /* Wayland key-repeat is a client-driven
                                 timer off wl_keyboard::repeat_info,
                                 not implemented in Phase 2 — see
                                 docs/platform-input.md */

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        xkb_keysym_t keysym = xkb_state_key_get_one_sym(conn->xkb_state, key + 8);
        fdk_u32 cp = xkb_keysym_to_utf32(keysym);
        event.key.codepoint = cp;
    } else {
        event.key.codepoint = 0;
    }

    conn->dispatch(conn->keyboard_focus, &event, conn->dispatch_user_data);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                uint32_t mods_depressed, uint32_t mods_latched,
                                uint32_t mods_locked, uint32_t group) {
    (void)keyboard;
    (void)serial;
    fdk_platform_connection *conn = data;
    if (conn->xkb_state == NULL) {
        return;
    }
    xkb_state_update_mask(conn->xkb_state, mods_depressed, mods_latched, mods_locked,
                           0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                  int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
    /* See keyboard_key()'s is_repeat comment — repeat timer not
     * implemented in Phase 2, so this info isn't acted on yet. */
}

static const struct wl_keyboard_listener g_keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ---- Pointer ---- */

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)serial;
    fdk_platform_connection *conn = data;

    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i]->surface == surface) {
            conn->pointer_focus = conn->windows[i];
            conn->pointer_x = wl_fixed_to_double(sx);
            conn->pointer_y = wl_fixed_to_double(sy);
            fdk_event_data event = { .type = FDK_EVENT_POINTER_ENTER };
            conn->dispatch(conn->windows[i], &event, conn->dispatch_user_data);
            return;
        }
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface) {
    (void)pointer;
    (void)serial;
    (void)surface;
    fdk_platform_connection *conn = data;

    if (conn->pointer_focus != NULL) {
        fdk_event_data event = { .type = FDK_EVENT_POINTER_LEAVE };
        conn->dispatch(conn->pointer_focus, &event, conn->dispatch_user_data);
    }
    conn->pointer_focus = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                            wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)time;
    fdk_platform_connection *conn = data;
    conn->pointer_x = wl_fixed_to_double(sx);
    conn->pointer_y = wl_fixed_to_double(sy);

    if (conn->pointer_focus == NULL) {
        return;
    }
    fdk_event_data event = { .type = FDK_EVENT_POINTER_MOTION };
    event.pointer.position.x = (fdk_f32)conn->pointer_x;
    event.pointer.position.y = (fdk_f32)conn->pointer_y;
    conn->dispatch(conn->pointer_focus, &event, conn->dispatch_user_data);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                            uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer;
    (void)serial;
    (void)time;
    fdk_platform_connection *conn = data;
    if (conn->pointer_focus == NULL) {
        return;
    }

    /* Linux evdev button codes (BTN_LEFT=0x110, BTN_RIGHT=0x111,
     * BTN_MIDDLE=0x112) are what wl_pointer reports — translated to
     * FDK's own small enum for the common three; anything else passes
     * through as its raw evdev code, matching X11 backend's handling
     * of "extra" buttons beyond the standard three (x11_events.c). */
    fdk_u32 fdk_button;
    switch (button) {
        case 0x110: fdk_button = FDK_POINTER_BUTTON_LEFT; break;
        case 0x111: fdk_button = FDK_POINTER_BUTTON_RIGHT; break;
        case 0x112: fdk_button = FDK_POINTER_BUTTON_MIDDLE; break;
        default:    fdk_button = button; break;
    }

    fdk_event_data event;
    event.type = (state == WL_POINTER_BUTTON_STATE_PRESSED)
        ? FDK_EVENT_POINTER_BUTTON_DOWN : FDK_EVENT_POINTER_BUTTON_UP;
    event.pointer_button.position.x = (fdk_f32)conn->pointer_x;
    event.pointer_button.position.y = (fdk_f32)conn->pointer_y;
    event.pointer_button.button = fdk_button;
    conn->dispatch(conn->pointer_focus, &event, conn->dispatch_user_data);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                          uint32_t axis, wl_fixed_t value) {
    (void)pointer;
    (void)time;
    fdk_platform_connection *conn = data;
    if (conn->pointer_focus == NULL) {
        return;
    }

    fdk_event_data event = { .type = FDK_EVENT_POINTER_SCROLL };
    event.scroll.position.x = (fdk_f32)conn->pointer_x;
    event.scroll.position.y = (fdk_f32)conn->pointer_y;
    double delta = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        event.scroll.delta_x = 0.0f;
        event.scroll.delta_y = (fdk_f32)-delta; /* Wayland: positive = down;
                                                    fdk_event.h follows the
                                                    X11 backend's convention
                                                    of positive = scroll up,
                                                    see x11_events.c */
    } else {
        event.scroll.delta_x = (fdk_f32)-delta;
        event.scroll.delta_y = 0.0f;
    }
    conn->dispatch(conn->pointer_focus, &event, conn->dispatch_user_data);
}

/* frame/axis_source/axis_stop/axis_discrete: part of the wl_pointer
 * v5+ "grouped axis event" protocol additions. FDK's scroll event
 * model (fdk_event.h) is per-axis-event, not grouped, so these are
 * intentionally no-ops — accepted here only because the listener
 * struct requires every callback to be non-NULL once bound at the
 * version we request (5, see wayland_connection.c's wl_seat bind). */
static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data;
    (void)pointer;
}
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {
    (void)data; (void)pointer; (void)axis_source;
}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener g_pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* ---- Seat capability binding ---- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    fdk_platform_connection *conn = data;

    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && conn->keyboard == NULL) {
        conn->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(conn->keyboard, &g_keyboard_listener, conn);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && conn->keyboard != NULL) {
        wl_keyboard_destroy(conn->keyboard);
        conn->keyboard = NULL;
    }

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && conn->pointer == NULL) {
        conn->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(conn->pointer, &g_pointer_listener, conn);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && conn->pointer != NULL) {
        wl_pointer_destroy(conn->pointer);
        conn->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    FDK_DEBUG("seat name: %s", name);
}

static const struct wl_seat_listener g_seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

void fdk_wayland_bind_seat_listeners(fdk_platform_connection *conn) {
    conn->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (conn->xkb_context == NULL) {
        FDK_ERROR("xkb_context_new failed — keyboard input will not work");
    }
    wl_seat_add_listener(conn->seat, &g_seat_listener, conn);
}

void fdk_wayland_teardown_seat(fdk_platform_connection *conn) {
    if (conn->keyboard) wl_keyboard_destroy(conn->keyboard);
    if (conn->pointer) wl_pointer_destroy(conn->pointer);
    if (conn->seat) wl_seat_destroy(conn->seat);
    if (conn->xkb_state) xkb_state_unref(conn->xkb_state);
    if (conn->xkb_keymap) xkb_keymap_unref(conn->xkb_keymap);
    if (conn->xkb_context) xkb_context_unref(conn->xkb_context);
}
