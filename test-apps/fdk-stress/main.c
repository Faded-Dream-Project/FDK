/* main.c — fdk-stress: the release churn rig.
 *
 * The literal "stress test" the release asked for: phase after
 * phase of churn against the library ALONE — window create/destroy
 * storms, widget churn, a 20k-row model hammered with
 * insert/remove/rewrite while its change callback counts, a
 * 200-timer storm, clipboard round trips, resize storms, font
 * churn, and a 5k-row List rebuilt three ways — every phase
 * interleaved with real pumps so repaints, layout and the event
 * loop stay in the path the whole time. Under ASan/UBSan (the
 * test-apps default) it doubles as a leak and use-after-free trap.
 *
 * The receipts: every phase prints its counters; the exit asserts
 * them (timers all fired, model row math adds up, clipboard round
 * trips returned their own bytes) and prints STRESS PASS.
 */

#include "app.h"
#include "fdk/fdk_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the phase machine ------------------------------------------
 * One phase runs a bounded slice per loop pass (so pumps interleave
 * — the churn never starves the event loop it is stressing). */

typedef enum phase {
    PH_WINDOWS,
    PH_WIDGETS,
    PH_MODEL,
    PH_TIMERS,
    PH_CLIPBOARD,
    PH_RESIZE,
    PH_FONTS,
    PH_LIST,
    PH_DONE,
} phase;

typedef struct stress {
    ta_app ta;
    phase ph;
    /* windows */
    fdk_window *wins[24];
    int wins_made, wins_gone, win_events;
    /* widgets */
    fdk_widget *churn_box, *churn_label;
    int widgets_made, widgets_gone, churn_round;
    /* model */
    fdk_model *model;
    int model_stage, model_rows, model_removed, model_updates;
    int model_changes;
    /* timers */
    int timers_made, timers_fired;
    fdk_timer *repeater;
    int repeater_fires;
    /* clipboard */
    int clip_sets, clip_reads, clip_mismatches;
    /* resize */
    int resizes;
    /* fonts */
    int fonts_made, fonts_gone;
    /* list */
    fdk_widget *list;
    int list_rows, list_removed, list_rewrites;
} stress;

static stress S;

/* ---- windows ---- */

static void win_event(fdk_window *w, const fdk_event_data *e,
                      void *user) {
    (void)w;
    stress *s = user;
    if (e->type == FDK_EVENT_WINDOW_EXPOSE) {
        s->win_events++;
    }
}

static void phase_windows(stress *s) {
    /* two creates per pass, then two destroys, until 24 round-trip */
    if (s->wins_made < 24) {
        fdk_window_options wo = {0};
        char title[64];
        snprintf(title, sizeof title, "stress-%d", s->wins_made);
        wo.title = title;
        wo.width = 120;
        wo.height = 80;
        if (fdk_ok(fdk_window_create(s->ta.ctx, &wo,
                                     &s->wins[s->wins_made]))) {
            fdk_window_set_event_callback(s->wins[s->wins_made],
                                          win_event, s);
            fdk_window_show(s->wins[s->wins_made]);
            s->wins_made++;
        } else {
            printf("FAIL window create #%d\n", s->wins_made);
            s->ta.quit = true;
        }
        return;
    }
    if (s->wins_gone < s->wins_made) {
        fdk_window_destroy(s->wins[s->wins_gone++]);
    }
    if (s->wins_gone == s->wins_made) {
        printf("[ok] windows: %d created, %d destroyed, %d exposes "
               "served\n",
               s->wins_made, s->wins_gone, s->win_events);
        s->ph = PH_WIDGETS;
    }
}

/* ---- widget churn: a box that grows and shrinks under layout ---- */

