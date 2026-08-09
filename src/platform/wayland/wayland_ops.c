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
};

const fdk_platform_ops *fdk_platform_wayland_ops(void) {
    return &g_wayland_ops;
}
