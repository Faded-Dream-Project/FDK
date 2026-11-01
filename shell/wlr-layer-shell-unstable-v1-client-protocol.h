/*
 * wlr-layer-shell-unstable-v1-client-protocol.h — the
 * zwlr_layer_shell_v1 protocol, hand-written in
 * wayland-scanner's client-header shape (the same form as the
 * other vendored headers in src/platform/wayland/generated/,
 * which are also committed pre-generated because the build
 * environment does not require wayland-scanner).
 *
 * Interface: wlr-layer-shell-unstable-v1, version 1 (the version
 * every compositor implements; keyboard_interactivity on-demand
 * is v4 and appears here only as the enum value guarded by
 * version checks at the call sites).
 *
 * This file is part of fdk-shell (shell/), NOT of FDK core: it is
 * the add-on's own protocol vocabulary, kept next to the add-on
 * that speaks it.
 */

#ifndef WLR_LAYER_SHELL_UNSTABLE_V1_CLIENT_PROTOCOL_H
#define WLR_LAYER_SHELL_UNSTABLE_V1_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-client.h"

#ifdef __cplusplus
extern "C" {
#endif

struct wl_compositor;
struct wl_output;
struct wl_surface;
struct xdg_popup;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

extern const struct wl_interface zwlr_layer_shell_v1_interface;
extern const struct wl_interface zwlr_layer_surface_v1_interface;

enum zwlr_layer_shell_v1_request {
	ZWLR_LAYER_SHELL_V1_DESTROY = 0,
	ZWLR_LAYER_SHELL_V1_GET_LAYER_SURFACE = 1,
};

enum zwlr_layer_surface_v1_request {
	ZWLR_LAYER_SURFACE_V1_SET_SIZE = 0,
	ZWLR_LAYER_SURFACE_V1_SET_ANCHOR = 1,
	ZWLR_LAYER_SURFACE_V1_SET_EXCLUSIVE_ZONE = 2,
	ZWLR_LAYER_SURFACE_V1_SET_MARGIN = 3,
	ZWLR_LAYER_SURFACE_V1_SET_KEYBOARD_INTERACTIVITY = 4,
	ZWLR_LAYER_SURFACE_V1_GET_POPUP = 5,
	ZWLR_LAYER_SURFACE_V1_ACK_CONFIGURE = 6,
	ZWLR_LAYER_SURFACE_V1_DESTROY = 7,
};

enum zwlr_layer_shell_v1_layer {
	ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND = 0,
	ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM = 1,
	ZWLR_LAYER_SHELL_V1_LAYER_TOP = 2,
	ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY = 3,
};

enum zwlr_layer_surface_v1_anchor {
	ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP = 1,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM = 2,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT = 4,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT = 8,
};

enum zwlr_layer_surface_v1_keyboard_interactivity {
	ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE = 0,
	ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE = 1,
	ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND = 2, /* v4 */
};

#define ZWLR_LAYER_SHELL_V1_GET_LAYER_SURFACE_SINCE_VERSION 1

static inline struct zwlr_layer_surface_v1 *
zwlr_layer_shell_v1_get_layer_surface(
	struct zwlr_layer_shell_v1 *zwlr_layer_shell_v1,
	struct wl_surface *surface, struct wl_output *output,
	uint32_t layer, const char *namespace_)
{
	struct wl_proxy *id;
	id = wl_proxy_marshal_flags((struct wl_proxy *) zwlr_layer_shell_v1,
		ZWLR_LAYER_SHELL_V1_GET_LAYER_SURFACE,
		&zwlr_layer_surface_v1_interface, wl_proxy_get_version(
			(struct wl_proxy *) zwlr_layer_shell_v1),
		WL_MARSHAL_FLAG_DESTROY, surface, output, layer,
		namespace_);
	return (struct zwlr_layer_surface_v1 *) id;
}

#define ZWLR_LAYER_SURFACE_V1_SET_SIZE_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_SET_ANCHOR_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_SET_EXCLUSIVE_ZONE_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_SET_MARGIN_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_SET_KEYBOARD_INTERACTIVITY_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_GET_POPUP_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_ACK_CONFIGURE_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_DESTROY_SINCE_VERSION 1

static inline void
zwlr_layer_surface_v1_set_size(struct zwlr_layer_surface_v1 *p,
                               int32_t width, int32_t height)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_SET_SIZE, NULL, wl_proxy_get_version(
			(struct wl_proxy *) p), WL_MARSHAL_FLAG_DESTROY,
		width, height);
}

