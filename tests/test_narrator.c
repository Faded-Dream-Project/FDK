/* test_narrator.c — the embedded screen reader core (1.1.0),
 * headless.
 *
 * The no-bus policy's proof of work: FDK narrates focus moves,
 * toggles, and value changes through an application-wired sink,
 * with no registry, no bus, no daemon — verified entirely without
 * a display, because the narrator is just another subscriber of
 * the public a11y notifications.
 *
 * Groups:
 *   compose      — the utterance composer (names, roles, states,
 *                  values, truncation, invalid args)
 *   announce     — the forced status path
 *   engine       — start/stop lifecycle, focus/toggle/value
 *                  narration, the deliberate silences (focus-out,
 *                  unfocused churn, typing)
 *   localization — the i18n catalog wiring
 *   limits       — subscriber-slot exhaustion and recovery
 */

#include "fdk/fdk.h"
#include "fdk/fdk_a11y.h"
#include "fdk/fdk_i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__,        \
                    __LINE__);                                          \
            g_fail++;                                                   \
        } else {                                                        \
            printf("[ok] %s\n", msg);                                   \
        }                                                               \
    } while (0)

/* ---- the sink: records every utterance ------------------------------- */

#define UTTER_MAX 64
typedef struct {
    char utterances[UTTER_MAX][256];
    size_t count;
} speech;

static void record_speech(const char *utterance, void *user) {
    speech *s = user;
    if (s->count < UTTER_MAX) {
        snprintf(s->utterances[s->count], sizeof(s->utterances[0]), "%s",
                 utterance);
    }
    s->count++;
}

static bool speech_has(const speech *s, const char *utterance) {
    for (size_t i = 0; i < s->count && i < UTTER_MAX; i++) {
        if (strcmp(s->utterances[i], utterance) == 0) {
            return true;
        }
    }
    return false;
}

static void speech_reset(speech *s) {
    s->count = 0;
}

/* ---- shared tree ------------------------------------------------------ */

static fdk_font *g_font = NULL;

static void load_font(void) {
    const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]);
         i++) {
        g_font = fdk_font_load(candidates[i], 14);
        if (g_font != NULL) {
            return;
        }
    }
    g_font = NULL;
}

static void make_tree(fdk_widget **root_out, fdk_widget **go_out,
                      fdk_widget **two_out, fdk_widget **cb_out) {
    fdk_widget *root = NULL;
    fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300}, &root);
    fdk_widget *go = NULL;
    fdk_button_create(root, g_font, "Go", &go);
    fdk_widget *two = NULL;
    fdk_button_create(root, g_font, "Two", &two);
    fdk_widget *cb = NULL;
    fdk_checkbox_create(root, g_font, "Accept", &cb);
    *root_out = root;
    *go_out = go;
    *two_out = two;
    *cb_out = cb;
}

/* ---- compose ----------------------------------------------------------- */

static void test_compose_basics(void) {
    fdk_widget *root, *go, *two, *cb;
    make_tree(&root, &go, &two, &cb);

    char buf[256];
    size_t n = fdk_a11y_compose_announcement(go, buf, sizeof(buf));
    CHECK(n == strlen("Go, button") && strcmp(buf, "Go, button") == 0,
          "compose: named button");

    n = fdk_a11y_compose_announcement(cb, buf, sizeof(buf));
    CHECK(n == strlen("Accept, check box") &&
              strcmp(buf, "Accept, check box") == 0,
          "compose: unchecked checkbox has no state word");

    fdk_widget_set_enabled(go, false);
    n = fdk_a11y_compose_announcement(go, buf, sizeof(buf));
    CHECK(strcmp(buf, "Go, button, disabled") == 0,
          "compose: disabled word appended");
    fdk_widget_set_enabled(go, true);

    /* Unnamed widget: role alone ("separator" never has a name). */
    fdk_widget *sep = NULL;
    fdk_separator_create(root, FDK_HORIZONTAL, &sep);
    n = fdk_a11y_compose_announcement(sep, buf, sizeof(buf));
    CHECK(n == strlen("separator") && strcmp(buf, "separator") == 0,
          "compose: unnamed widget speaks the role alone");

    /* Override beats the class name. */
    fdk_widget_set_accessible_name(two, "Cancel");
    n = fdk_a11y_compose_announcement(two, buf, sizeof(buf));
    CHECK(strcmp(buf, "Cancel, button") == 0,
          "compose: accessible-name override");
    fdk_widget_set_accessible_name(two, NULL);

    fdk_widget_destroy(root);
}

