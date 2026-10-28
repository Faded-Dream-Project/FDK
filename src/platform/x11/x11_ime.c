/*
 * x11_ime.c — XIM preedit callbacks + the composition buffer (1.1.0, M-1)
 *
 * The X11 side of the IME model. The per-window XIC is created with
 * XIMPreeditCallbacks | XIMStatusNothing (falling back to
 * XIMPreeditNothing when the IM refuses the style — the 1.3.1
 * posture): the IM hands COMPOSITION to the client through three
 * callbacks, and we render it inline (the Entry/TextView preedit
 * groundwork), which is exactly what CSD toolkits do.
 *
 * The composition buffer (the per-IC preedit string) is maintained
 * here from the draw callbacks: each draw replaces chg_length
 * CHARACTERS at chg_first with the callback's text (NULL = pure
 * deletion). The char-index ↔ byte-offset mapping is why
 * fdk__x11_preedit_apply is a pure, headless-testable function —
 * the UTF-8 index math is where every XIM client historically
 * corrupts its buffer.
 *
 * Committed text flows Xutf8LookupString (x11_events.c): a
 * single-codepoint commit stays the ordinary KEY_DOWN codepoint
 * (compatibility), a multi-codepoint commit (the CJK case an IME
 * exists for) dispatches FDK_EVENT_IME_COMMIT and the key event
 * itself is swallowed.
 *
 * Sandbox honesty: there is NO XIM server here (no fcitx5/ibus), so
 * the callback wiring is code-reviewed + owner-verified on the real
 * desktop (TESTING-ON-DESKTOP.md, fcitx5 recommended), while the
 * BUFFER MATH is pinned headless (tests/test_ime.c) and the commit
 * dispatch is pinned by the integration rig through the test seam.
 */

#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* ---- the composition buffer math (pure, headless-tested) --------------- */

/* Counts UTF-8 characters (not bytes) in a NUL-terminated string. */
static size_t preedit_char_count(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
        if ((*p & 0xC0u) != 0x80u) {
            n++; /* not a continuation byte: a character starts here */
        }
    }
    return n;
}

/* Byte offset of the idx-th CHARACTER (clamped to the byte length).
 * The idx-th character is the idx-th LEAD (non-continuation) byte —
 * the `seen == idx` test must sit INSIDE the lead-byte branch, after
 * the check that this byte IS one: testing it at every byte (the
 * first shape this function had, caught by test_ime's CJK leg on
 * its first run) returns the middle of the PREVIOUS character for
 * every idx > 0 under multi-byte text. */
static size_t preedit_char_to_byte(const char *s, size_t idx) {
    size_t seen = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0) {
        if ((*p & 0xC0u) != 0x80u) {
            if (seen == idx) {
                return (size_t)(p - (const unsigned char *)s);
            }
            seen++;
        }
        p++;
    }
    return (size_t)(p - (const unsigned char *)s);
}

/* Replace `chg_length` CHARACTERS at character index `chg_first` in
 * the NUL-terminated UTF-8 `buf` (capacity cap, always >= strlen+1)
 * with `insert` (a NUL-terminated UTF-8 string, NULL = delete only).
 * Returns the new byte length, or (size_t)-1 on a bad call
 * (chg_first beyond the string) — the buffer is left untouched then.
 *
 * XIM's chg_first/chg_length are CHARACTER units per the spec; a
 * chg_length of 0 with non-NULL text is a pure insert at chg_first;
 * text NULL with chg_length 0 is a no-op (caret-only move). */
size_t fdk__x11_preedit_apply(char *buf, size_t cap, long chg_first,
                              long chg_length, const char *insert) {
    if (buf == NULL || chg_first < 0 || chg_length < 0) {
        return (size_t)-1;
    }
    size_t len = strlen(buf);
    /* chg_first is a CHARACTER index: == the character count is the
     * legal append; BEYOND it is a protocol violation — refuse
     * rather than corrupt. (The byte-offset clamp alone cannot see
     * the difference: char-to-byte clamps both to strlen.) */
    if ((size_t)chg_first > preedit_char_count(buf)) {
        return (size_t)-1;
    }
    size_t first_byte = preedit_char_to_byte(buf, (size_t)chg_first);
    size_t end_byte = preedit_char_to_byte(buf + first_byte,
                                           (size_t)chg_length);
    size_t ins_len = (insert != NULL) ? strlen(insert) : 0;

    /* Clip the deletion to the string's end (chg_length may exceed
     * the remainder — the IM means "the rest"). */
    size_t removed = (first_byte + end_byte <= len) ? end_byte : (len - first_byte);
    size_t new_len = len - removed + ins_len;
    if (new_len + 1 > cap) {
        return (size_t)-1; /* would overflow: refuse (the caller's
                            * cap discipline keeps preedits bounded) */
    }
    memmove(buf + first_byte + ins_len, buf + first_byte + removed,
            len - first_byte - removed + 1 /* include the NUL */);
    if (ins_len > 0) {
        memcpy(buf + first_byte, insert, ins_len);
    }
    buf[new_len] = '\0';
    return new_len;
}

