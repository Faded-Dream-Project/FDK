/* wayland_im_vk.c — P3: input-method-v2 + virtual-keyboard first-class.
 *
 * The IM add-on pair: zwp_input_method_manager_v2 mints an input
 * method bound to a seat (the compositor routes text-input-v3 traffic
 * to it); the add-on commits strings/preedit and takes the keyboard
 * grab. zwp_virtual_keyboard_manager_v1 mints a virtual keyboard on a
 * seat (the on-screen keyboard path: keymap + key events). The raw
 * protocol objects return to the add-on (raw-event surface pattern);
 * FDK bridges nothing it does not model.
 */
#define _GNU_SOURCE /* memfd_create under -std=c17 */
#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"
#include "core/alloc_internal.h"

#include <sys/mman.h>
#include <unistd.h>
#include "generated/input-method-unstable-v2-client-protocol.h"
#include "generated/virtual-keyboard-unstable-v1-client-protocol.h"

void *fdk_wayland_input_method_create(fdk_platform_connection *conn,
                                      void *wl_seat) {
    if (conn == NULL || conn->input_method_manager == NULL ||
        wl_seat == NULL) {
        return NULL; /* no manager: the honest UNSUPPORTED path */
    }
    return zwp_input_method_manager_v2_get_input_method(
        conn->input_method_manager, (struct wl_seat *)wl_seat);
}

void fdk_wayland_input_method_destroy(void *im) {
    if (im != NULL) {
        zwp_input_method_v2_destroy((struct zwp_input_method_v2 *)im);
    }
}

void *fdk_wayland_input_method_grab(void *im) {
    return im != NULL ? zwp_input_method_v2_grab_keyboard(
                            (struct zwp_input_method_v2 *)im)
                      : NULL;
}

void fdk_wayland_input_method_commit(void *im, const char *text) {
    if (im == NULL) {
        return;
    }
    struct zwp_input_method_v2 *m = (struct zwp_input_method_v2 *)im;
    if (text != NULL && text[0] != '\0') {
        zwp_input_method_v2_commit_string(m, text);
    }
    zwp_input_method_v2_commit(m, 0); /* serial 0: the manager tracks */
}

void *fdk_wayland_virtual_keyboard_create(fdk_platform_connection *conn,
                                          void *wl_seat,
                                          const char *keymap, fdk_u32 size) {
    if (conn == NULL || conn->virtual_keyboard_manager == NULL ||
        wl_seat == NULL) {
        return NULL;
    }
    struct zwp_virtual_keyboard_v1 *kb =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
            conn->virtual_keyboard_manager, (struct wl_seat *)wl_seat);
    /* No keymap from the caller: install the DEFAULT. The protocol
     * refuses any key event before a keymap exists ("Cannot send a
     * keypress before defining a keymap" — the compositor kills the
     * connection; caught live by the P3 rig), so a keymap-less VK
     * that only dies on its FIRST key call is a trap, not a feature.
     * The default is the compact standard form (evdev codes, the us
     * symbol set) — KEY_Q is <AD01>, matching the rig's emission. */
    if (kb != NULL && (keymap == NULL || size == 0)) {
        static const char default_keymap[] =
            "xkb_keymap {"
            " xkb_keycodes { include \"evdev\" };"
            " xkb_types { include \"basic\" };"
            " xkb_compat { include \"basic\" };"
            " xkb_symbols { include \"pc+us\" };"
            "};";
        keymap = default_keymap;
        size = (fdk_u32)(sizeof default_keymap - 1);
    }
    if (kb != NULL && keymap != NULL && size > 0) {
        int fd = memfd_create("fdk-vkeymap", 0);
        if (fd >= 0) {
            ssize_t w = write(fd, keymap, size);
            (void)w;
            zwp_virtual_keyboard_v1_keymap(kb, 1 /* XKB_LAYOUT_FMT */,
                                           fd, (uint32_t)size);
            close(fd);
        }
    }
    return kb;
}

void fdk_wayland_virtual_keyboard_key(void *kb, fdk_u32 time_ms,
                                      fdk_u32 key, fdk_u32 state) {
    if (kb != NULL) {
        zwp_virtual_keyboard_v1_key((struct zwp_virtual_keyboard_v1 *)kb,
                                    time_ms, key, state);
    }
}