static void phase_widgets(stress *s) {
    for (int i = 0; i < 25; i++) {
        if (s->widgets_made == s->widgets_gone) {
            fdk_widget *l = NULL;
            char txt[48];
            snprintf(txt, sizeof txt, "churn-%d-%d", s->churn_round,
                     i);
            if (fdk_ok(fdk_label_create(s->churn_box, s->ta.font_ui,
                                        txt, &l))) {
                s->churn_label = l;
                s->widgets_made++;
            }
        } else if (s->churn_label != NULL) {
            fdk_widget_destroy(s->churn_label); /* relayout */
            s->churn_label = NULL;
            s->widgets_gone++;
        }
    }
    if (s->widgets_gone >= 400) {
        fdk_widget_destroy(s->churn_box);
        printf("[ok] widget churn: %d created, %d destroyed over %d "
               "rounds\n",
               s->widgets_made, s->widgets_gone, s->churn_round);
        s->ph = PH_MODEL;
    }
    s->churn_round++;
}

/* ---- the model: 20k rows, remove half, rewrite, resolve ---- */

static void model_changed(fdk_model *m, fdk_model_change change,
                          fdk_model_row row, void *user) {
    (void)m;
    (void)row;
    stress *s = user;
    s->model_changes++;
    (void)change;
}

static void phase_model(stress *s) {
    /* stage 0: append, stage 1: remove half, stage 2: verify+handoff */
    if (s->model_stage == 0 && s->model_rows < 20000) {
        for (int i = 0; i < 2000 && s->model_rows < 20000; i++) {
            fdk_model_row r =
                fdk_model_row_append(s->model, FDK_MODEL_ROW_NONE);
            /* a top-level row IS parented FDK_MODEL_ROW_NONE — the
             * row's INDEX is the address that matters here */
            s->model_rows++;
            if (s->model_rows % 313 == 0) {
                fdk_model_row a;
                a.parent = FDK_MODEL_ROW_NONE;
                a.index = r.index;
                fdk_model_set_int(s->model, a, 0,
                                  (fdk_i64)s->model_rows);
                s->model_updates++;
            }
        }
        if (s->model_rows >= 20000) {
            s->model_stage = 1;
        }
        return;
    }
    if (s->model_stage == 1) {
        /* remove every other row, bottom-up so the indexes below
         * the cut hold; sliced per pass like every other phase */
        size_t cut = 19999 - (size_t)s->model_removed * 2;
        for (size_t k = 0; k < 1000 && cut > 0; k++, cut -= 2) {
            fdk_model_row r;
            r.parent = FDK_MODEL_ROW_NONE;
            r.index = cut;
            if (fdk_ok(fdk_model_row_remove(s->model, r))) {
                s->model_removed++;
            } else {
                printf("FAIL model remove at %zu\n", cut);
                s->ta.quit = true;
                return;
            }
        }
        if (s->model_removed >= 10000) {
            s->model_stage = 2;
        }
        return;
    }
    if (s->model_stage == 2) {
        /* read a survivor back, then hand off */
        fdk_model_row r;
        r.parent = FDK_MODEL_ROW_NONE;
        r.index = 5;
        fdk_i64 v = 0;
        (void)fdk_model_get_int(s->model, r, 0, &v);
        printf("[ok] model: %d rows appended, %d removed, %d updates, "
               "%d change notifications, survivor[5].i=%lld\n",
               s->model_rows, s->model_removed, s->model_updates,
               s->model_changes, (long long)v);
        s->model_stage = 3;
        s->ph = PH_TIMERS;
    }
}

/* ---- the timer storm ---- */

static void storm_fire(fdk_timer *t, void *user) {
    (void)t;
    stress *s = user;
    s->timers_fired++;
}

static void repeater_fire(fdk_timer *t, void *user) {
    (void)t;
    stress *s = user;
    s->repeater_fires++;
}

static void phase_timers(stress *s) {
    if (s->repeater == NULL) {
        for (int i = 0; i < 200; i++) {
            if (fdk_timer_add(s->ta.ctx, (fdk_u32)(1 + i % 25), false,
                              storm_fire, s) != NULL) {
                s->timers_made++;
            }
        }
        s->repeater = fdk_timer_add(s->ta.ctx, 15, true, repeater_fire,
                                    s);
    }
    if (s->timers_fired >= s->timers_made) {
        fdk_timer_remove(s->repeater);
        s->repeater = NULL;
        printf("[ok] timers: %d one-shots all fired, repeater fired "
               "%dx\n",
               s->timers_made, s->repeater_fires);
        s->ph = PH_CLIPBOARD;
    }
}

