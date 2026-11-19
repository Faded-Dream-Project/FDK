/*
 * fdk_gl.h — application-owned OpenGL inside FDK windows (1.0.0)
 *
 * Part of the public FDK API.
 *
 * THE GAP THIS CLOSES (the 2026-11-18 directive): before this header
 * the only application-owned GL path was the add-on surface
 * (fdk_ext_gl_* in fdk_extend.h, which takes an fdk_ext_surface).
 * An application with an ordinary fdk_window — the normal case —
 * had NO public way to render with its own OpenGL calls into that
 * window, or into a rectangular region of it (a 3D viewport inside
 * an application window with FDK-drawn widgets around it). And the
 * GL lane engaged lazily at the first framebuffer acquisition, so
 * fdk_ext_gl_make_current answered the SAME FDK_ERR_UNSUPPORTED
 * for "not yet engaged", "window below the size gate", and "this
 * backend can never do it" — the caller could not tell them apart.
 *
 * This header adds the two complementary halves:
 *
 *   1. fdk_window_gl_* — window-level engage/query. An application
 *      explicitly engages the lane for a plain window BEFORE any
 *      present (no throwaway-present dance), asks WHY when the
 *      answer is no (fdk_gl_status), and gets the version, vendor
 *      and renderer of the context it will run on
 *      (fdk_window_gl_info).
 *
 *   2. The GL viewport widget (fdk_gl_viewport_create) — the
 *      composition half. It owns a rectangular region of an
 *      ordinary window's widget tree, requests frames from the
 *      application on demand, calls the application's render
 *      callback with the window's lane context current and the
 *      viewport/scale already set, and composites in the tree's
 *      paint order — so menus, popups and tooltips that overlap
 *      the GL region draw OVER it (they are later in z-order), and
 *      pointer events keep reaching the right widget. This is the
 *      canvas widget's shape (src/widget/canvas.c) carried to GL.
 *
 * CONTEXT FLAVOR: where the driver offers it, the engaged context
 * is a CORE-PROFILE desktop OpenGL 3.3 or newer context
 * (glXCreateContextAttribsARB on X11, EGL_KHR_create_context on
 * Wayland); where it does not, the lane falls back to the same
 * legacy context it always used and the flavor is reported
 * honestly through fdk_gl_info.flavor. The application's render
 * callback must be prepared for either (load its own functions
 * through the platform loader; FDK deliberately does not ship a
 * GL loader — the lane itself resolves only the tiny subset it
 * needs, the documented house pattern).
 *
 * ABI: everything here is additive (new header, new functions, new
 * appended enum values); the SONAME stays libfdk.so.1 per
 * docs/abi-policy.md.
 */

#ifndef FDK_GL_H
#define FDK_GL_H

#include "fdk_types.h"
#include "fdk_error.h"
#include "fdk_core.h"   /* fdk_context, fdk_render_mode */
#include "fdk_window.h"
#include "fdk_widget.h"
#include "fdk_surface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Why the answer is no (STEP 2: diagnosable failures) ------------------
 *
 * Every refusal of a GL call names its reason through this enum.
 * The distinction this buys over a bare FDK_ERR_UNSUPPORTED:
 * FDK_GL_REASON_NOT_YET and FDK_GL_REASON_SIZE_GATE are states an
 * APPLICATION can act on (engage explicitly, or grow the window /
 * set FDK_GL_FORCE), while FDK_GL_REASON_BACKEND and
 * FDK_GL_REASON_DRIVER are terminal for the process. The reasons
 * are also logged (INFO level, once per state transition) so a
 * field report carries the why in plain text. */
typedef enum fdk_gl_reason {
    /* The lane can and does serve this window right now. */
    FDK_GL_REASON_OK = 0,
    /* The lane is available but has not engaged yet — the window
     * has not presented (nobody asked for a framebuffer) and no
     * explicit fdk_window_gl_engage() ran. Act: engage, or present
     * once. This is the "not yet" that used to be
     * indistinguishable from "never". */
    FDK_GL_REASON_NOT_YET = 1,
    /* The window is below the lane's small-surface gate
     * (FDK_GL_MIN_PIXELS, 131072 px). Act: resize the window,
     * or set FDK_GL_FORCE=1, or engage explicitly (an explicit
     * engage overrides the size gate — the application asked). */
    FDK_GL_REASON_SIZE_GATE = 2,
    /* The window's render mode is FDK_RENDER_CPU — the
     * application (or context) pinned it off the GPU lanes. */
    FDK_GL_REASON_CPU_MODE = 3,
    /* The driver/connection refused: software renderer without
     * FDK_GL_FORCE, no usable FBConfig/EGLConfig, context
     * creation failed, or a self-check failure marked the lane
     * refused. Terminal for this process. */
    FDK_GL_REASON_DRIVER = 4,
    /* The backend has no GL lane at all (the Wayland-disabled
     * build's X11-only counterpart is fine; this is "backend
     * without GL"), or the window is on no connection. */
    FDK_GL_REASON_BACKEND = 5,
    /* The lane engaged once but was downgraded mid-session
     * (present failure, context loss). Permanent for this
     * window; a NEW window on the same connection retries. */
    FDK_GL_REASON_DOWNGRADED = 6
} fdk_gl_reason;

