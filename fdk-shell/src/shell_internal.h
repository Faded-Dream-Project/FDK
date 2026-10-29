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

    /* The output event receiver (0.2.0) — backends fire through
     * fdkshell__fire_output_event(). */
    fdk_shell_output_event_fn output_cb;
    void *output_cb_user;

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

    /* Auto-hide (0.5.0, C-5 / A-08): the reveal-strip model. The
     * effective placement the backends apply is desc OVERLAID with
     * the hidden state: the primary edge's margin grown by (extent -
     * reveal), the exclusive zone forced 0. dwell_ms is the
     * leave-to-hide delay; ah_timer is that dwell. */
    int32_t ah_reveal;      /* 0 = auto-hide off                   */
    uint32_t ah_dwell_ms;
    int ah_hidden;          /* the hidden state is applied          */
    void *ah_timer;         /* fdk_timer*, opaque here              */

    struct fdk_shell_layer *next, *prev; /* the shell's list      */

    void *backend; /* x11_layer_state / wl_layer_state        */
};

struct fdk_shell_popup {
    fdk_shell *shell;
    fdk_ext_surface *ext; /* FDK-OWNED popup extension surface */
    struct fdk_shell_layer *parent; /* dies with its layer       */
    struct fdk_shell_popup *parent_popup; /* cascade chain (0.2.0);
                                           * NULL for layer-rooted  */
    fdk_shell_popup_event_fn cb;
    void *cb_user;
    fdk_size size;

    struct fdk_shell_popup *next, *prev; /* the shell's list     */
};

/* ---- auto-hide helpers (shell.c; the backends apply the slide
 * natively — a position offset on X11, a negative margin on Wayland,
 * where zwlr margins are signed) ---- */

/* The slide edge (the single-anchored axis), 0 when none. */
uint32_t fdkshell__slide_edge(const fdk_shell_layer *layer);
/* The hidden slide amount: extent - reveal (>= 0). */
int32_t fdkshell__slide_amount(const fdk_shell_layer *layer);

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

/* shell.c: backends report output lifecycle changes through this;
 * it routes to the app callback. index is the backend's STABLE
 * record index. */
/* The M-6 test seam (x11.c; internal linkage — the multi-output
 * origin math proven without a multi-output headless server). */
void fdkshell__x11_test_inject_output(fdk_shell *shell, int index,
                                      int x, int y, int w, int h);
void fdkshell__x11_test_clear_outputs(fdk_shell *shell);

void fdkshell__fire_output_event(fdk_shell *shell,
                                 fdk_shell_output_event_type type,
                                 uint32_t index);

/* x11.c */
#if defined(FDKSHELL_BUILD_X11)
/* The X11 output face (0.2.0): RandR output records when RandR is
 * reachable, the whole screen as one output otherwise. */
uint32_t fdkshell__x11_output_count(fdk_shell *shell);
fdk_result fdkshell__x11_output_name(fdk_shell *shell, uint32_t index,
                                     char *buf, size_t buflen);
fdk_result fdkshell__x11_output_get_info(fdk_shell *shell, uint32_t index,
                                          fdk_shell_output_info *out);
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
/* 0.5.0, C-5: applies desc (the caller already folded the auto-hide
 * overlay into it) to the live window: geometry recompute + move/
 * resize + size hints + struts; level changes ride the state-atom
 * exchange. */
void fdkshell__x11_layer_reconfig(fdk_shell_layer *layer);
#endif

/* wl.c */
#if defined(FDKSHELL_BUILD_WAYLAND)
/* The kind's popup_attached hook (wl.c — the layer-shell get_popup
 * adoption; shell.c wires it into the kind ops). */
void shell_kind_popup_attached(fdk_ext_surface *surface,
                               void *native_popup, void *user);
/* Output enumeration + live metrics (shell.c's output API forwards
 * to these). The registry/output listeners fire output events
 * through fdkshell__fire_output_event(). */
uint32_t fdkshell__wl_output_count(fdk_shell *shell);
fdk_result fdkshell__wl_output_name(fdk_shell *shell, uint32_t index,
                                     char *buf, size_t buflen);
fdk_result fdkshell__wl_output_get_info(fdk_shell *shell, uint32_t index,
                                         fdk_shell_output_info *out);
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
/* 0.5.0, C-5: the zwlr double-buffered state — set_anchor/margin/
 * zone/size/keyboard + commit. A LEVEL change rides the role swap
 * (destroy + re-create on the target shell layer, same wl_surface). */
void fdkshell__wl_layer_reconfig(fdk_shell_layer *layer,
                                 int level_changed);
#endif

#endif /* FDKSHELL_INTERNAL_H */