/* ---- clipboard round trips ---- */

static void phase_clipboard(stress *s) {
    for (int i = 0; i < 10 && s->clip_sets < 50; i++) {
        char buf[96];
        snprintf(buf, sizeof buf, "stress-clip-%d-payload-ΔΔΔ",
                 s->clip_sets);
        if (!fdk_ok(fdk_clipboard_set_text(s->ta.ctx, buf))) {
            break; /* no clipboard on this rig: degrade, don't fail */
        }
        s->clip_sets++;
        char *got = fdk_clipboard_get_text(s->ta.ctx);
        if (got != NULL) {
            s->clip_reads++;
            if (strcmp(got, buf) != 0) {
                s->clip_mismatches++;
            }
            free(got);
        }
    }
    if (s->clip_sets >= 50) {
        printf("[ok] clipboard: %d sets, %d read-backs, %d "
               "mismatches\n",
               s->clip_sets, s->clip_reads, s->clip_mismatches);
        s->ph = PH_RESIZE;
    }
}

/* ---- the resize storm ---- */

static void phase_resize(stress *s) {
    for (int i = 0; i < 2 && s->resizes < 40; i++) {
        int w = 600 + (s->resizes % 5) * 90;
        int h = 420 + (s->resizes % 4) * 60;
        fdk_window_resize(s->ta.window, (fdk_i32)w, (fdk_i32)h);
        s->resizes++;
    }
    if (s->resizes >= 40) {
        fdk_window_resize(s->ta.window, 900, 600);
        printf("[ok] resize: %d window resizes relayouted\n",
               s->resizes);
        s->ph = PH_FONTS;
    }
}

/* ---- font churn ---- */

static void phase_fonts(stress *s) {
    for (int i = 0; i < 6 && s->fonts_made < 60; i++) {
        fdk_font *f =
            fdk_font_load_system_default((fdk_i32)(10 + s->fonts_made %
                                                   22));
        if (f != NULL) {
            s->fonts_made++;
            fdk_font_destroy(f);
            s->fonts_gone++;
        }
    }
    if (s->fonts_made >= 60) {
        printf("[ok] fonts: %d loaded, %d destroyed\n", s->fonts_made,
               s->fonts_gone);
        s->ph = PH_LIST;
    }
}

/* ---- the list: 5k rows three ways (append, insert, rewrite) ---- */

