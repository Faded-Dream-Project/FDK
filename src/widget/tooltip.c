#define FDK_LOG_TAG "widgets"

/*
 * tooltip.c — the tooltip layer (1.3.2)
 *
 * One tooltip at a time, process-wide (the same single-instance shape
 * as the a11y subscriber registry): the pointer rests on a widget
 * with tooltip text for ~500ms -> a toolkit-owned, input-TRANSPARENT
 * popup appears near the widget's lower edge; any pointer press, key
 * press, or hover change dismisses it (and a new hover re-arms the
 * delay). The application never learns the popup exists — it pumps
 * events; FDK does the rest (the menu machinery's discipline).
 *
 * Placement: below-right of the widget's absolute bounds, offset
 * (12, 6); when that would overflow the window's right edge the box
 * shifts left to fit, and when it would overflow the bottom the box
 * flips ABOVE the widget (the classic tooltip dance, integer math
 * only).
 *
 * The box: rounded (theme tooltip_corner_radius), tooltip_background
 * fill, tooltip_border edge, tooltip_text label wrapped at 360px,
 * 8px/6px padding. A canvas widget paints it into the popup's root.
 *
 * The popup is INPUT-TRANSPARENT (fdk__window_create_popup_ex): an
 * empty input region and NO popup grab, decided at create time on
 * both backends. The grab half is not cosmetic — XGrabPointer on a
 * tooltip would make the server emit a LeaveNotify against the
 * owner window (the real pointer sits inside it during a hover),
 * which the event layer translates into the hover-out that dismisses
 * the tooltip the same instant it maps; a keyboard grab would
 * additionally steal the app's keys while the hint is up.
 *
 * Lifetime safety: the shown text is the MODULE's copy (the target
 * may be destroyed while the box is up — its string would dangle
 * under the paint hook), and the target pointers are only ever
 * COMPARED (never dereferenced outside the show decision, which
 * re-checks the DESTROYING flag). The popup window is parented to
 * the owner window and dies with it via the popup-family sweep; the
 * module's state clears on the next hover change or shutdown.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* Timing/geometry knobs. */
#define TIP_DELAY_MS 500  /* hover rest before showing (GTK-ish)    */
#define TIP_MAX_W 360     /* wrap width                              */
#define TIP_PAD_X 8
#define TIP_PAD_Y 6
#define TIP_OFFSET_X 12   /* from the widget's left edge            */
#define TIP_OFFSET_Y 6    /* below the widget's bottom edge          */

/* ---- module state (single instance) ---- */

typedef struct fdk_tooltip_state {
    /* Pending (armed, not yet shown). pending_target is only ever
     * compared by address (never dereferenced outside the timer's
     * own callback, which re-validates via the user pointer it was
     * HANDED — the same pointer, still valid because destroy would
     * have fired the hover transition that cancels the timer). */
    fdk_widget *pending_target;
    fdk_timer *pending_timer;
    /* Shown. */
    fdk_window *win;              /* the popup (owned)                */
    fdk_widget *shown_target;     /* compared by address only         */
    char *text;                   /* owned copy the popup paints      */
    fdk_font *font;               /* owned; loaded per show           */
} fdk_tooltip_state;

static fdk_tooltip_state g_tip;

/* ---- helpers ---- */

/* Cancels the pending delay (if any). Safe unconditionally. */
static void tip_cancel_pending(void) {
    if (g_tip.pending_timer != NULL) {
        fdk_timer_remove(g_tip.pending_timer);
        g_tip.pending_timer = NULL;
    }
    g_tip.pending_target = NULL;
}

/* Destroys the shown popup (if any). Safe unconditionally. */
static void tip_dismiss(void) {
    if (g_tip.win != NULL) {
        fdk_window *win = g_tip.win;
        g_tip.win = NULL;
        fdk_window_destroy(win);
    }
    if (g_tip.font != NULL) {
        fdk_font_destroy(g_tip.font);
        g_tip.font = NULL;
    }
    fdk_free(g_tip.text);
    g_tip.text = NULL;
    g_tip.shown_target = NULL;
}

/* Wraps `text` at the tip width, writing the line array (caller
 * frees with fdk_free) and the widest line's advance. Returns false
 * on failure (nothing to show). */
