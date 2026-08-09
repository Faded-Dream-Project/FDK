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
};

const fdk_platform_ops *fdk_platform_x11_ops(void) {
    return &g_x11_ops;
}
