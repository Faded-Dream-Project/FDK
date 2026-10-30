#include "platform/x11/x11_platform.h"

/* The pump's platform-deadline source (1.1.0-rc1): the GL lane's
 * pending self-check retry. X11's other timing (key repeat) is
 * server-side, so this is the only self-timed state the backend
 * holds — and it exists only while a probe verdict is pending. */
static int fdk_x11_next_wakeup_ms(fdk_platform_connection *conn,
                                  long long *out_ms_until) {
    if (conn == NULL) {
        return 0;
    }
    return fdk_x11_gl_selfcheck_next_wakeup(conn->gl, out_ms_until);
}

static const fdk_platform_ops g_x11_ops = {
    .name = "x11",
    .connect = fdk_x11_connect,
    .disconnect = fdk_x11_disconnect,
    .get_event_fd = fdk_x11_get_event_fd,
    .dispatch_pending = fdk_x11_dispatch_pending,
    .next_wakeup_ms = fdk_x11_next_wakeup_ms,
    .window_create = fdk_x11_window_create,
    .window_destroy = fdk_x11_window_destroy,
    .window_show = fdk_x11_window_show,
    .window_hide = fdk_x11_window_hide,
    .window_set_title = fdk_x11_window_set_title,
    .window_set_icon = fdk_x11_window_set_icon,
    .window_set_role = fdk_x11_window_set_role,
    .window_set_command = fdk_x11_window_set_command, /* D-5: X11's
        session lane is WM_COMMAND + WM_SAVE_YOURSELF; the session
        object ops (session_*, window_join/leave_session) stay NULL
        — the public open answers the documented NULL on X11. */
    .window_resize = fdk_x11_window_resize,
    .window_set_size_limits = fdk_x11_window_set_size_limits,
    .window_set_wm_decorations = fdk_x11_window_set_wm_decorations,
    .window_get_position = fdk_x11_window_get_position,
    .window_move_to = fdk_x11_window_move_to,
    .window_move_resize_to = fdk_x11_window_move_resize_to,
    .window_set_maximized = fdk_x11_window_set_maximized,
    .window_set_fullscreen = fdk_x11_window_set_fullscreen,
    .window_set_minimized = fdk_x11_window_set_minimized,
    .window_begin_move = fdk_x11_window_begin_move,
    .window_begin_resize = fdk_x11_window_begin_resize,
    .window_query_pointer = fdk_x11_window_query_pointer,
    .window_set_cursor = fdk_x11_window_set_cursor,
    .window_set_input_region = fdk_x11_window_set_input_region,
    .window_activate = fdk_x11_window_activate,
    .window_activation_token = fdk_x11_window_activation_token,
    .window_get_framebuffer = fdk_x11_window_get_framebuffer,
    .window_present = fdk_x11_window_present,
    .window_ever_presented = fdk_x11_window_ever_presented,
    .window_get_scale = fdk_x11_window_get_scale,
    .window_render_path = fdk_x11_window_render_path,
    .clipboard_set_text = fdk_x11_clipboard_set_text,
    .clipboard_get_text = fdk_x11_clipboard_get_text,
    .clipboard_set_primary_text = fdk_x11_clipboard_set_primary_text,
    .clipboard_get_primary_text = fdk_x11_clipboard_get_primary_text,
    .clipboard_set_image = fdk_x11_clipboard_set_image,
    .clipboard_get_image = fdk_x11_clipboard_get_image,
    .window_popup_regrab = fdk_x11_window_popup_regrab,
    .window_set_modal = fdk_x11_window_set_modal,
    .window_set_drop_formats = fdk_x11_window_set_drop_formats,
    .drag_begin = fdk_x11_drag_begin,
    .ext_surface_adopt = fdk_x11_ext_surface_adopt,
    .native_display = fdk_x11_native_display,
    .ext_surface_native = fdk_x11_ext_surface_native,
    .ext_surface_resized = fdk_x11_ext_surface_resized,
    .subsurface_create = fdk_x11_subsurface_create,
    .subsurface_set_position = fdk_x11_subsurface_set_position,
    .subsurface_resize = fdk_x11_subsurface_resize,
    .subsurface_set_desync = fdk_x11_subsurface_set_desync,
    .subsurface_place = fdk_x11_subsurface_place,
    .toplevels_open = fdk_x11_toplevels_open,
    .toplevels_close = fdk_x11_toplevels_close,
    .toplevels_count = fdk_x11_toplevels_count,
    .toplevels_at = fdk_x11_toplevels_at,
    .toplevels_activate = fdk_x11_toplevels_activate,
    .toplevels_close_window = fdk_x11_toplevels_close_window,
};

const fdk_platform_ops *fdk_platform_x11_ops(void) {
    return &g_x11_ops;
}
