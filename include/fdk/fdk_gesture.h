/*
 * fdk_gesture.h — gesture recognizers (the §23/B2b round,
 * 2026-11-01): the GtkGesture / QGestureRecognizer-shaped layer
 * over FDK's pointer + touch events.
 *
 * FDK's touch ENGINE (src/window/touch.c) already synthesizes taps,
 * two-finger scroll, PINCH and SWIPE events from raw touch slots.
 * What was missing is the RECOGNIZER side application code attaches
 * to a widget or window:
 *
 *   click      single / double / triple (the 400 ms / 5 px
 *              double-click convention; n_press counts up)
 *   long-press press-and-hold past 500 ms without leaving the slop
 *              (delay_factor scales the timeout), cancelled on
 *              movement or release
 *   drag       begin / update / end with the total offset from the
 *              press point (the move-a-widget gesture)
 *   pan        drag with an orientation latch (horizontal /
 *              vertical first, GTK's GtkGesturePan)
 *   swipe      a fast fling on release — velocity in px/s and the
 *              dominant direction
 *   zoom       the two-finger span ratio, re-baselined
 *   rotate     the two-finger angle, re-baselined (the pinch
 *              companion FDK's raw PINCH event lacked)
 *
 * The model: one opaque recognizer per gesture, fed the events a
 * widget/window already receives (POINTER_*, TOUCH_*), firing its
 * callbacks when recognized. Attach to a WIDGET for scoping (the
 * widget's bounds gate the press; FDK routes the events it
 * receives) or feed it manually from any event callback for custom
 * surfaces. Multiple recognizers on one widget compose the GTK
 * way: the first to CLAIM a sequence (fdk_gesture_claim) makes the
 * later ones see it as DENIED — a drag beats a click once motion
 * passes the threshold, exactly like GTK's sequence states.
 *
 * Time: timeouts are measured on the recognizer's clock, which is
 * the caller's to drive — the default reads the system monotonic
 * clock; tests install a controllable one (fdk_gesture_set_clock)
 * so no test ever sleeps.
 *
 * Thresholds (GTK4's defaults, verified against its docs this
 * round): double-click window 400 ms and 5 px; long-press 500 ms;
 * drag threshold 5 px; swipe: velocity > 300 px/s on the dominant
 * axis. Sources:
 *   https://docs.gtk.org/gtk4/property.Settings.gtk-long-press-time.html
 *   https://docs.gtk.org/gtk4/class.Gesture.html
 */

#ifndef FDK_GESTURE_H
#define FDK_GESTURE_H

#include "fdk_error.h"
#include "fdk_event.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Recognizer kinds --------------------------------------------------- */

typedef enum fdk_gesture_kind {
    FDK_GESTURE_CLICK = 1,
    FDK_GESTURE_LONG_PRESS = 2,
    FDK_GESTURE_DRAG = 3,
    FDK_GESTURE_PAN = 4,
    FDK_GESTURE_SWIPE = 5,
    FDK_GESTURE_ZOOM = 6,
    FDK_GESTURE_ROTATE = 7,
} fdk_gesture_kind;

typedef struct fdk_gesture fdk_gesture;

/* ---- Callback payloads --------------------------------------------------- */

/* The pan/swipe directions. */
typedef enum fdk_gesture_direction {
    FDK_GESTURE_DIR_LEFT = 1,
    FDK_GESTURE_DIR_RIGHT = 2,
    FDK_GESTURE_DIR_UP = 3,
    FDK_GESTURE_DIR_DOWN = 4,
} fdk_gesture_direction;

/* click: n_press = 1/2/3 (the multi-click count). */
typedef void (*fdk_gesture_click_fn)(fdk_gesture *g, int n_press,
                                     fdk_pointf where, void *user);

/* long-press: pressed after the hold; cancelled when it degrades
 * (moved past the slop, released early, or a second finger). */
typedef void (*fdk_gesture_long_press_fn)(fdk_gesture *g,
                                          fdk_pointf where,
                                          bool pressed_not_cancelled,
                                          void *user);

/* drag: begin at the threshold crossing, update per motion, end
 * (with the FINAL total offset) at release. */
