#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

fdk_result fdk_wayland_register_window(fdk_platform_connection *conn,
                                        fdk_platform_window *pwindow) {
    if (conn->window_count == conn->window_capacity) {
        size_t new_capacity = (conn->window_capacity == 0) ? 4 : conn->window_capacity * 2;
        fdk_platform_window **new_array =
            fdk_realloc(conn->windows, new_capacity * sizeof(fdk_platform_window *));
        if (new_array == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        conn->windows = new_array;
        conn->window_capacity = new_capacity;
    }

    conn->windows[conn->window_count++] = pwindow;
    return FDK_OK;
}

void fdk_wayland_unregister_window(fdk_platform_connection *conn,
                                    fdk_platform_window *pwindow) {
    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i] == pwindow) {
            conn->windows[i] = conn->windows[conn->window_count - 1];
            conn->window_count--;
            /* If this window currently held pointer/keyboard focus,
             * clear it so a stale pointer is never dereferenced by a
             * later input event delivered before the compositor sends
             * a fresh enter/leave for whatever's focused now. */
            if (conn->pointer_focus == pwindow) conn->pointer_focus = NULL;
            if (conn->keyboard_focus == pwindow) conn->keyboard_focus = NULL;
            return;
        }
    }
    FDK_WARN("unregister_window: window not found in registry (double free?)");
}

/* ---- HiDPI: wl_output tracking (Phase 3 completion) --------------------
 *
 * Every wl_output global is bound and its scale event recorded, so a
 * window can derive its scale from the outputs it is displayed on
 * (xdg_toplevel enter/leave carry the wl_output objects). The done
 * event marks the output's initial state complete — wl_output
 * guarantees geometry/mode/scale arrive before done, so scale is
 * trustworthy only after it. Compositors re-send the whole
 * geometry/mode/scale/done sequence when an output's scale CHANGES
 * (e.g. the user drags a monitor slider), and our listener updates
 * the recorded scale in place; windows re-derive their preferred
 * scale on their next configure/enter event, which compositors send
 * around such changes.
 */

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
    /* Position and physical size don't influence scale selection. */
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output,
                        uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh) {
    (void)data;
    (void)output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

static void output_scale(void *data, struct wl_output *output,
                         int32_t factor) {
    fdk_platform_connection *conn = data;
    for (size_t i = 0; i < conn->output_count; i++) {
        if (conn->outputs[i].output == output) {
            if (factor < 1) {
                factor = 1; /* defensive: protocol says >= 1 */
            }
            if (conn->outputs[i].scale != factor && conn->outputs[i].scale != 0) {
                FDK_DEBUG("output scale changed %d -> %d",
                          conn->outputs[i].scale, factor);
            }
            conn->outputs[i].scale = factor;
            return;
        }
    }
}

static void output_done(void *data, struct wl_output *output) {
    (void)data;
    (void)output; /* state complete; nothing deferred was waiting */
}

static const struct wl_output_listener g_output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .scale = output_scale,
    .done = output_done,
};

void fdk_wayland_track_output(fdk_platform_connection *conn,
                              struct wl_output *output) {
    if (conn->output_count == conn->output_capacity) {
        size_t cap = conn->output_capacity == 0 ? 4 : conn->output_capacity * 2;
        /* fdk_reallocarray-style growth with overflow check. */
        if (cap > SIZE_MAX / sizeof(*conn->outputs)) {
            wl_output_destroy(output);
            return;
        }
        void *grown = fdk_realloc(conn->outputs, cap * sizeof(*conn->outputs));
        if (grown == NULL) {
            wl_output_destroy(output); /* fail closed, not half-bound */
            return;
        }
        conn->outputs = grown;
        conn->output_capacity = cap;
    }
    conn->outputs[conn->output_count].output = output;
    conn->outputs[conn->output_count].scale = 1; /* until the scale event */
    conn->output_count++;
    wl_output_add_listener(output, &g_output_listener, conn);
}

/* Releases every tracked output (disconnect path). */
void fdk_wayland_destroy_outputs(fdk_platform_connection *conn) {
    for (size_t i = 0; i < conn->output_count; i++) {
        if (conn->outputs[i].output != NULL) {
            wl_output_destroy(conn->outputs[i].output);
        }
    }
    fdk_free(conn->outputs);
    conn->outputs = NULL;
    conn->output_count = 0;
    conn->output_capacity = 0;
}
