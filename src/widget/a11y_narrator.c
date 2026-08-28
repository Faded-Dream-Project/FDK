/* a11y_narrator.c — the embedded screen reader core (1.1.0).
 *
 * THE NO-BUS POLICY, IMPLEMENTED. GTK and Qt reach screen readers
 * through AT-SPI2 over D-Bus: a registry daemon, a session bus, a
 * bridge process — three moving parts the toolkit does not own and
 * the application cannot audit. FDK's answer (docs/dependencies.md,
 * "The no-bus policy") is to own the job in-process: the narrator is
 * an ordinary SUBSCRIBER of the public a11y notifications that
 * composes utterances from live fdk_a11y_describe() snapshots and
 * SPEAKS them through a sink the application wires — a TTS engine,
 * a braille device, a subtitle bar, a log line. FDK itself links,
 * loads, and requires none of it.
 *
 * What the engine narrates (while started, sink attached):
 *   - focus moves       -> the newly focused widget, fully described
 *   - toggle changes    -> CHECKED/PRESSED/SELECTED/EXPANDED on the
 *                          focused widget, re-announced at the new
 *                          value ("Accept, check box, checked")
 *   - value changes     -> sliders, progress, scroll fractions, spins
 *                          on the focused widget, compact ("Volume,
 *                          64%"). Editable text is deliberately NOT
 *                          narrated per keystroke: typing is not a
 *                          screen-reader event, and the flood would
 *                          drown everything else.
 *   - forced messages   -> fdk_a11y_announce("File saved") reaches
 *                          the sink regardless of the engine state.
 *
 * Layer position: widget-layer, backend-neutral, headless-verifiable
 * (same discipline as the rest of the a11y core — see test_narrator.c).
 *
 * Reentrancy: the sink runs inside whatever call stack mutated the
 * tree (focus() et al.), with the narrator's composition ALREADY
 * DONE — the utterance is heap-built before the sink is touched, so
 * the sink's contract is the same one FDK gives every event
 * callback: it may query the tree, but must not destroy widgets.
 * Utterances are valid only for the duration of the call.
 */

#include "fdk/fdk_a11y.h"
#include "fdk/fdk_i18n.h"

#include "core/alloc_internal.h"
#include "widgets_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- state ----------------------------------------------------------- */

static fdk_a11y_speak_fn g_speak;
static void *g_speak_user;
static bool g_engine;               /* automatic narration on/off    */
static const fdk_catalog *g_cat;    /* optional localization source  */

/* Localizes one glue word when a catalog is wired. English (the
 * msgid, gettext convention) when not — the composer's word set is
 * documented in the header so translators know the domain. */
static const char *tr(const char *s) {
    if (g_cat != NULL && s != NULL) {
        const char *t = fdk_catalog_get(g_cat, s);
        if (t != NULL) {
            return t;
        }
    }
    return s;
}

/* ---- the composer ----------------------------------------------------- */

/* Growable string with the toolkit's allocator and the same
 * overflow discipline as the parsers: on allocation failure the
 * composer degrades to truncation (never a crash, never a dangling
 * pointer), and callers see the truncated length. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} sbuf;

static void sbuf_init(sbuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

static void sbuf_grow(sbuf *b, size_t need) {
    if (b->oom || b->cap >= need) {
        return;
    }
    size_t cap = (b->cap != 0) ? b->cap : 64;
    while (cap < need) {
        size_t next = cap * 2;
        if (next < cap) { /* overflow: clamp, mark, stop */
            b->oom = true;
            return;
        }
        cap = next;
    }
    char *p = fdk_realloc(b->data, cap);
    if (p == NULL) {
        b->oom = true;
        return;
    }
    b->data = p;
    b->cap = cap;
}