typedef void (*fdk_gesture_drag_fn)(fdk_gesture *g,
                                    fdk_pointf offset_from_press,
                                    fdk_pointf where, void *user);

/* swipe: velocity in logical px/s (dominant axis signed), the
 * direction, on release. */
typedef void (*fdk_gesture_swipe_fn)(fdk_gesture *g,
                                     fdk_f32 velocity_x,
                                     fdk_f32 velocity_y,
                                     fdk_gesture_direction dir,
                                     void *user);

/* zoom/rotate: the cumulative ratio / radians since the posture
 * began (1.0 / 0.0 at start), and the incremental delta since the
 * previous callback. */
typedef void (*fdk_gesture_scale_fn)(fdk_gesture *g, fdk_f32 value,
                                     fdk_f32 delta, void *user);

/* ---- Lifecycle ------------------------------------------------------------ */

fdk_gesture *fdk_gesture_create(fdk_gesture_kind kind);
void fdk_gesture_destroy(fdk_gesture *g);

/* Configuration (before or after feeding; affects the next
 * evaluation):
 *   fdk_gesture_set_threshold_px  the drag/pan activation slop and
 *                                 the click/long-press travel bound
 *                                 (default 5 px)
 *   fdk_gesture_set_multi_click_ms the double/triple-click window
 *                                 (default 400 ms)
 *   fdk_gesture_set_long_press_ms the hold (default 500 ms;
 *                                 delay_factor multiplies)
 *   fdk_gesture_set_delay_factor  GTK's delay-factor on the hold
 *                                 (default 1.0)
 *   fdk_gesture_set_orientation   PAN only: latch the axis (both
 *                                 by default) */
void fdk_gesture_set_threshold_px(fdk_gesture *g, fdk_f32 px);
void fdk_gesture_set_multi_click_ms(fdk_gesture *g, long long ms);
void fdk_gesture_set_long_press_ms(fdk_gesture *g, long long ms);
void fdk_gesture_set_delay_factor(fdk_gesture *g, fdk_f32 factor);
void fdk_gesture_set_orientation(fdk_gesture *g,
                                 fdk_gesture_direction axis);

/* Callbacks + user data (one per recognizer; setting NULL clears). */
void fdk_gesture_on_click(fdk_gesture *g, fdk_gesture_click_fn fn,
                          void *user);
void fdk_gesture_on_long_press(fdk_gesture *g,
                               fdk_gesture_long_press_fn fn,
                               void *user);
void fdk_gesture_on_drag(fdk_gesture *g, fdk_gesture_drag_fn fn,
                         void *user);
void fdk_gesture_on_swipe(fdk_gesture *g, fdk_gesture_swipe_fn fn,
                          void *user);
void fdk_gesture_on_scale(fdk_gesture *g, fdk_gesture_scale_fn fn,
                          void *user);

/* ---- Feeding ---------------------------------------------------------------
 * Hand the recognizer every POINTER_* / TOUCH_* event the widget or
 * window receives (widget callbacks: fdk_widget_set_event_callback's
 * stream; windows: fdk_window_set_event_callback). Feed returns
 * true when THIS event was consumed by a claim (see below).
 *
 * Sequences and claims: a press opens a sequence; the first
 * recognizer (in feed order, per process — attach drags before
 * clicks) whose rule fires calls its callback and CLAIMS the
 * sequence; further events of a claimed sequence still feed the
 * claimant (drag update/end) but other recognizers see them as
 * denied (a click never fires after its press started a drag).
 * fdk_gesture_claim forces the claim from application logic (the
 * GtkGesture "claim the sequence yourself" escape hatch). */
bool fdk_gesture_feed(fdk_gesture *g, const fdk_event_data *event);
void fdk_gesture_claim(fdk_gesture *g);

/* Resets the recognizer to idle (a widget's unmap or a grab loss —
 * GTK's cancel semantics: an interrupted gesture ends SILENTLY,
 * its callbacks do not fire). */
void fdk_gesture_reset(fdk_gesture *g);

/* The controllable clock (tests; the production default reads the
 * monotonic millisecond clock). NULL restores production. */
void fdk_gesture_set_clock(long long (*now_ms)(void *user), void *user);

#ifdef __cplusplus
}
#endif

#endif /* FDK_GESTURE_H */
