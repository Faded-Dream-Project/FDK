/*
 * wlr-layer-shell-unstable-v1-protocol.c — the interface records
 * for shell/wlr-layer-shell-unstable-v1-client-protocol.h, in the
 * same shape as src/platform/wayland/generated/-protocol.c (the
 * build never requires wayland-scanner; protocol code is
 * committed). Compiled only in Wayland-enabled builds (the X11
 * leg of fdk-shell does not speak this protocol).
 *
 * Request and event opcodes are the protocol's fixed wire order:
 *   zwlr_layer_shell_v1: destroy, get_layer_surface.
 *   zwlr_layer_surface_v1 requests: set_size, set_anchor,
 *     set_exclusive_zone, set_margin,
 *     set_keyboard_interactivity, get_popup, ack_configure,
 *     destroy; events: configure, closed.
 * The get_layer_surface signature is "no?us" (new_id, object,
 * nullable object, uint, string); get_popup's is "o" with the
 * xdg_popup type.
 */

#include "fdk_shell_build.h" /* FDK_SHELL_WAYLAND gate */

#if FDK_SHELL_WAYLAND

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdint.h>

#ifndef __has_attribute
# define __has_attribute(x) 0 /* compatibility with non-clang    */
#endif

#if (__has_attribute(visibility) || defined(__GNUC__) && __GNUC__ >= 4)
#define WL_PRIVATE __attribute__((visibility("hidden")))
#else
#define WL_PRIVATE
#endif

extern const struct wl_interface wl_output_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface xdg_popup_interface;

static const struct wl_interface *zwlr_layer_shell_types[] = {
	NULL,
	&zwlr_layer_surface_v1_interface,
	&wl_surface_interface,
	&wl_output_interface,
	NULL,
	&xdg_popup_interface,
};

static const struct wl_message
zwlr_layer_shell_v1_requests[] = {
	{ "destroy", "", zwlr_layer_shell_types + 0 },
	{ "get_layer_surface", "no?us", zwlr_layer_shell_types + 1 },
};

WL_PRIVATE const struct wl_interface zwlr_layer_shell_v1_interface = {
	"zwlr_layer_shell_v1", 4,
	2, zwlr_layer_shell_v1_requests,
	0, NULL,
};

static const struct wl_message
zwlr_layer_surface_v1_requests[] = {
	{ "set_size", "ii", zwlr_layer_shell_types + 0 },
	{ "set_anchor", "u", zwlr_layer_shell_types + 0 },
	{ "set_exclusive_zone", "i", zwlr_layer_shell_types + 0 },
	{ "set_margin", "iiii", zwlr_layer_shell_types + 0 },
	{ "set_keyboard_interactivity", "u", zwlr_layer_shell_types + 0 },
	{ "get_popup", "o", zwlr_layer_shell_types + 5 },
	{ "ack_configure", "u", zwlr_layer_shell_types + 0 },
	{ "destroy", "", zwlr_layer_shell_types + 0 },
};

static const struct wl_message
zwlr_layer_surface_v1_events[] = {
	{ "configure", "uii", zwlr_layer_shell_types + 0 },
	{ "closed", "", zwlr_layer_shell_types + 0 },
};

WL_PRIVATE const struct wl_interface zwlr_layer_surface_v1_interface = {
	"zwlr_layer_surface_v1", 4,
	8, zwlr_layer_surface_v1_requests,
	2, zwlr_layer_surface_v1_events,
};

#else /* !FDK_SHELL_WAYLAND: the X11 leg speaks no protocol; one
       * harmless exported symbol keeps the translation unit
       * non-empty for -Wpedantic. */
int fdk_shell_wayland_protocol_unused;
#endif /* FDK_SHELL_WAYLAND */