static bool tip_wrap(fdk_font *font, const char *text,
                     fdk_text_line **out_lines, size_t *out_n,
                     fdk_i32 *out_widest) {
    *out_lines = NULL;
    *out_n = 0;
    *out_widest = 0;
    size_t n = 0;
    if (!fdk_ok(fdk_font_break_lines_utf8(font, text, strlen(text),
                                          TIP_MAX_W - TIP_PAD_X * 2, NULL,
                                          0, &n, NULL)) ||
        n == 0) {
        return false;
    }
    fdk_text_line *lines = fdk_alloc_array(n, sizeof(*lines));
    if (lines == NULL) {
        return false;
    }
    if (!fdk_ok(fdk_font_break_lines_utf8(font, text, strlen(text),
                                          TIP_MAX_W - TIP_PAD_X * 2,
                                          lines, n, &n, NULL))) {
        fdk_free(lines);
        return false;
    }
    fdk_i32 widest = 0;
    for (size_t i = 0; i < n; i++) {
        if (lines[i].advance_width > widest) {
            widest = lines[i].advance_width;
        }
    }
    *out_lines = lines;
    *out_n = n;
    *out_widest = widest;
    return true;
}

/* The canvas paint callback: draws the rounded box + wrapped text.
 * Runs inside the paint walk — draw only (the canvas contract), and
 * idempotently (the re-wrap is the same deterministic pass). */