/* The status record fdk_gl_status() fills. Zero-initialized
 * callers get reason = FDK_GL_REASON_BACKEND (the most honest
 * "know nothing" answer), which is also what a NULL window or
 * dead connection reports. */
typedef struct fdk_gl_status_data {
    /* Why the lane serves or refuses this window right now. */
    fdk_gl_reason reason;
    /* The window's current pixel area (the size-gate input, for
     * the caller's own diagnostics). */
    fdk_i64 pixel_area;
    /* The lane's small-surface gate on this build (the constant
     * the reason refers to; 131072 on both backends). */
    fdk_i64 size_gate;
    /* Nonzero when the process set FDK_GL_FORCE=1 (the override
     * the SIZE_GATE reason suggests). */
    int force_override;
    /* Nonzero when the window's render mode is the blocker
     * (mirrors reason == FDK_GL_REASON_CPU_MODE). */
    int cpu_mode;
} fdk_gl_status_data;

/* The context identity an engaged lane reports. Strings are
 * pointers into the lane's own storage — valid until the
 * connection dies; copy them if you keep them. "<unknown>" when
 * the field was never read (the lane logs it at engagement; this
 * struct is the API-facing copy). */
typedef struct fdk_gl_info {
    /* Major.minor of the ACTUAL context in use (e.g. 4.5 for
     * Mesa's compatibility profile on llvmpipe, 4.6 core on the
     * GTX 1050 Ti when the driver offers core). 0.0 = the flavor
     * line below is the only identity available. */
    int version_major;
    int version_minor;
    /* The context flavor the driver actually gave us:
     *   "core"     core-profile desktop GL 3.3+ (what 3D apps want)
     *   "compat"   legacy desktop GL (compatibility profile)
     *   "gles2"    OpenGL ES 2.0 (the Wayland lane's fallback)
     *   "none"     no context on this window
     */
    char flavor[8];
    const char *vendor;    /* glGetString(GL_VENDOR)   */
    const char *renderer;  /* glGetString(GL_RENDERER) */
    const char *version;   /* glGetString(GL_VERSION)  */
} fdk_gl_info;

/* ---- Window-level engage/query ------------------------------------------
 *
 * fdk_window_gl_engage: ask the lane to serve THIS window now —
 * before any present. This is the documented replacement for the
 * throwaway-present dance examples/22_ext_complete.c had to do
 * (its "Engage the lane FIRST" comment). An explicit engage:
 *   - overrides the small-surface size gate (the application
 *     asked; a 300x180 window may have its GL),
 *   - respects every other verdict (CPU render mode, driver
 *     refusal, FDK_NO_GL) and reports the reason on failure,
 *   - is idempotent: engaging an already-engaged window returns
 *     FDK_OK with no side effects.
 * Can fail with FDK_ERR_INVALID_ARGUMENT (NULL window),
 * FDK_ERR_NOT_INITIALIZED (no connection), or any FDK_ERR_* the
 * backend's lane init returns (the reason enum carries the WHY;
 * the log line carries the detail). */
fdk_result fdk_window_gl_engage(fdk_window *window);

/* Why the lane serves or refuses this window, right now. Reads
 * live state: mid-session downgrades surface honestly. Never
 * fails on a non-NULL out_status; a NULL window returns
 * FDK_ERR_INVALID_ARGUMENT. */
fdk_result fdk_gl_status(fdk_window *window,
                         fdk_gl_status_data *out_status);

/* The engaged context's identity (version/flavor/vendor/
 * renderer). Zeroed with reason-flavor "none" when the lane is
 * not engaged. Never fails on non-NULL arguments. */
fdk_result fdk_window_gl_info(fdk_window *window, fdk_gl_info *out_info);