static inline void
zwlr_layer_surface_v1_set_anchor(struct zwlr_layer_surface_v1 *p,
                                  uint32_t anchor)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_SET_ANCHOR, NULL,
		wl_proxy_get_version((struct wl_proxy *) p),
		WL_MARSHAL_FLAG_DESTROY, anchor);
}

static inline void
zwlr_layer_surface_v1_set_exclusive_zone(struct zwlr_layer_surface_v1 *p,
                                         int32_t zone)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_SET_EXCLUSIVE_ZONE, NULL,
		wl_proxy_get_version((struct wl_proxy *) p),
		WL_MARSHAL_FLAG_DESTROY, zone);
}

static inline void
zwlr_layer_surface_v1_set_margin(struct zwlr_layer_surface_v1 *p,
                                 int32_t top, int32_t right,
                                 int32_t bottom, int32_t left)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_SET_MARGIN, NULL,
		wl_proxy_get_version((struct wl_proxy *) p),
		WL_MARSHAL_FLAG_DESTROY, top, right, bottom, left);
}

static inline void
zwlr_layer_surface_v1_set_keyboard_interactivity(
	struct zwlr_layer_surface_v1 *p, uint32_t keyboard_interactivity)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_SET_KEYBOARD_INTERACTIVITY, NULL,
		wl_proxy_get_version((struct wl_proxy *) p),
		WL_MARSHAL_FLAG_DESTROY, keyboard_interactivity);
}

static inline void
zwlr_layer_surface_v1_get_popup(struct zwlr_layer_surface_v1 *p,
                                struct xdg_popup *popup)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_GET_POPUP, NULL,
		wl_proxy_get_version((struct wl_proxy *) p),
		WL_MARSHAL_FLAG_DESTROY, popup);
}

static inline void
zwlr_layer_surface_v1_ack_configure(struct zwlr_layer_surface_v1 *p,
                                    uint32_t serial)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_ACK_CONFIGURE, NULL,
		wl_proxy_get_version((struct wl_proxy *) p),
		WL_MARSHAL_FLAG_DESTROY, serial);
}

static inline void
zwlr_layer_surface_v1_destroy(struct zwlr_layer_surface_v1 *p)
{
	wl_proxy_marshal_flags((struct wl_proxy *) p,
		ZWLR_LAYER_SURFACE_V1_DESTROY, NULL,
		wl_proxy_get_version((struct wl_proxy *) p),
		WL_MARSHAL_FLAG_DESTROY);
}

enum zwlr_layer_surface_v1_error {
	ZWLR_LAYER_SURFACE_V1_ERROR_ALREADY_CONSTRUCTED = 1,
	ZWLR_LAYER_SURFACE_V1_ERROR_ALREADY_CONFIGURED = 2,
};

struct zwlr_layer_surface_v1_listener {
	void (*configure)(void *data,
			  struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1,
			  uint32_t serial, uint32_t width, uint32_t height);
	void (*closed)(void *data,
		       struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1);
};

static inline int
zwlr_layer_surface_v1_add_listener(
	struct zwlr_layer_surface_v1 *p,
	const struct zwlr_layer_surface_v1_listener *l, void *data)
{
	return wl_proxy_add_listener((struct wl_proxy *) p,
		(void (**)(void)) l, data);
}

#define ZWLR_LAYER_SURFACE_V1_CONFIGURE_SINCE_VERSION 1
#define ZWLR_LAYER_SURFACE_V1_CLOSED_SINCE_VERSION 1

#ifdef __cplusplus
}
#endif

#endif /* WLR_LAYER_SHELL_UNSTABLE_V1_CLIENT_PROTOCOL_H */
