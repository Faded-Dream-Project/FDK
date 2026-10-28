/*
 * wayland_ime.c — text-input-v3, the Wayland IME client (1.1.0, M-1)
 *
 * Binds zwp_text_input_manager_v3 (optional global — absent means
 * the compositor has no text-input protocol; keyboard input still
 * works, there is just no IME composition) and drives ONE
 * zwp_text_input_v3 for the connection's seat through the v3 state
 * machine:
 *
 *   enter(surface)  -> enable + content-type + surrounding + commit
 *                      (the compositor activates the IME for the
 *                      focused surface; v3's enter is the
 *                      authoritative focus gate — wlroots only sends
 *                      it when an input method is actually there)
 *   leave()         -> disable + commit
 *   preedit_string /
 *   commit_string   -> LATCHED pending state, applied at `done`
 *                      (v3 semantics: the compositor batches a
 *                      state update and confirms it with done; acting
 *                      before done would show preedit the compositor
 *                      may still revise in the same batch)
 *   done(serial)    -> apply: a pending commit dispatches
 *                      FDK_EVENT_IME_COMMIT (and clears any showing
 *                      preedit FIRST — v3 replaces the preedit with
 *                      the commit); a pending preedit dispatches
 *                      FDK_EVENT_IME_PREEDIT; an empty batch
 *                      dispatches PREEDIT "" (the compositor reset
 *                      the composition).
 *
 * SURROUNDING TEXT: the window layer pushes the focused text
 * widget's (text, caret) through the ime_update_surrounding op
 * (window.c's post-routing hook — focus gains and IME event
 * routing, the honest cadence: per-keystroke sync is churn the
 * protocol allows but no consumer needs). set_text_change_cause is
 * sent with INPUT_METHOD cause for the IME-driven pushes.
 *
 * delete_surrounding_text: consumed and logged at DEBUG — the
 * autocorrect-style context edit is level-2 material (the roadmap's
 * M-1 list names enable/surrounding/commit/preedit/disable; most
 * CJK engines never send it, and folding it into COMMIT needs the
 * widget caret context the event model does not carry yet).
 *
 * The strings events hand us are owned by libwayland only until the
 * next event — every latched pending is a copy.
 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "generated/text-input-unstable-v3-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* ---- the connection's text-input state --------------------------------- */

/* Latched pending strings (copies; freed/replaced per batch). */
static void ime_pending_set(char **slot, const char *text) {
    fdk_free(*slot);
    *slot = NULL;
    if (text != NULL && text[0] != '\0') {
        size_t n = strlen(text) + 1;
        *slot = fdk_alloc(n);
        if (*slot != NULL) {
            memcpy(*slot, text, n);
        }
    }
}

static void ime_pending_clear(char **slot) {
    fdk_free(*slot);
    *slot = NULL;
}

/* The focused surface's pwindow (the enter event carries the
 * surface; every dispatch targets that window). */
static fdk_platform_window *ime_focus_window(fdk_platform_connection *conn,
                                             struct wl_surface *surface) {
    if (surface == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i]->surface == surface) {
            return conn->windows[i];
        }
    }
    return NULL;
}

static void ime_dispatch(fdk_platform_connection *conn,
                         fdk_platform_window *pwindow, fdk_event_type type,
                         const char *text, int cursor) {
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.ime.text = (text != NULL) ? text : "";
    event.ime.cursor = cursor;
    conn->dispatch(pwindow, &event, conn->dispatch_user_data);
}

/* ---- the zwp_text_input_v3 listener ------------------------------------- */

/* enter(): a NEW focus session begins. The v3 state machine only
 * emits its enable EVENT when enabled goes false->true at a commit
 * — enable-on-still-enabled is a protocol NO-OP (wlroots applies
 * nothing, sway never re-activates the IM). A compositor that
 * drops the focused surface WITHOUT sending leave (wlroots on
 * window destroy — the observed sway 1.10.1 shape) leaves the
 * input enabled; self-heal by closing the stale session first. */