static void tip_paint(fdk_widget *canvas, fdk_surface *surface,
                      fdk_rect bounds, fdk_rect clip, void *user) {
    (void)canvas;
    (void)clip;
    (void)user; /* the module's own text copy, not a target pointer */
    const char *text = g_tip.text;
    fdk_font *font = g_tip.font;
    if (text == NULL || font == NULL ||
        bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_color fill = fdk_theme_get_color(NULL, FDK_TK_TOOLTIP_BACKGROUND);
    fdk_color border = fdk_theme_get_color(NULL, FDK_TK_TOOLTIP_BORDER);
    fdk_color fg = fdk_theme_get_color(NULL, FDK_TK_TOOLTIP_TEXT);
    fdk_i32 radius =
        fdk_theme_get_metric(NULL, FDK_TM_TOOLTIP_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, radius, fill);
    fdk_surface_draw_rounded_rect(surface, bounds, radius, border);

    fdk_text_line *lines = NULL;
    size_t n = 0;
    fdk_i32 widest = 0;
    if (!tip_wrap(font, text, &lines, &n, &widest)) {
        return;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk_i32 pitch = fm.ascent + fm.descent;
    fdk_i32 y = bounds.y + TIP_PAD_Y + fm.ascent;
    for (size_t i = 0; i < n; i++) {
        (void)fdk_surface_draw_utf8(surface, font,
                                    text + lines[i].byte_offset,
                                    lines[i].byte_len,
                                    bounds.x + TIP_PAD_X, y, fg);
        y += pitch;
    }
    fdk_free(lines);
}

/* The delay timer's callback: the pointer rested — show the box. */
static void tip_delay_elapsed(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_widget *target = user;
    g_tip.pending_timer = NULL;
    g_tip.pending_target = NULL;

    /* The widget layer cancels the pending timer on every hover
     * transition and destroy clears the hovered flag — a target that
     * reaches this callback WITHOUT cancellation but mid-destruction
     * is still guarded here (the flags are the truth). */
    if (target == NULL || (target->flags & FDK_WF_DESTROYING) != 0 ||
        !fdk_widget_is_effectively_visible(target) ||
        target->tooltip == NULL) {
        return;
    }

    tip_dismiss(); /* one at a time */

    fdk_window *owner =
        fdk__window_of_owner(fdk__widget_window_owner(target));
    if (owner == NULL) {
        return; /* detached tree: never show (documented) */
    }

    fdk_font *font = fdk_font_load_system_default(12);
    if (font == NULL) {
        return; /* no font: no tooltip (honest) */
    }

    fdk_text_line *lines = NULL;
    size_t n = 0;
    fdk_i32 widest = 0;
    if (!tip_wrap(font, target->tooltip, &lines, &n, &widest)) {
        fdk_font_destroy(font);
        return;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk_i32 pitch = fm.ascent + fm.descent;
    fdk_i32 w = widest + TIP_PAD_X * 2;
    fdk_i32 h = (fdk_i32)(n * (size_t)pitch) + TIP_PAD_Y * 2;
    fdk_free(lines);
    if (w < 24) {
        w = 24; /* a one-word tip still reads as a box */
    }

    /* Placement: below-right of the widget, clamped to the owner
     * window's bounds, flipping above when the bottom overflows. */
    fdk_rect abs = fdk_widget_get_absolute_bounds(target);
    fdk_size owner_size = {0, 0};
    (void)fdk_window_get_size(owner, &owner_size);
    fdk_i32 x = abs.x + TIP_OFFSET_X;
    fdk_i32 y = abs.y + abs.height + TIP_OFFSET_Y;
    if (x + w > (fdk_i32)owner_size.width) {
        x = (fdk_i32)owner_size.width - w; /* shift left to fit */
    }
    if (x < 0) {
        x = 0;
    }
    if (y + h > (fdk_i32)owner_size.height) {
        y = abs.y - h - TIP_OFFSET_Y; /* flip above the widget */
        if (y < 0) {
            y = (fdk_i32)owner_size.height - h; /* last resort */
            if (y < 0) {
                y = 0;
            }
        }
    }

    fdk_window *pop = NULL;
    fdk_result r = fdk__window_create_popup_ex(owner->ctx, owner, x, y,
                                               w, h, true, &pop);
    if (!fdk_ok(r)) {
        fdk_font_destroy(font);
        return;
    }
    fdk__window_set_auto_paint(pop, true);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(pop, &root);
    if (fdk_ok(r)) {
        fdk_widget *cv = NULL;
        r = fdk_canvas_create(root, tip_paint, NULL, &cv);
        if (fdk_ok(r)) {
            fdk_widget_set_bounds(cv, (fdk_rect){0, 0, w, h});
        }
    }
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        fdk_font_destroy(font);
        return;
    }

    /* Module-owned state from here on: the text COPY (the target may
     * die while shown — its string must not dangle under tip_paint)
     * and the font. */
    g_tip.text = fdk__strdup(target->tooltip);
    if (g_tip.text == NULL) {
        fdk_window_destroy(pop);
        fdk_font_destroy(font);
        return;
    }
    g_tip.font = font;
    g_tip.win = pop;
    g_tip.shown_target = target;
    /* Map LAST, the menu machinery's discipline: everything the
     * first frame needs (canvas, bounds, module state) is in place
     * before the window becomes viewable, so the first present is
     * the complete box — no flash of empty popup. */
    fdk_window_show(pop);
    FDK_DEBUG("tooltip: shown (%dx%d at %d,%d)", w, h, x, y);
}

/* ---- widget-core hooks (called from widget.c) ---- */

/* Every hover transition lands here (set_hovered + the window-level
 * LEAVE path). old/new may be NULL and may be DESTROYING (the
 * transition runs after event deliveries, whose callbacks may have
 * destroyed things — comparisons are by address, dereferences are
 * flag-guarded). */
void fdk__tooltip_hover_changed(fdk_widget *root, fdk_widget *old_hit,
                                fdk_widget *new_hit) {
    (void)root;
    if (old_hit != NULL) {
        if (g_tip.pending_target == old_hit) {
            tip_cancel_pending();
        }
        if (g_tip.shown_target == old_hit) {
            tip_dismiss();
        }
    }
    if (new_hit != NULL && new_hit->tooltip != NULL &&
        (new_hit->flags & FDK_WF_DESTROYING) == 0 &&
        fdk_widget_is_effectively_enabled(new_hit)) {
        tip_cancel_pending(); /* a fresh hover restarts the clock */
        tip_dismiss();        /* and replaces any shown tip */
        fdk_window *owner =
            fdk__window_of_owner(fdk__widget_window_owner(new_hit));
        if (owner == NULL) {
            return; /* detached: store-only contract */
        }
        g_tip.pending_target = new_hit;
        g_tip.pending_timer = fdk_timer_add(
            owner->ctx, TIP_DELAY_MS, false, tip_delay_elapsed, new_hit);
        if (g_tip.pending_timer == NULL) {
            g_tip.pending_target = NULL; /* OOM: no tooltip this time */
        }
    }
}

/* Any pointer press / key press. */
void fdk__tooltip_hide(void) {
    tip_cancel_pending();
    tip_dismiss();
}

/* Shutdown hook (called from fdk_shutdown AFTER the window sweep —
 * the popup would already be dead with its owner; this clears the
 * rest of the module state). */
void fdk__tooltip_shutdown(void) {
    tip_cancel_pending();
    tip_dismiss();
}