static void test_compose_states_values(void) {
    fdk_widget *root = NULL;
    fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300}, &root);

    /* Checked checkbox. */
    fdk_widget *cb = NULL;
    fdk_checkbox_create(root, g_font, "Accept", &cb);
    fdk_checkbox_set_checked(cb, true);
    char buf[256];
    fdk_a11y_compose_announcement(cb, buf, sizeof(buf));
    CHECK(strcmp(buf, "Accept, check box, checked") == 0,
          "compose: checked word");

    /* Read-only entry: the text is the entry's VALUE (entries expose
     * their text through the value interface), so it lands last. */
    fdk_widget *entry = NULL;
    fdk_entry_create(root, g_font, "Username", &entry);
    fdk_entry_set_read_only(entry, true);
    fdk_a11y_compose_announcement(entry, buf, sizeof(buf));
    CHECK(strcmp(buf, "entry, read only, Username") == 0,
          "compose: read-only word");

    /* Slider: value text with the %g rendering. */
    fdk_widget *slider = NULL;
    fdk_slider_create(root, 0.0, 100.0, 64.0, &slider);
    fdk_widget_set_accessible_name(slider, "Volume");
    fdk_a11y_compose_announcement(slider, buf, sizeof(buf));
    CHECK(strcmp(buf, "Volume, slider, 64") == 0,
          "compose: slider value text");

    /* Progress: percent rendering, name via override (no text
     * constructor). */
    fdk_widget *prog = NULL;
    fdk_progress_create(root, &prog);
    fdk_progress_set_fraction(prog, 0.45f);
    fdk_widget_set_accessible_name(prog, "Copying");
    fdk_a11y_compose_announcement(prog, buf, sizeof(buf));
    CHECK(strcmp(buf, "Copying, progress bar, 45%") == 0,
          "compose: progress value text");

    fdk_widget_destroy(root);
}

static void test_compose_truncation(void) {
    fdk_widget *root, *go, *two, *cb;
    make_tree(&root, &go, &two, &cb);

    /* A tiny cap truncates but stays terminated; snprintf semantics
     * report the FULL needed length so callers can size-and-retry. */
    char tiny[5];
    size_t n = fdk_a11y_compose_announcement(go, tiny, sizeof(tiny));
    CHECK(n == strlen("Go, button") && strlen(tiny) == sizeof(tiny) - 1 &&
              tiny[sizeof(tiny) - 1] == '\0',
          "compose: truncation reports the full length, stays terminated");

    char exact[32];
    size_t n2 = fdk_a11y_compose_announcement(go, exact, n + 1);
    CHECK(n2 == strlen("Go, button") && strcmp(exact, "Go, button") == 0,
          "compose: retry with ret+1 fits exactly");

    /* Invalid arguments. */
    char buf[8] = "dirty";
    CHECK(fdk_a11y_compose_announcement(NULL, buf, sizeof(buf)) == 0 &&
              buf[0] == '\0',
          "compose: NULL widget -> 0 and empty buf");
    buf[0] = 'd';
    CHECK(fdk_a11y_compose_announcement(go, buf, 0) == 0,
          "compose: zero cap -> 0");
    CHECK(fdk_a11y_compose_announcement(go, NULL, sizeof(buf)) == 0,
          "compose: NULL buf -> 0");

    /* NOTE: a fully destroyed pointer is ordinary C UB by FDK's
     * documented contract (docs/memory.md) — the dying-widget
     * refusal inside describe() protects callbacks that run DURING
     * teardown, not use-after-free. No test here passes freed
     * pointers on purpose. */

    fdk_widget_destroy(root);
}

/* ---- announce ----------------------------------------------------------- */

static void test_announce(void) {
    /* No sink attached: forced announcements are no-ops, not
     * crashes. */
    fdk_a11y_announce("File saved");
    fdk_a11y_announce(NULL);

    speech s;
    memset(&s, 0, sizeof(s));
    fdk_a11y_set_speaker(record_speech, &s);

    fdk_a11y_announce("File saved");
    CHECK(s.count == 1 && speech_has(&s, "File saved"),
          "announce: forced message reaches the sink");

    fdk_a11y_announce(NULL);
    fdk_a11y_announce("");
    CHECK(s.count == 1, "announce: NULL and empty are no-ops");

    /* Detach; announce goes nowhere (still no crash). */
    fdk_a11y_set_speaker(NULL, NULL);
    fdk_a11y_announce("File saved");
    CHECK(!fdk_a11y_narrator_active(),
          "announce: detaching the sink parks the engine");
}

/* ---- the engine ---------------------------------------------------------- */