static void ti_enter(void *data, struct zwp_text_input_v3 *ti,
                     struct wl_surface *surface) {
    (void)ti;
    fdk_platform_connection *conn = data;
    /* A batch latched for the dead session never applies (its
     * window is gone; the showing flag dies with it). */
    ime_pending_clear(&conn->ime_pending_preedit);
    ime_pending_clear(&conn->ime_pending_commit);
    conn->ime_preedit_showing = 0;
    conn->ime_focus = ime_focus_window(conn, surface);
    if (conn->ime_focus == NULL) {
        return; /* not one of ours (should not happen) */
    }
    if (conn->ime_enabled) {
        /* The leave never came: close the stale session so the
         * enable below is a real false->true transition. */
        zwp_text_input_v3_disable(ti);
        zwp_text_input_v3_commit(ti);
    }
    /* The v3 handshake: enable + the state we know + commit. The
     * surrounding text may be empty at this instant (the window
     * layer's focus push follows a moment later — set_surrounding
     * can repeat anytime while enabled). */
    zwp_text_input_v3_enable(ti);
    zwp_text_input_v3_set_content_type(
        ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
        ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    zwp_text_input_v3_set_surrounding_text(
        ti, conn->ime_surrounding ? conn->ime_surrounding : "",
        conn->ime_surrounding_cursor, conn->ime_surrounding_cursor);
    zwp_text_input_v3_set_text_change_cause(
        ti, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
    zwp_text_input_v3_commit(ti);
    conn->ime_enabled = 1;
    FDK_DEBUG("text-input: enabled for focused surface");
}

static void ti_leave(void *data, struct zwp_text_input_v3 *ti,
                     struct wl_surface *surface) {
    (void)ti;
    (void)surface; /* the surface we entered; tracked in ime_focus */
    fdk_platform_connection *conn = data;
    if (conn->ime_focus == NULL) {
        return;
    }
    /* Clear any showing composition FIRST (the compositor will not
     * send a done for it once we disable), then disable + commit. */
    ime_dispatch(conn, conn->ime_focus, FDK_EVENT_IME_PREEDIT, "", 0);
    conn->ime_focus = NULL;
    conn->ime_enabled = 0;
    zwp_text_input_v3_disable(ti);
    zwp_text_input_v3_commit(ti);
    /* The latched pendings of a half-finished batch are dead. */
    ime_pending_clear(&conn->ime_pending_preedit);
    ime_pending_clear(&conn->ime_pending_commit);
    fdk_free(conn->ime_surrounding);
    conn->ime_surrounding = NULL;
    conn->ime_surrounding_cursor = 0;
    FDK_DEBUG("text-input: disabled (focus left)");
}

static void ti_preedit_string(void *data, struct zwp_text_input_v3 *ti,
                              const char *text, int32_t cursor_begin,
                              int32_t cursor_end) {
    (void)ti;
    (void)cursor_end; /* preedit highlight span end — nothing renders
                       * it (the inline composition is plain + the
                       * single caret); kept for the level-2 story. */
    fdk_platform_connection *conn = data;
    ime_pending_set(&conn->ime_pending_preedit, text);
    conn->ime_pending_preedit_cursor = (int)cursor_begin;
}

static void ti_commit_string(void *data, struct zwp_text_input_v3 *ti,
                             const char *text) {
    (void)ti;
    fdk_platform_connection *conn = data;
    ime_pending_set(&conn->ime_pending_commit, text);
}

static void ti_delete_surrounding_text(void *data,
                                       struct zwp_text_input_v3 *ti,
                                       uint32_t before_length,
                                       uint32_t after_length) {
    (void)ti;
    fdk_platform_connection *conn = data;
    FDK_DEBUG("text-input: delete_surrounding_text(%u, %u) — "
              "level-2 material, consumed unapplied (M-1 scope)",
              before_length, after_length);
    (void)conn;
}

static void ti_done(void *data, struct zwp_text_input_v3 *ti,
                    uint32_t serial) {
    (void)ti;
    (void)serial; /* our own commit's serial — the state-apply gate,
                   * not something we echo back in v3. */
    fdk_platform_connection *conn = data;
    if (conn->ime_focus == NULL) {
        return;
    }
    /* Apply the batch: commit wins (it replaces the preedit); a
     * lone preedit shows; an empty batch clears whatever showed. */
    if (conn->ime_pending_commit != NULL) {
        if (conn->ime_preedit_showing) {
            ime_dispatch(conn, conn->ime_focus, FDK_EVENT_IME_PREEDIT,
                         "", 0);
            conn->ime_preedit_showing = 0;
        }
        ime_dispatch(conn, conn->ime_focus, FDK_EVENT_IME_COMMIT,
                     conn->ime_pending_commit, 0);
        ime_pending_clear(&conn->ime_pending_commit);
        ime_pending_clear(&conn->ime_pending_preedit);
    } else if (conn->ime_pending_preedit != NULL) {
        ime_dispatch(conn, conn->ime_focus, FDK_EVENT_IME_PREEDIT,
                     conn->ime_pending_preedit,
                     conn->ime_pending_preedit_cursor);
        conn->ime_preedit_showing = 1;
        ime_pending_clear(&conn->ime_pending_preedit);
    } else if (conn->ime_preedit_showing) {
        ime_dispatch(conn, conn->ime_focus, FDK_EVENT_IME_PREEDIT,
                     "", 0);
        conn->ime_preedit_showing = 0;
    }
}

/* NOTE: v3 has NO preedit_style / modifiers / keysym events (the
 * v2-era features) — nothing here stubs them. Rich preedit styling
 * and IME-forwarded keysyms are level-2 story, not parking spots. */

static const struct zwp_text_input_v3_listener g_text_input_listener = {
    .enter = ti_enter,
    .leave = ti_leave,
    .preedit_string = ti_preedit_string,
    .commit_string = ti_commit_string,
    .delete_surrounding_text = ti_delete_surrounding_text,
    .done = ti_done,
};

/* ---- lifecycle + the surrounding push ----------------------------------- */

void fdk_wayland_ime_seat_ready(fdk_platform_connection *conn) {
    if (conn == NULL || conn->text_input_manager == NULL ||
        conn->seat == NULL || conn->text_input != NULL) {
        return;
    }
    conn->text_input =
        zwp_text_input_manager_v3_get_text_input(conn->text_input_manager,
                                                 conn->seat);
    if (conn->text_input == NULL) {
        FDK_WARN("zwp_text_input_v3 creation failed — no IME "
                 "composition on Wayland");
        return;
    }
    zwp_text_input_v3_add_listener(conn->text_input,
                                   &g_text_input_listener, conn);
    FDK_INFO("text-input-v3: IME composition available");
}

void fdk_wayland_ime_teardown(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }
    if (conn->text_input != NULL) {
        zwp_text_input_v3_destroy(conn->text_input);
        conn->text_input = NULL;
    }
    if (conn->text_input_manager != NULL) {
        zwp_text_input_manager_v3_destroy(conn->text_input_manager);
        conn->text_input_manager = NULL;
    }
    ime_pending_clear(&conn->ime_pending_preedit);
    ime_pending_clear(&conn->ime_pending_commit);
    fdk_free(conn->ime_surrounding);
    conn->ime_surrounding = NULL;
    conn->ime_surrounding_cursor = 0;
    conn->ime_focus = NULL;
    conn->ime_preedit_showing = 0;
    conn->ime_enabled = 0;
}

/* A window died while holding IME focus: its latched batch must
 * never apply (the dispatch target is gone — a use-after-free
 * otherwise) and the enabled session is left STALE (wlroots drops
 * the focused surface without a leave); the next enter's self-heal
 * closes it. Called from the Wayland window destroy path. */
void fdk_wayland_ime_window_destroyed(fdk_platform_connection *conn,
                                      fdk_platform_window *pwindow) {
    if (conn == NULL || pwindow == NULL || conn->ime_focus != pwindow) {
        return;
    }
    conn->ime_focus = NULL;
    ime_pending_clear(&conn->ime_pending_preedit);
    ime_pending_clear(&conn->ime_pending_commit);
    conn->ime_preedit_showing = 0;
    fdk_free(conn->ime_surrounding);
    conn->ime_surrounding = NULL;
    conn->ime_surrounding_cursor = 0;
    /* ime_enabled stays 1 deliberately: ti_enter's self-heal needs
     * to see the stale session to close it. */
}

/* The ops-table op behind window.c's post-routing hook: the focused
 * text widget's context, pushed at focus gain and after IME routing.
 * NULL text clears the context (focus lost to a non-text widget). */
void fdk_wayland_ime_update_surrounding(fdk_platform_window *pwindow,
                                        const char *text, int cursor) {
    if (pwindow == NULL || pwindow->conn == NULL) {
        return;
    }
    fdk_platform_connection *conn = pwindow->conn;
    if (conn->text_input == NULL || conn->ime_focus != pwindow) {
        return; /* not enabled for this window */
    }
    ime_pending_set(&conn->ime_surrounding, text);
    conn->ime_surrounding_cursor = (text != NULL) ? cursor : 0;
    zwp_text_input_v3_set_surrounding_text(
        conn->text_input, text != NULL ? text : "",
        conn->ime_surrounding_cursor, conn->ime_surrounding_cursor);
    zwp_text_input_v3_set_text_change_cause(
        conn->text_input, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
    zwp_text_input_v3_commit(conn->text_input);
}
