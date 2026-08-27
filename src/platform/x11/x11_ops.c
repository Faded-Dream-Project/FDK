#include "platform/x11/x11_platform.h"

static const fdk_platform_ops g_x11_ops = {
    .name = "x11",
    .connect = fdk_x11_connect,
    .disconnect = fdk_x11_disconnect,
    .get_event_fd = fdk_x11_get_event_fd,
    .dispatch_pending = fdk_x11_dispatch_pending,
    .window_create = fdk_x11_window_create,
    .window_destroy = fdk_x11_window_destroy,
    .window_show = fdk_x11_window_show,
    .window_hide = fdk_x11_window_hide,
    .window_set_title = fdk_x11_window_set_title,
    .window_resize = fdk_x11_window_resize,
    .window_set_size_limits = fdk_x11_window_set_size_limits,
    .window_set_wm_decorations = fdk_x11_window_set_wm_decorations,
    .window_get_position = fdk_x11_window_get_position,
    .window_move_to = fdk_x11_window_move_to,
    .window_move_resize_to = fdk_x11_window_move_resize_to,
    .window_set_maximized = fdk_x11_window_set_maximized,
    .window_set_minimized = fdk_x11_window_set_minimized,
    .window_begin_move = fdk_x11_window_begin_move,
    .window_begin_resize = fdk_x11_window_begin_resize,
    .window_get_framebuffer = fdk_x11_window_get_framebuffer,
    .window_present = fdk_x11_window_present,
};

const fdk_platform_ops *fdk_platform_x11_ops(void) {
    return &g_x11_ops;
}