/* Appends a string; a failed append marks oom and stops copying. */
static void sbuf_append(sbuf *b, const char *s) {
    if (s == NULL || b->oom) {
        return;
    }
    size_t n = strlen(s);
    sbuf_grow(b, b->len + n + 1);
    if (b->oom) {
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sbuf_append_sep(sbuf *b) {
    if (b->len != 0 && !b->oom) {
        sbuf_append(b, ", ");
    }
}

static void sbuf_destroy(sbuf *b) {
    fdk_free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* The word set a screen reader speaks for states. Only states a
 * user can act on are named; visibility bookkeeping (VISIBLE/
 * SHOWING/FOCUSABLE/ENABLED) is silent unless disabled — silence
 * is the normal case worth breaking only for the exception. */
static void compose_states(sbuf *b, fdk_a11y_state_set st) {
    if ((st & FDK_A11Y_CHECKED) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("checked"));
    }
    if ((st & FDK_A11Y_PRESSED) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("pressed"));
    }
    if ((st & FDK_A11Y_SELECTED) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("selected"));
    }
    if ((st & FDK_A11Y_EXPANDED) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("expanded"));
    }
    if ((st & FDK_A11Y_READ_ONLY) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("read only"));
    }
    if ((st & FDK_A11Y_REQUIRED) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("required"));
    }
    if ((st & FDK_A11Y_INVALID) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("invalid"));
    }
    if ((st & FDK_A11Y_MODAL) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("modal"));
    }
    if ((st & FDK_A11Y_BUSY) != 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("busy"));
    }
    if ((st & FDK_A11Y_ENABLED) == 0) {
        sbuf_append_sep(b);
        sbuf_append(b, tr("disabled"));
    }
}

/* Builds the full announcement for `w` into `b`: "Save, button",
 * "Accept, check box, checked", "Volume, slider, 64%". The shared
 * path of the public composer and the engine's focus/toggle
 * narration, so the two can never drift apart. */
static void compose_full(const fdk_widget *w, sbuf *b) {
    fdk_a11y_info info;
    if (!fdk_ok(fdk_a11y_describe(w, &info))) {
        fdk_a11y_info_free(&info);
        return;
    }
    if (info.name != NULL) {
        sbuf_append(b, info.name);
    }
    sbuf_append_sep(b);
    sbuf_append(b, tr(fdk_a11y_role_name(info.role)));
    compose_states(b, info.states);
    if (info.has_value && info.value_text != NULL) {
        sbuf_append_sep(b);
        sbuf_append(b, info.value_text);
    }
    fdk_a11y_info_free(&info);
}

size_t fdk_a11y_compose_announcement(const fdk_widget *widget, char *buf,
                                     size_t cap) {
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0'; /* every failure path leaves an empty string */
    if (widget == NULL) {
        return 0;
    }

    sbuf b;
    sbuf_init(&b);
    compose_full(widget, &b);

    /* snprintf semantics: the return is what the FULL announcement
     * would need, so a caller can size-and-retry; the copy into buf
     * truncates (always NUL-terminated). */
    size_t need = b.len;
    if (b.data != NULL) {
        size_t copy = (need >= cap) ? cap - 1 : need;
        memcpy(buf, b.data, copy);
        buf[copy] = '\0';
    }
    sbuf_destroy(&b);
    return need;
}

/* ---- the sink --------------------------------------------------------- */

void fdk_a11y_set_speaker(fdk_a11y_speak_fn fn, void *user_data) {
    if (fn == NULL) {
        /* Detaching the sink parks the engine too: a narrator with
         * nowhere to speak holds no subscriber slot. */
        fdk_a11y_narrator_stop();
        g_speak = NULL;
        g_speak_user = NULL;
        return;
    }
    g_speak = fn;
    g_speak_user = user_data;
}

void fdk_a11y_narrator_set_catalog(const fdk_catalog *catalog) {
    g_cat = catalog;
}

/* ---- speaking --------------------------------------------------------- */

/* Composes the full announcement for `w` and hands it to the sink.
 * The utterance is built BEFORE the sink runs, and freed after —
 * the sink never sees a pointer into borrowed widget state. */
static void speak_widget(const fdk_widget *w) {
    if (g_speak == NULL) {
        return;
    }
    sbuf b;
    sbuf_init(&b);
    compose_full(w, &b);
    if (b.data != NULL && !b.oom) {
        g_speak(b.data, g_speak_user);
    }
    sbuf_destroy(&b);
}

/* Compact value utterance for change narration: "Volume, 64%".
 * Falls back to the full announcement when there is no rendered
 * value text (progress without text, scroll fractions). */