/* Decodes an XIMText's string into a freshly allocated UTF-8 buffer
 * (XIM hands either multi-byte in the locale's encoding — UTF-8 for
 * us since the XIC rides Xutf8 — or wide chars). NULL when the text
 * carries no string (pure deletion draws). */
static char *preedit_ximtext_to_utf8(XIMText *text) {
    if (text == NULL) {
        return NULL;
    }
    if (text->encoding_is_wchar) {
        /* Wide-char form: convert to UTF-8 ourselves (the multibyte
         * form is what our locale mode produces, but an IM may
         * answer either). */
        if (text->string.wide_char == NULL || text->length == 0) {
            return NULL;
        }
        size_t cap = (size_t)text->length * 4u + 1u;
        char *out = fdk_alloc(cap);
        if (out == NULL) {
            return NULL;
        }
        size_t n = 0;
        for (int i = 0; i < text->length; i++) {
            unsigned long cp = (unsigned long)text->string.wide_char[i];
            if (cp < 0x80u) {
                out[n++] = (char)cp;
            } else if (cp < 0x800u) {
                out[n++] = (char)(0xC0u | (cp >> 6));
                out[n++] = (char)(0x80u | (cp & 0x3Fu));
            } else if (cp < 0x10000u) {
                out[n++] = (char)(0xE0u | (cp >> 12));
                out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                out[n++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                out[n++] = (char)(0xF0u | (cp >> 18));
                out[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                out[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
        }
        out[n] = '\0';
        return out;
    }
    if (text->string.multi_byte == NULL || text->length == 0) {
        return NULL;
    }
    /* Multi-byte in the CURRENT LOCALE. We bring the XIC up under
     * Xutf8 semantics (the locale is UTF-8 on every modern desktop;
     * setlocale ran in fdk__x11_ensure_xim) — copy is the decode. */
    size_t n = strlen(text->string.multi_byte);
    char *out = fdk_alloc(n + 1);
    if (out != NULL) {
        memcpy(out, text->string.multi_byte, n + 1);
    }
    return out;
}

/* ---- the XIM preedit callbacks ------------------------------------------ */

/* Per-window callback registration: XIMCallback carries its own
 * client_data, so each window's IC gets its OWN array (freed at
 * window destroy — the IC may reference the structs). */
typedef struct {
    XIMCallback start;
    XIMCallback draw;
    XIMCallback done;
} fdk_x11_preedit_cbs;

/* Composition starts: the IM asks for the max preedit size we can
 * hold. -1 = no limit (we grow the buffer per draw); XIM's contract
 * only uses the answer for its own planning. */
static int xim_preedit_start(XIC ic, XPointer client, XPointer call) {
    (void)ic;
    (void)call;
    (void)client; /* the pwindow — see fdk_x11_ime_attach_cbs */
    return -1;
}

/* Composition ends: clear the buffer and tell the window. */
static void xim_preedit_done(XIC ic, XPointer client, XPointer call) {
    (void)ic;
    (void)call;
    fdk_platform_window *pwindow = (fdk_platform_window *)client;
    if (pwindow == NULL || pwindow->ime_preedit == NULL) {
        return;
    }
    fdk_x11_ime_dispatch(pwindow, FDK_EVENT_IME_PREEDIT, "", 0);
    pwindow->ime_preedit[0] = '\0';
    pwindow->ime_preedit_showing = 0;
}

/* The buffer math entry point: one draw = one apply. */
static void xim_preedit_draw(XIC ic, XPointer client,
                             XPointer call_data) {
    (void)ic;
    fdk_platform_window *pwindow = (fdk_platform_window *)client;
    XIMPreeditDrawCallbackStruct *draw =
        (XIMPreeditDrawCallbackStruct *)call_data;
    if (pwindow == NULL || draw == NULL) {
        return;
    }

    /* Ensure the buffer exists (lazily; the common desktop that
     * never composes never allocates). */
    if (pwindow->ime_preedit == NULL) {
        pwindow->ime_preedit_cap = 128;
        pwindow->ime_preedit = fdk_alloc(pwindow->ime_preedit_cap);
        if (pwindow->ime_preedit == NULL) {
            pwindow->ime_preedit_cap = 0;
            return;
        }
        pwindow->ime_preedit[0] = '\0';
    }

    char *insert = preedit_ximtext_to_utf8(draw->text);
    size_t r = fdk__x11_preedit_apply(pwindow->ime_preedit,
                                      pwindow->ime_preedit_cap,
                                      draw->chg_first, draw->chg_length,
                                      insert);
    fdk_free(insert);
    if (r == (size_t)-1) {
        /* Refused (overflow or bad indices): grow once for overflow
         * shapes and retry; a truly hostile draw resets the buffer —
         * a corrupted composition is worse than a cleared one. */
        if (draw->chg_first >= 0 &&
            (long)preedit_char_count(pwindow->ime_preedit) >=
                draw->chg_first) {
            size_t want = pwindow->ime_preedit_cap * 2u;
            char *grown = fdk_alloc(want);
            if (grown != NULL) {
                memcpy(grown, pwindow->ime_preedit,
                       pwindow->ime_preedit_cap);
                fdk_free(pwindow->ime_preedit);
                pwindow->ime_preedit = grown;
                pwindow->ime_preedit_cap = want;
                char *ins2 = preedit_ximtext_to_utf8(draw->text);
                r = fdk__x11_preedit_apply(pwindow->ime_preedit,
                                           pwindow->ime_preedit_cap,
                                           draw->chg_first,
                                           draw->chg_length, ins2);
                fdk_free(ins2);
            }
        }
        if (r == (size_t)-1) {
            pwindow->ime_preedit[0] = '\0';
        }
    }

    /* Tell the window: the composition text (empty = cleared). */
    fdk_x11_ime_dispatch(pwindow, FDK_EVENT_IME_PREEDIT,
                         pwindow->ime_preedit, 0);
    pwindow->ime_preedit_showing = pwindow->ime_preedit[0] != '\0';
}

/* Build + ATTACH the per-window callback array; the nested list is
 * returned for XCreateIC's XNPreeditAttributes (the CALLER frees it
 * with XFree after the IC exists). Returns NULL when the window
 * already has callbacks attached (idempotent) or on allocation
 * failure.
 *
 * XNVaNestedList is a STRING tag for XCreateIC's varargs, not a
 * function — the programmatic builder is XVaCreateNestedList (same
 * shape: name/value pairs, NULL-terminated, freed with XFree). */
void *fdk_x11_ime_attach_cbs(fdk_platform_window *pwindow) {
    if (pwindow == NULL || pwindow->ime_cbs != NULL) {
        return NULL;
    }
    fdk_x11_preedit_cbs *cbs = fdk_alloc(sizeof(*cbs));
    if (cbs == NULL) {
        return NULL;
    }
    /* Xlib's own API contract: the preedit callbacks are declared
     * XIMProc (void return, XIC-first) but preedit START must return
     * int — every XIM client casts (XIMPreeditStartCallbackProc's
     * signature predates the typedef unification). The cast is the
     * documented mechanism, not a defect; silenced at the narrowest
     * scope like the vendored-code pragmas. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    cbs->start.callback = (XIMProc)xim_preedit_start;
    cbs->start.client_data = (XPointer)pwindow;
    cbs->draw.callback = (XIMProc)xim_preedit_draw;
    cbs->draw.client_data = (XPointer)pwindow;
    cbs->done.callback = (XIMProc)xim_preedit_done;
    cbs->done.client_data = (XPointer)pwindow;
#pragma GCC diagnostic pop
    pwindow->ime_cbs = cbs;
    return XVaCreateNestedList(0, XNPreeditStartCallback, &cbs->start,
                               XNPreeditDrawCallback, &cbs->draw,
                               XNPreeditDoneCallback, &cbs->done, NULL);
}

/* ---- the dispatch helper (also the test seam's body) --------------------- */

void fdk_x11_ime_dispatch(fdk_platform_window *pwindow,
                          fdk_event_type type, const char *text,
                          int cursor) {
    if (pwindow == NULL || pwindow->conn == NULL) {
        return;
    }
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.ime.text = (text != NULL) ? text : "";
    event.ime.cursor = cursor;
    pwindow->conn->dispatch(pwindow, &event,
                            pwindow->conn->dispatch_user_data);
}

/* Test seam: drive the REAL draw-callback math against a raw buffer
 * (the sandbox has no XIM server; the buffer math is what needs
 * pinning — tests/test_ime.c). Returns the new byte length or
 * (size_t)-1, exactly fdk__x11_preedit_apply's contract. */
size_t fdk_x11_ime_test_apply(char *buf, size_t cap, long chg_first,
                              long chg_length, const char *insert) {
    return fdk__x11_preedit_apply(buf, cap, chg_first, chg_length, insert);
}

/* Per-window teardown (from fdk_x11_window_destroy). */
void fdk_x11_ime_window_cleanup(fdk_platform_window *pwindow) {
    if (pwindow == NULL) {
        return;
    }
    fdk_free(pwindow->ime_preedit);
    pwindow->ime_preedit = NULL;
    pwindow->ime_preedit_cap = 0;
    pwindow->ime_preedit_showing = 0;
    fdk_free(pwindow->ime_cbs);
    pwindow->ime_cbs = NULL;
}