static void test_engine_lifecycle(void) {
    /* Starting without a sink is the documented argument error. */
    fdk_result r = fdk_a11y_narrator_start();
    CHECK(r == FDK_ERR_INVALID_ARGUMENT, "engine: start needs a sink");

    speech s;
    memset(&s, 0, sizeof(s));
    fdk_a11y_set_speaker(record_speech, &s);
    r = fdk_a11y_narrator_start();
    CHECK(fdk_ok(r) && fdk_a11y_narrator_active(),
          "engine: start with a sink");

    /* Idempotent. */
    r = fdk_a11y_narrator_start();
    CHECK(fdk_ok(r) && fdk_a11y_narrator_active(), "engine: restart is a no-op");

    fdk_widget *root, *go, *two, *cb;
    make_tree(&root, &go, &two, &cb);

    /* Focus moves narrate the GAINING side only. */
    fdk_widget_focus(go);
    CHECK(s.count == 1 && speech_has(&s, "Go, button"),
          "engine: focus move narrated");
    speech_reset(&s);

    fdk_widget_focus(two);
    CHECK(s.count == 1 && speech_has(&s, "Two, button"),
          "engine: focus-out is silent, focus-in speaks");
    speech_reset(&s);

    /* Refocusing the same widget: no event, no utterance. */
    fdk_widget_focus(two);
    CHECK(s.count == 0, "engine: no event when focus does not move");
    speech_reset(&s);

    /* Toggle on the FOCUSED widget re-announces at the new value. */
    fdk_widget_focus(cb);
    CHECK(speech_has(&s, "Accept, check box"), "engine: checkbox focused");
    speech_reset(&s);
    fdk_checkbox_set_checked(cb, true);
    CHECK(s.count == 1 && speech_has(&s, "Accept, check box, checked"),
          "engine: toggle narrated at the new value");
    speech_reset(&s);
    fdk_checkbox_set_checked(cb, false);
    CHECK(s.count == 1 && speech_has(&s, "Accept, check box"),
          "engine: untoggle drops the state word");
    speech_reset(&s);

    /* Toggle on an UNFOCUSED widget: background churn is silent. */
    fdk_widget *far_cb = NULL;
    fdk_checkbox_create(root, g_font, "Far", &far_cb);
    fdk_checkbox_set_checked(far_cb, true);
    CHECK(s.count == 0, "engine: unfocused toggle is silent");
    speech_reset(&s);

    /* Stop: narration stops, forced announce still reaches the sink. */
    fdk_a11y_narrator_stop();
    CHECK(!fdk_a11y_narrator_active(), "engine: stop");
    fdk_widget_focus(go);
    CHECK(s.count == 0, "engine: no narration after stop");
    fdk_a11y_announce("Paused");
    CHECK(speech_has(&s, "Paused"),
          "engine: forced announce survives stop");

    /* Restart resumes narration. */
    r = fdk_a11y_narrator_start();
    CHECK(fdk_ok(r), "engine: restart after stop");
    fdk_widget_focus(two);
    CHECK(speech_has(&s, "Two, button"), "engine: narration resumed");

    fdk_a11y_set_speaker(NULL, NULL);
    CHECK(!fdk_a11y_narrator_active(),
          "engine: detaching the sink parks the engine");
    fdk_widget_destroy(root);
}

static void test_engine_values(void) {
    speech s;
    memset(&s, 0, sizeof(s));
    fdk_a11y_set_speaker(record_speech, &s);
    (void)fdk_a11y_narrator_start();

    fdk_widget *root = NULL;
    fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300}, &root);

    /* Focused slider value change: compact utterance. */
    fdk_widget *slider = NULL;
    fdk_slider_create(root, 0.0, 100.0, 10.0, &slider);
    fdk_widget_set_accessible_name(slider, "Volume");
    fdk_widget_focus(slider);
    speech_reset(&s);
    fdk_slider_set_value(slider, 64.0);
    CHECK(s.count == 1 && speech_has(&s, "Volume, 64"),
          "engine: focused slider change is compact");

    /* Unfocused value churn (a progress bar filling in the
     * background) is silent. */
    fdk_widget *prog = NULL;
    fdk_progress_create(root, &prog);
    fdk_progress_set_fraction(prog, 0.5f);
    CHECK(s.count == 1, "engine: unfocused value change is silent");

    /* Typing is not narrated per keystroke: an editable's value
     * changes reach the subscriber but not the sink. */
    fdk_widget *entry = NULL;
    fdk_entry_create(root, g_font, "", &entry);
    fdk_widget_focus(entry);
    speech_reset(&s);
    (void)fdk_entry_set_text(entry, "hello");
    CHECK(s.count == 0, "engine: typing is not narrated");

    /* Spin buttons are editable-but-valued: they narrate compactly
     * (the user drove the change with keys on the focused widget). */
    fdk_widget *spin = NULL;
    fdk_spin_create(root, g_font, 0.0, 100.0, 1.0, &spin);
    fdk_widget_focus(spin);
    speech_reset(&s);
    fdk_spin_set_value(spin, 7.0);
    CHECK(s.count == 1, "engine: focused spin change narrated");

    fdk_a11y_set_speaker(NULL, NULL);
    fdk_widget_destroy(root);
}