static void speak_value_change(const fdk_widget *w) {
    if (g_speak == NULL) {
        return;
    }
    fdk_a11y_info info;
    if (!fdk_ok(fdk_a11y_describe(w, &info))) {
        fdk_a11y_info_free(&info);
        return;
    }
    if (info.value_text == NULL) {
        fdk_a11y_info_free(&info);
        speak_widget(w); /* no rendering: full announce, not silence */
        return;
    }
    sbuf b;
    sbuf_init(&b);
    if (info.name != NULL) {
        sbuf_append(&b, info.name);
        sbuf_append(&b, ", ");
    }
    sbuf_append(&b, info.value_text);
    fdk_a11y_info_free(&info);
    if (b.data != NULL && !b.oom) {
        g_speak(b.data, g_speak_user);
    }
    sbuf_destroy(&b);
}

void fdk_a11y_announce(const char *text) {
    if (g_speak == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    g_speak(text, g_speak_user);
}

/* ---- the engine ------------------------------------------------------- */

/* Reads the CURRENT focused state of `w` from a live snapshot —
 * the event contract says the new value is readable at receipt
 * time, so this is the post-change truth, never a stale one. */
static bool is_focused_now(const fdk_widget *w) {
    fdk_a11y_info info;
    if (!fdk_ok(fdk_a11y_describe(w, &info))) {
        fdk_a11y_info_free(&info);
        return false;
    }
    bool focused = (info.states & FDK_A11Y_FOCUSED) != 0;
    fdk_a11y_info_free(&info);
    return focused;
}

static void narrator_on_event(const fdk_a11y_event *ev, void *user) {
    (void)user;
    if (g_speak == NULL) {
        return; /* parked: engine active but sinkless (cannot happen
                 * through the public API — stop() runs on detach —
                 * but a NULL check here costs nothing and keeps the
                 * callback honest if that invariant ever changes) */
    }

    if (ev->kind == FDK_A11Y_STATE_CHANGED) {
        if (ev->state_flag == FDK_A11Y_FOCUSED) {
            /* Speak only the gaining side; focus-out is silence
             * before the next widget's utterance. */
            if (is_focused_now(ev->widget)) {
                speak_widget(ev->widget);
            }
            return;
        }
        switch (ev->state_flag) {
        case FDK_A11Y_CHECKED:
        case FDK_A11Y_PRESSED:
        case FDK_A11Y_SELECTED:
        case FDK_A11Y_EXPANDED:
            /* Toggle re-announcement, focused widget only — a check
             * box toggled by keyboard is focused; background state
             * churn (list reselects, radio group cleanups) would be
             * noise. Compose AFTER the flip: the event means the
             * new value is live. */
            if (is_focused_now(ev->widget)) {
                speak_widget(ev->widget);
            }
            return;
        default:
            return;
        }
    }

    if (ev->kind == FDK_A11Y_VALUE_CHANGED) {
        fdk_a11y_info info;
        if (!fdk_ok(fdk_a11y_describe(ev->widget, &info))) {
            fdk_a11y_info_free(&info);
            return;
        }
        bool focused = (info.states & FDK_A11Y_FOCUSED) != 0;
        bool editable = (info.states & FDK_A11Y_EDITABLE) != 0;
        bool has_value = info.has_value;
        fdk_a11y_info_free(&info);
        if (focused && has_value && !editable) {
            speak_value_change(ev->widget);
        }
        return;
    }
}

fdk_result fdk_a11y_narrator_start(void) {
    if (g_speak == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* nowhere to speak yet */
    }
    if (g_engine) {
        return FDK_OK; /* idempotent */
    }
    fdk_result r = fdk_a11y_subscribe(NULL, narrator_on_event, NULL);
    if (!fdk_ok(r)) {
        return r; /* FDK_ERR_LIMIT: the 16 slots are taken */
    }
    g_engine = true;
    return FDK_OK;
}

void fdk_a11y_narrator_stop(void) {
    if (!g_engine) {
        return;
    }
    (void)fdk_a11y_unsubscribe(NULL, narrator_on_event, NULL);
    g_engine = false;
}

bool fdk_a11y_narrator_active(void) {
    return g_engine;
}