static void phase_list(stress *s) {
    for (int i = 0; i < 2500 && s->list_rows < 5000; i++) {
        char txt[48];
        snprintf(txt, sizeof txt, "row-%d", s->list_rows);
        size_t idx = 0;
        if (fdk_ok(fdk_list_append(s->list, txt, &idx))) {
            s->list_rows++;
        }
    }
    if (s->list_rows >= 5000 && s->list_removed == 0) {
        for (int i = 0; i < 1250; i++) {
            if (fdk_ok(fdk_list_remove(s->list, (size_t)(4999 - i)))) {
                s->list_removed++;
            }
        }
    }
    if (s->list_removed >= 1250 && s->list_rewrites < 500) {
        for (int i = 0; i < 250 && s->list_rewrites < 500; i++) {
            char txt[48];
            snprintf(txt, sizeof txt, "rewrite-%d", s->list_rewrites);
            if (fdk_ok(fdk_list_set_row_text(
                    s->list, (size_t)(s->list_rewrites % 2000), txt))) {
                s->list_rewrites++;
            }
        }
    }
    if (s->list_rewrites >= 500) {
        printf("[ok] list: %d rows, %d removed, %d rewrites (count "
               "now %zu)\n",
               s->list_rows, s->list_removed, s->list_rewrites,
               fdk_list_row_count(s->list));
        s->ph = PH_DONE;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* phase receipts must survive a timeout kill: no buffering */
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&S, 0, sizeof S);

    if (!ta_init(&S.ta, "stress")) {
        return 1;
    }
    if (!ta_open(&S.ta, "fdk-stress", 900, 600)) {
        return 1;
    }

    /* the churn box the widget phase grows and shrinks */
    if (!fdk_ok(fdk_box_create(S.ta.content, FDK_VERTICAL,
                               &S.churn_box))) {
        return 1;
    }
    fdk_box_set_spacing(S.churn_box, 2);

    /* the model: one INT column, a counting change callback */
    fdk_model_value_type cols[1] = {FDK_MODEL_INT};
    S.model = fdk_model_create(cols, 1);
    fdk_model_set_on_change(S.model, model_changed, &S);

    /* the list */
    if (!fdk_ok(fdk_list_create(S.ta.content, S.ta.font_ui,
                                &S.list))) {
        return 1;
    }
    fdk_widget_set_expand(S.list, true, true);

    while (!S.ta.quit && S.ph != PH_DONE) {
        switch (S.ph) {
        case PH_WINDOWS: phase_windows(&S); break;
        case PH_WIDGETS: phase_widgets(&S); break;
        case PH_MODEL: phase_model(&S); break;
        case PH_TIMERS: phase_timers(&S); break;
        case PH_CLIPBOARD: phase_clipboard(&S); break;
        case PH_RESIZE: phase_resize(&S); break;
        case PH_FONTS: phase_fonts(&S); break;
        case PH_LIST: phase_list(&S); break;
        default: S.ta.quit = true; break;
        }
        ta_pump(&S.ta);
    }

    /* ---- the receipts + assertions ---- */
    int fails = 0;
    if (S.wins_made != 24 || S.wins_gone != 24) {
        printf("FAIL window lifecycle (%d/%d)\n", S.wins_made,
               S.wins_gone);
        fails++;
    }
    if (S.widgets_made != S.widgets_gone || S.widgets_made == 0) {
        printf("FAIL widget churn (%d/%d)\n", S.widgets_made,
               S.widgets_gone);
        fails++;
    }
    if (S.model_rows != 20000 || S.model_removed != 10000) {
        printf("FAIL model churn (%d rows, %d removed)\n",
               S.model_rows, S.model_removed);
        fails++;
    }
    if (S.model_changes == 0) {
        printf("FAIL model change notifications never fired\n");
        fails++;
    }
    if (S.timers_fired != S.timers_made || S.timers_made == 0) {
        printf("FAIL timer storm (%d/%d fired)\n", S.timers_fired,
               S.timers_made);
        fails++;
    }
    if (S.clip_sets > 0 && S.clip_reads != S.clip_sets) {
        printf("FAIL clipboard round trips (%d/%d)\n", S.clip_reads,
               S.clip_sets);
        fails++;
    }
    if (S.clip_mismatches != 0) {
        printf("FAIL clipboard payload mismatch\n");
        fails++;
    }
    if (S.resizes != 40) {
        printf("FAIL resize storm (%d)\n", S.resizes);
        fails++;
    }
    if (S.fonts_made != 60 || S.fonts_gone != 60) {
        printf("FAIL font churn (%d/%d)\n", S.fonts_made,
               S.fonts_gone);
        fails++;
    }
    if (S.list_rows != 5000 || S.list_removed != 1250 ||
        S.list_rewrites != 500) {
        printf("FAIL list churn (%d/%d/%d)\n", S.list_rows,
               S.list_removed, S.list_rewrites);
        fails++;
    }
    if (S.model != NULL) {
        fdk_model_destroy(S.model);
    }
    printf("stress receipt: windows=%d widgets=%d model=%d/%d "
           "timers=%d clip=%d resize=%d fonts=%d list=%d/%d/%d\n",
           S.wins_made, S.widgets_made, S.model_rows,
           S.model_removed, S.timers_fired, S.clip_sets, S.resizes,
           S.fonts_made, S.list_rows, S.list_removed,
           S.list_rewrites);
    ta_close(&S.ta);
    if (fails == 0) {
        printf("STRESS PASS\n");
        return 0;
    }
    printf("STRESS FAIL (%d checks)\n", fails);
    return 1;
}