/* ---- localization -------------------------------------------------------- */

/* FDK's catalog format has no gettext-style header entry (an empty
 * msgid is rejected) — comments and msgid/msgstr pairs only. */
static const char *const NARRATOR_CATALOG =
    "# narrator glue words\n"
    "msgid \"button\"\n"
    "msgstr \"knapp\"\n"
    "msgid \"checked\"\n"
    "msgstr \"avkrysset\"\n";

static void test_localization(void) {
    fdk_widget *root, *go, *two, *cb;
    make_tree(&root, &go, &two, &cb);

    fdk_catalog *cat = NULL;
    fdk_result r = fdk_catalog_parse(NARRATOR_CATALOG,
                                      strlen(NARRATOR_CATALOG), &cat);
    CHECK(fdk_ok(r), "l10n: catalog parsed");
    if (!fdk_ok(r)) {
        fdk_widget_destroy(root);
        return;
    }

    fdk_a11y_narrator_set_catalog(cat);

    char buf[256];
    fdk_a11y_compose_announcement(go, buf, sizeof(buf));
    CHECK(strcmp(buf, "Go, knapp") == 0, "l10n: role word localized");

    fdk_checkbox_set_checked(cb, true);
    fdk_a11y_compose_announcement(cb, buf, sizeof(buf));
    CHECK(strcmp(buf, "Accept, check box, avkrysset") == 0,
          "l10n: state word localized, untranslated words pass through");

    /* Unknown msgids pass through unchanged (the gettext contract
     * fdk_translate already guarantees; the composer relies on it). */
    fdk_widget *sep = NULL;
    fdk_separator_create(root, FDK_HORIZONTAL, &sep);
    fdk_a11y_compose_announcement(sep, buf, sizeof(buf));
    CHECK(strcmp(buf, "separator") == 0, "l10n: untranslated msgid passes");

    /* Catalog unset: back to English. */
    fdk_a11y_narrator_set_catalog(NULL);
    fdk_a11y_compose_announcement(go, buf, sizeof(buf));
    CHECK(strcmp(buf, "Go, button") == 0, "l10n: NULL catalog is English");

    fdk_catalog_destroy(cat);
    fdk_widget_destroy(root);
}

/* ---- limits --------------------------------------------------------------- */

static void limit_sink(const char *utterance, void *user) {
    (void)utterance;
    (void)user;
}

static void limit_on_event(const fdk_a11y_event *ev, void *user) {
    (void)ev;
    (void)user;
}

static void test_slot_limit(void) {
    fdk_a11y_set_speaker(limit_sink, NULL);

    /* Occupy all 16 subscriber slots. */
    for (int i = 0; i < FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        fdk_result r = fdk_a11y_subscribe(NULL, limit_on_event,
                                          (void *)(intptr_t)(i + 1));
        if (!fdk_ok(r)) {
            CHECK(false, "limits: filler subscribe failed");
            fdk_a11y_set_speaker(NULL, NULL);
            return;
        }
    }

    fdk_result r = fdk_a11y_narrator_start();
    CHECK(r == FDK_ERR_LIMIT, "limits: no free slot -> FDK_ERR_LIMIT");
    CHECK(!fdk_a11y_narrator_active(), "limits: engine not started");

    /* Free one slot: start succeeds now. */
    (void)fdk_a11y_unsubscribe(NULL, limit_on_event, (void *)(intptr_t)1);
    r = fdk_a11y_narrator_start();
    CHECK(fdk_ok(r) && fdk_a11y_narrator_active(),
          "limits: start succeeds after a slot frees");

    /* Cleanup. */
    fdk_a11y_set_speaker(NULL, NULL); /* parks the engine */
    for (int i = 2; i <= FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        (void)fdk_a11y_unsubscribe(NULL, limit_on_event,
                                   (void *)(intptr_t)i);
    }
    CHECK(true, "limits: cleanup done");
}

/* ---- main ------------------------------------------------------------------ */

int main(void) {
    load_font();
    test_compose_basics();
    test_compose_states_values();
    test_compose_truncation();
    test_announce();
    test_engine_lifecycle();
    test_engine_values();
    test_localization();
    test_slot_limit();
    if (g_font != NULL) {
        fdk_font_destroy(g_font);
    }
    if (g_fail != 0) {
        fprintf(stderr, "%d FAILURES\n", g_fail);
        return 1;
    }
    printf("all narrator tests passed\n");
    return 0;
}
