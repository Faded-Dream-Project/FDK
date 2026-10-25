/*
 * shell_internal.h — fdk-shell internals
 *
 * Not installed. The public face is include/fdk-shell/fdk-shell.h;
 * this file defines the shared objects (fdk_shell handle, layer,
 * popup) and the backend contract the X11 and Wayland halves
 * implement. The backend state pointers stay void* here so this
 * header stays free of Xlib/wayland-client types; the backend .c
 * files own their concrete structs.
 *
 * The compile-time knobs (set by the Makefile):
 *   FDKSHELL_BUILD_X11      — the X11 backend is compiled in
 *   FDKSHELL_BUILD_WAYLAND  — the Wayland backend is compiled in
 * A backend that is compiled out has no functions — the dispatcher
 * refuses with FDK_ERR_UNSUPPORTED before reaching it.
 */

#ifndef FDKSHELL_INTERNAL_H
#define FDKSHELL_INTERNAL_H

#include "fdk-shell/fdk-shell.h"

#include <stdlib.h>
#include <string.h>

/* Logging rides FDK's public logging API (fdk_log.h) with this
 * package's tag — an add-on gets the same leveled, tagged stream
 * the toolkit itself writes to. */
#define FDKSHELL_LOG_TAG "fdk-shell"
#define FDKS_INFO(...) \
    fdk_log(FDK_LOG_INFO, FDKSHELL_LOG_TAG, __VA_ARGS__)
#define FDKS_WARN(...) \
    fdk_log(FDK_LOG_WARN, FDKSHELL_LOG_TAG, __VA_ARGS__)
#define FDKS_ERROR(...) \
    fdk_log(FDK_LOG_ERROR, FDKSHELL_LOG_TAG, __VA_ARGS__)

/* ---- the objects ---------------------------------------------------------- */

struct fdk_shell {
    fdk_context *ctx;
    fdk_ext_surface_kind *kind; /* "fdk-shell.layer", registered    */
    fdk_ext_backend backend;

    /* Per-backend state (owned + defined by the backend .c). */
    void *x11;
    void *wl;

    /* Live surfaces, destroyed by shutdown's sweep (the same
     * teardown-not-delivery policy FDK's own sweeps use). Intrusive
     * doubly-linked lists: layers and popups. */
    struct fdk_shell_layer *layers_head;
    struct fdk_shell_layer *layers_tail;
    struct fdk_shell_popup *popups_head;
    struct fdk_shell_popup *popups_tail;
};

struct fdk_shell_layer {
    fdk_shell *shell;
    fdk_ext_surface *ext; /* FDK's adopted wrapper; NULL mid-create */
    fdk_shell_event_fn cb;
    void *cb_user;
    fdk_size size; /* most recent configure                   */
    fdk_shell_layer_desc desc;

    struct fdk_shell_layer *next, *prev; /* the shell's list      */

    void *backend; /* x11_layer_state / wl_layer_state        */
};

struct fdk_shell_popup {
    fdk_shell *shell;
    fdk_ext_surface *ext; /* FDK-OWNED popup extension surface */
    struct fdk_shell_layer *parent; /* dies with its layer       */
    fdk_shell_popup_event_fn cb;
    void *cb_user;
    fdk_size size;

    struct fdk_shell_popup *next, *prev; /* the shell's list     */
};

/* The pending-layer slot: fdk_shell_layer_create stages the layer
 * (with its desc) here before calling fdk_ext_surface_create — the
 * kind's create callback retrieves it. Single-threaded contract,
 * one create in flight at a time (asserted). */
extern struct fdk_shell_layer *fdkshell__pending_layer;

/* ---- the backend contract --------------------------------------------------
 *
 * layer create/destroy ride the KIND ops (fdk_extend.h):
 *   create  builds the native surface + role, writes the tagged
 *           native handle;
 *   destroy tears the ROLE down (the native surface is FDK's).
 * show/hide/keyboard-grab are backend extras the dispatcher calls.
 * All functions run on FDK's dispatch thread, inside shell.c's
 * calls. */

/* x11.c */
#if defined(FDKSHELL_BUILD_X11)
int fdkshell__x11_supported(fdk_context *ctx);
fdk_result fdkshell__x11_init(fdk_shell *shell);
void fdkshell__x11_shutdown(fdk_shell *shell);
fdk_result fdkshell__x11_layer_create(fdk_shell *shell,
                                       fdk_shell_layer *layer,
                                       fdk_ext_native_handle *out);
void fdkshell__x11_layer_destroy(fdk_shell *shell,
                                  fdk_shell_layer *layer);
void fdkshell__x11_layer_show(fdk_shell_layer *layer);
void fdkshell__x11_layer_hide(fdk_shell_layer *layer);
#endif

/* wl.c */
#if defined(FDKSHELL_BUILD_WAYLAND)
/* The kind's popup_attached hook (wl.c — the layer-shell get_popup
 * adoption; shell.c wires it into the kind ops). */
void shell_kind_popup_attached(fdk_ext_surface *surface,
                               void *native_popup, void *user);
/* Output enumeration (shell.c's output API forwards to these). */
uint32_t fdkshell__wl_output_count(fdk_shell *shell);
fdk_result fdkshell__wl_output_name(fdk_shell *shell, uint32_t index,
                                     char *buf, size_t buflen);
int fdkshell__wl_supported(fdk_context *ctx);
fdk_result fdkshell__wl_init(fdk_shell *shell);
void fdkshell__wl_shutdown(fdk_shell *shell);
fdk_result fdkshell__wl_layer_create(fdk_shell *shell,
                                      fdk_shell_layer *layer,
                                      fdk_ext_native_handle *out);
void fdkshell__wl_layer_destroy(fdk_shell *shell,
                                 fdk_shell_layer *layer);
void fdkshell__wl_layer_show(fdk_shell_layer *layer);
void fdkshell__wl_layer_hide(fdk_shell_layer *layer);
#endif

#endif /* FDKSHELL_INTERNAL_H */
