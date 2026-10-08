/*
 * fdk_animation.h — time-based animation and easing (1.3.8)
 *
 * The polish half of the interaction contract: things that MOVE
 * should move like physical objects, not teleport. FDK's answer is
 * deliberately small — an easing library (pure math, no state) and
 * ONE animator: "drive a number from 0 to 1 over a duration, call
 * me per frame, tell me when it's over." Everything animated is
 * animated THROUGH that number: the callback sets whatever real
 * property needs setting (a scroll offset, a position, a blend
 * factor) and the animator invalidates the widget; the damage
 * tracker repaints exactly what moved.
 *
 * EASING. The curve family is a curated set, not a registry:
 * linear, smoothstep, the quad/cubic in/out/in-out triples, sine
 * in-out, expo out, and back_out (the slight overshoot that makes
 * reveals feel mechanical-but-alive). Every curve maps [0,1] to
 * [0,1] with e(0)=0 and e(1)=1; back_out exceeds 1 mid-flight on
 * purpose (the overshoot IS the effect). Inputs are clamped.
 * fdk_ease_apply() never fails — an unknown kind is linear (the
 * never-an-error posture of a math function).
 *
 * THE ANIMATOR. fdk_widget_animate() attaches a single-shot
 * animation to a widget: duration, easing, a per-frame tick and an
 * optional done callback. Frames tick at the display cadence the
 * timer queue already drives (one shared ~16 ms ticker per
 * process, created when the first animation starts and removed
 * when the last ends — an idle toolkit costs nothing). The tick
 * receives the EASED progress e in [0,1] (or slightly past 1 for
 * back_out); it is called on the first frame after start with
 * e = ease(elapsed/duration), and the animation completes when
 * elapsed >= duration, after which done(finished = true) fires
 * exactly once.
 *
 * CANCEL SEMANTICS. fdk_animation_cancel() stops a running
 * animation and fires done(finished = false) — the callback's
 * chance to snap the visual to its intended end state (or reverse
 * it; policy is the application's). Destroying the animating
 * widget drops its animations WITHOUT any callback — a dying
 * widget must not be touched by animators mid-funeral (the
 * use-after-free guard; tie user_data's lifetime to the widget,
 * or cancel explicitly in your own teardown). Starting a new
 * animation on the same widget does NOT cancel the old one —
 * callers compose (the ScrollView cancels its in-flight scroll
 * before retargeting; that is the right layer for that policy).
 *
 * HANDLE LIFETIME. A finished (or canceled) animation's handle
 * stays a valid QUERY object for a grace period of roughly a
 * second of animation traffic (running() = false, widget()
 * answers, cancel() is a no-op) — long enough for every
 * reasonable read-after-done pattern — and is then reclaimed.
 * Handles are not keepsakes: keep long-term state in done(), not
 * in the handle. When the LAST widget root in the process is
 * destroyed the engine shuts down and every handle becomes dead
 * memory — do not touch animation handles after tearing down your
 * final tree (this is what keeps a clean app exit leak-free).
 *
 * REPETITION is application policy: re-arm in done(). The engine
 * does not know about loops, ping-pong, or delays — a settings
 * store, not a timeline editor.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: whole-window fade or slide
 * for popups and dialogs. X11 override-redirect popups and
 * Wayland xdg_popups have no opacity control in FDK's protocol
 * surface — animating the WINDOW would move protocol geometry,
 * which compositors repaint at their own pace (judder). Content
 * inside a window moves; the window itself appears at once. That
 * is the same trade GTK makes for menu popups.
 */

#ifndef FDK_ANIMATION_H
#define FDK_ANIMATION_H

#include "fdk_types.h"
#include "fdk_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- easing ------------------------------------------------------------ */

typedef enum {
    FDK_EASE_LINEAR = 0,
    FDK_EASE_SMOOTHSTEP,     /* hermite 3t^2-2t^3 — the default */
    FDK_EASE_QUAD_IN,
    FDK_EASE_QUAD_OUT,
    FDK_EASE_QUAD_IN_OUT,
    FDK_EASE_CUBIC_IN,
    FDK_EASE_CUBIC_OUT,
    FDK_EASE_CUBIC_IN_OUT,
    FDK_EASE_SINE_IN_OUT,
    FDK_EASE_EXPO_OUT,
    FDK_EASE_BACK_OUT,       /* overshoots past 1, settles at 1 */
} fdk_ease_kind;

/* Applies curve `kind` to t, both clamped to [0,1]. Returns a
 * value in [0,1] for every monotone curve; back_out exceeds 1
 * mid-flight by design. Unknown kinds are LINEAR — a math
 * function never fails, it degenerates. */
double fdk_ease_apply(fdk_ease_kind kind, double t);

/* ---- the animator ------------------------------------------------------- */

typedef struct fdk_animation fdk_animation;

/* Per frame. `e` is the eased progress; for back_out it may
 * slightly exceed 1 before settling. Called at most once per
 * display frame while the animation runs, always on the UI thread,
 * never after done(). May cancel THIS animation (legal; it simply
 * stops after the callback returns) — but must not destroy the
 * animating widget (the pump holds it for the invalidation right
 * after; schedule destroys outside the tick). */
typedef void (*fdk_animation_tick_fn)(fdk_animation *anim, double e,
                                      void *user_data);

/* Exactly once, when the animation stops: finished = true on
 * natural completion, false on fdk_animation_cancel(). NOT called
 * when the widget is destroyed underneath the animation. May start
 * a new animation (restart patterns) — it will first tick on the
 * NEXT frame, never re-entrantly inside this call. */
typedef void (*fdk_animation_done_fn)(fdk_animation *anim,
                                      bool finished, void *user_data);

/* Starts a single-shot animation on `widget`: `duration_ms` from
 * now (0 completes on the first frame), easing `ease`, per-frame
 * `on_tick` (required — an animation that observes nothing is a
 * timer, and FDK has timers), optional `on_done`, `user_data`
 * passed to both. Returns the animation handle, or NULL when
 * widget/on_tick is NULL or the allocation failed (an animation
 * failing to start is a missing polish, not an app failure — NULL
 * is checked, not propagated).
 *
 * The widget is invalidated every frame AFTER the tick returns;
 * the damage tracker does the rest. Standalone trees (no window)
 * accept animations — they tick whenever the test pump drives
 * them, which is exactly the headless-test seam. */
fdk_animation *fdk_widget_animate(fdk_widget *widget,
                                  fdk_u32 duration_ms,
                                  fdk_ease_kind ease,
                                  fdk_animation_tick_fn on_tick,
                                  fdk_animation_done_fn on_done,
                                  void *user_data);

/* Stops a running animation: fires on_done(finished = false) if
 * one was given, then releases the handle. NULL / already-stopped
 * animations are a quiet no-op. */
void fdk_animation_cancel(fdk_animation *anim);

/* The widget this animation is attached to (NULL after the widget
 * was destroyed underneath it). */
fdk_widget *fdk_animation_widget(const fdk_animation *anim);

/* True while the animation is running (false for NULL, finished,
 * or widget-detached handles). A handle past its animation is a
 * query object, not a dangling pointer. */
bool fdk_animation_running(const fdk_animation *anim);

#ifdef __cplusplus
}
#endif

#endif /* FDK_ANIMATION_H */