/* ---- The GL viewport widget ----------------------------------------------
 *
 * A rectangular region of an ordinary window rendered by the
 * application's own OpenGL code, compositing with the FDK widget
 * tree in paint order:
 *
 *   - The application creates the viewport like any widget
 *     (fdk_gl_viewport_create(parent, ...)), lays it out with the
 *     normal box/grid machinery, and renders in the callback.
 *   - FDK engages the window's lane at the FIRST frame the
 *     viewport is on screen (the window-level engage above is
 *     available for apps that want it earlier).
 *   - On every frame the viewport owes (a resize, an explicit
 *     fdk_gl_viewport_request_frame, or the window's own damage
 *     walk touching the region), FDK:
 *       1. makes the lane context current on the window,
 *       2. sets glViewport to the region (physical pixels — the
 *          window scale already applied),
 *       3. calls the application's render callback,
 *       4. composites the region with the window's frame and
 *          presents through the window's normal path — the rest
 *          of the window is CPU-rasterized; menus, popups and
 *          tooltips that overlap the region draw OVER the GL
 *          content because they paint later in z-order,
 *       5. reports context loss honestly if the lane downgraded.
 *   - The callback receives the LOGICAL size (what the app should
 *     project into) and the window scale. It draws with its own
 *     GL code — depth test, shaders, everything the context
 *     flavor allows — and returns; FDK never draws GL on the
 *     application's behalf.
 *
 * FRAME MODEL: application-driven. A viewport renders ONLY when
 * something changed (widget damage, resize) or when the
 * application asks (fdk_gl_viewport_request_frame — the animated
 * 3D app's per-tick call). No forced continuous redraw, ever.
 * Vsync follows the window's present (the platform swap); an
 * application that wants to throttle asks for frames on a timer.
 *
 * CONTEXT LOSS: if the lane downgrades mid-session (driver
 * refusal, present failure), the viewport stops calling the
 * render callback and reports FDK_GL_REASON_DOWNGRADED through
 * the status query, and the CONTEXT_LOST callback (if set) fires
 * exactly once. The widget keeps its place in the layout (the
 * region falls back to the themed background) — no crash, no
 * leak, honest reporting. */

typedef struct fdk_gl_viewport fdk_gl_viewport;

/* The render callback. ctx-current state: the lane context is
 * current on the window when this runs; the callback may assume
 * glViewport was set to the region (it receives width/height
 * LOGICAL — project accordingly) and may load its own extension
 * functions through the platform's loader. It must NOT swap (FDK
 * owns the present), must not destroy widgets, and must not call
 * back into FDK rendering APIs (draw and return; the same
 * "draw only" contract as the canvas paint callback). */
typedef void (*fdk_gl_render_fn)(fdk_widget *viewport,
                                 int width, int height,
                                 fdk_f32 scale, void *user_data);

/* Fires once if the window's GL lane downgrades after the
 * viewport engaged (the honest context-loss report). After it
 * fires the render callback is no longer invoked. May be NULL. */
typedef void (*fdk_gl_context_lost_fn)(fdk_widget *viewport,
                                       void *user_data);

/* Creates the viewport widget. Fails with
 * FDK_ERR_INVALID_ARGUMENT (NULL parent/out), FDK_ERR_UNSUPPORTED
 * (the backend has no GL lane — e.g. the window is on no
 * connection), or FDK_ERR_OUT_OF_MEMORY. Creating the widget does
 * NOT engage the lane (the first frame does, or an explicit
 * fdk_window_gl_engage); the widget is usable on any window. */
fdk_result fdk_gl_viewport_create(fdk_widget *parent,
                                  fdk_gl_render_fn on_render,
                                  void *user_data,
                                  fdk_widget **out_viewport);

/* Replaces the render callback (NULL = no rendering; the region
 * shows the themed background). */
void fdk_gl_viewport_set_render_callback(fdk_widget *viewport,
                                         fdk_gl_render_fn on_render,
                                         void *user_data);

/* Sets the context-loss callback (NULL removes it). */
void fdk_gl_viewport_set_context_lost_callback(
    fdk_widget *viewport, fdk_gl_context_lost_fn on_lost,
    void *user_data);

/* Requests one frame of the GL region at the next paint — the
 * animated-scene heartbeat (call it from your timer, as many
 * times as you have animation steps; the requests coalesce). This
 * is the ONLY redraw trigger besides widget damage and resizes:
 * no continuous rendering happens on its own. */
void fdk_gl_viewport_request_frame(fdk_widget *viewport);

/* The viewport's current GL status (the same reason enum the
 * window query answers; FDK_GL_REASON_OK means the next paint
 * will call the render callback). */
fdk_result fdk_gl_viewport_status(fdk_widget *viewport,
                                  fdk_gl_status_data *out_status);

#ifdef __cplusplus
}
#endif

#endif /* FDK_GL_H */
