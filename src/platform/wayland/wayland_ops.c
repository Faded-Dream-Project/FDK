#include "platform/wayland/wayland_platform.h"

static const fdk_platform_ops g_wayland_ops = {
    .name = "wayland",
    .connect = fdk_wayland_connect,
    .disconnect = fdk_wayland_disconnect,
    .get_event_fd = fdk_wayland_get_event_fd,
    .dispatch_pending = fdk_wayland_dispatch_pending,
    .window_create = fdk_wayland_window_create,
    .window_destroy = fdk_wayland_window_destroy,
    .window_show = fdk_wayland_window_show,
    .window_hide = fdk_wayland_window_hide,
    .window_set_title = fdk_wayland_window_set_title,
    .window_resize = fdk_wayland_window_resize,
    .window_set_size_limits = fdk_wayland_window_set_size_limits,
    .window_set_wm_decorations = fdk_wayland_window_set_wm_decorations,
    .window_set_maximized = fdk_wayland_window_set_maximized,
    .window_set_minimized = fdk_wayland_window_set_minimized,
    .window_begin_move = fdk_wayland_window_begin_move,
    .window_begin_resize = fdk_wayland_window_begin_resize,
    .window_query_pointer = fdk_wayland_window_query_pointer,
    .window_set_cursor = fdk_wayland_window_set_cursor,
    .window_get_framebuffer = fdk_wayland_window_get_framebuffer,
    .window_present = fdk_wayland_window_present,
    .window_frame_ready = fdk_wayland_window_frame_ready,
    .window_ever_presented = fdk_wayland_window_ever_presented,
    .window_get_scale = fdk_wayland_window_get_scale,
    .clipboard_set_text = fdk_wayland_clipboard_set_text,
    .clipboard_get_text = fdk_wayland_clipboard_get_text,
    .window_set_drop_formats = fdk_wayland_window_set_drop_formats,
    .drag_begin = fdk_wayland_drag_begin,
};

const fdk_platform_ops *fdk_platform_wayland_ops(void) {
    return &g_wayland_ops;
}
