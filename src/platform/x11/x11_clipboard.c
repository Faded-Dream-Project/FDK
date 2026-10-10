#define FDK_LOG_TAG "x11"

/*
 * x11_clipboard.c — ICCCM CLIPBOARD + PRIMARY selections (Phase 9;
 * 1.3.4 adds PRIMARY)
 *
 * The design follows the ICCCM selection model exactly, and the SAME
 * machinery serves BOTH selections through a tiny descriptor —
 * CLIPBOARD (the explicit copy/paste buffer) and PRIMARY (the
 * classic Unix "whatever is selected" buffer that middle-click
 * pastes). They differ in exactly one thing each: the atom, and
 * which owned-text slot holds the copy.
 *
 *   - OWNERSHIP: set_text() calls XSetSelectionOwner on the
 *     connection-private clip_helper window. FDK keeps the text and
 *     serves it to ANY client that asks, for as long as it owns the
 *     selection. CLIPBOARD sets verify ownership took effect (the
 *     documented best-effort contract); PRIMARY sets are fire-and-
 *     forget — the classic model re-owns on every selection change,
 *     and a verification round-trip per drag-motion would be absurd.
 *
 *   - SERVING: SelectionRequest events arrive on the helper (the
 *     requestor names its own window + property; we must answer with
 *     a SelectionNotify-shaped ClientMessage whether we grant or
 *     refuse). TARGETS gets the format list; each supported text
 *     target gets the bytes. Refused targets get property = None.
 *
 *   - LOSING: SelectionClear arrives when another client takes
 *     ownership; FDK frees the matching copy and stops serving.
 *
 *   - READING: get_text() asks the current owner to convert into a
 *     private property on the helper (XConvertSelection), then waits
 *     for the SelectionNotify — bounded (250 ms) and WITHOUT
 *     dispatching any other event re-entrantly: XCheckTypedWindowEvent
 *     scans Xlib's queue and removes ONLY the notification we are
 *     waiting for, leaving every other event (input, expose, ...)
 *     untouched for the normal dispatch loop. poll() on the
 *     connection fd provides the bounded wait.
 *
 * Not supported, deliberately (see fdk_clipboard.h): COMPOUND_TEXT
 * (we serve UTF8_STRING/TEXT/STRING, which every modern client
 * accepts).
 *
 * 1.4.9 adds the IMAGE surface: image/png as a served/read target.
 * One clipboard content at a time: owning an image clears the owned
 * text and vice versa.
 *
 * 1.4.12 adds INCR (incremental) transfers, BOTH directions: the
 * ICCCM's chunked protocol for payloads beyond the atomic cap
 * (4 MiB). SERVING is event-driven — the requestor's deletes of the
 * property (PropertyNotify on ITS window, which we select) trigger
 * each next chunk, ending with the zero-length terminator; a flight
 * aborts gracefully when the owned content changes underneath it.
 * READING pumps PropertyNotify (NewValue) on the helper with a
 * per-chunk deadline and a whole-flight bound; the helper selects
 * PropertyChangeMask for exactly this. The hard payload bound is
 * 64 MiB both ways. One TX flight at a time (a new request
 * supersedes — a documented simplification; the read side is
 * naturally serial).
 */

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xatom.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FDK_CLIP_WAIT_MS 250
/* The atomic property cap: payloads at or under this size serve (and
 * read) in ONE property write/read, exactly as every FDK clipboard
 * transfer did before 1.4.12. Comfortably inside every real server's
 * max-request size while covering ordinary screenshots and UI
 * renders. */
#define FDK_CLIP_ATOMIC_MAX_BYTES (4u * 1024u * 1024u)

/* ---- INCR (incremental) transfer parameters (1.4.12) -------------
 *
 * Payloads beyond the atomic cap stream in chunks, per the ICCCM's
 * incremental-transfer protocol: the owner seeds the requestor's
 * property with type INCR and a 32-bit byte estimate; the requestor
 * deletes it; the owner answers each delete with the next chunk and
 * ends the flight with a zero-length chunk. Both directions ride
 * this module. */

/* Chunk size: the guaranteed protocol minimum max-request size is
 * 262144 bytes for the WHOLE request; XChangeProperty's overhead is
 * small but real, so stay a little under. (Servers typically accept
 * 16 MiB — the conservatism buys portability, and throughput at 256
 * KiB per roundtrip is a non-issue locally.) */
#define FDK_CLIP_INCR_CHUNK_BYTES 262080

/* The hard payload bound for incremental transfers (both directions):
 * a sanity alloc limit, the X11 twin of the Wayland pipe's 32 MiB
 * read bound (matched generously — a clipboard is not a file
 * transfer service). */
#define FDK_CLIP_INCR_MAX_BYTES (64u * 1024u * 1024u)

/* Per-chunk wait and whole-flight deadline for the READER (the
 * serve side is event-driven and never blocks): owners answer
 * deletes in microseconds locally; a dead owner must not hang the
 * caller's thread for more than the deadlines below. */
#define FDK_CLIP_INCR_CHUNK_WAIT_MS 1000
#define FDK_CLIP_INCR_TOTAL_WAIT_MS 30000

/* ---- the selection descriptor (one machinery, two selections) ---- */

/* INCR serve internals (defined below — the abort is needed early by
 * the content-changing paths). */
static void incr_tx_end(fdk_platform_connection *conn, bool write_end);

typedef struct {
    Atom atom;         /* the selection this descriptor addresses */
    char **owned;      /* points at the connection's owned-text slot */
    const char *name;  /* log vocabulary */
} sel_desc;

static sel_desc sel_of_clipboard(fdk_platform_connection *conn) {
    sel_desc d = {conn->atom_clipboard, &conn->clip_owned_text,
                  "clipboard"};
    return d;
}

static sel_desc sel_of_primary(fdk_platform_connection *conn) {
    sel_desc d = {XA_PRIMARY, &conn->primary_owned_text, "primary"};
    return d;
}

/* ---- ownership ---- */

fdk_result fdk_x11_clipboard_init(fdk_platform_connection *conn) {
    /* A never-mapped InputOnly window: no pixels, no buffer, invisible
     * to everyone — a pure protocol identity for selection traffic. */
    conn->clip_helper = XCreateWindow(
        conn->display, conn->root, 0, 0, 1, 1, 0, 0, InputOnly,
        CopyFromParent, 0, NULL);
    if (conn->clip_helper == None) {
        FDK_WARN("clipboard: could not create helper window");
        return FDK_ERR_PLATFORM;
    }
    /* 1.4.12: the helper watches its own properties — the read side
     * of INCR pumps PropertyNotify (NewValue) on atom_fdk_selection.
     * Event masks are per-client per-window, so this cannot disturb
     * anyone else's selections on the helper (XDND replies are
     * SendEvent'd to the creator and arrive regardless). */
    XSelectInput(conn->display, conn->clip_helper, PropertyChangeMask);
    conn->atom_clipboard = XInternAtom(conn->display, "CLIPBOARD", False);
    conn->atom_targets = XInternAtom(conn->display, "TARGETS", False);
    conn->atom_incr = XInternAtom(conn->display, "INCR", False);
    conn->atom_text = XInternAtom(conn->display, "TEXT", False);
    conn->atom_text_plain =
        XInternAtom(conn->display, "text/plain;charset=utf-8", False);
    conn->atom_fdk_selection =
        XInternAtom(conn->display, "_FDK_SELECTION", False);
    conn->atom_image_png = XInternAtom(conn->display, "image/png",
                                       False);
    conn->clip_owned_text = NULL;
    conn->primary_owned_text = NULL;
    conn->clip_owned_png = NULL;
    conn->clip_owned_png_len = 0;
    return FDK_OK;
}

void fdk_x11_clipboard_shutdown(fdk_platform_connection *conn) {
    /* End any in-flight incremental serve WITHOUT the terminating
     * write (the requestor's read times out — teardown is not the
     * moment for protocol traffic), then drop the session's copy. */
    incr_tx_end(conn, false);
    if (conn->display_dead) {
        /* Server gone: ownership died with the connection — free the
         * local copies and leave the XIDs to the dead socket. */
        conn->clip_helper = None;
        fdk_free(conn->clip_owned_text);
        conn->clip_owned_text = NULL;
        fdk_free(conn->primary_owned_text);
        conn->primary_owned_text = NULL;
        fdk_free(conn->clip_owned_png);
        conn->clip_owned_png = NULL;
        conn->clip_owned_png_len = 0;
        return;
    }
    if (conn->clip_helper != None) {
        /* Relinquish ownership (if any) before destroying the helper,
         * so the server-side owner field never points at a dead
         * window. XDestroyWindow on an unmapped InputOnly window is
         * cheap and cannot fail meaningfully. */
        if (XGetSelectionOwner(conn->display, conn->atom_clipboard) ==
            conn->clip_helper) {
            XSetSelectionOwner(conn->display, conn->atom_clipboard, None,
                               CurrentTime);
        }
        if (XGetSelectionOwner(conn->display, XA_PRIMARY) ==
            conn->clip_helper) {
            XSetSelectionOwner(conn->display, XA_PRIMARY, None,
                               CurrentTime);
        }
        XDestroyWindow(conn->display, conn->clip_helper);
        conn->clip_helper = None;
    }
    fdk_free(conn->clip_owned_text);
    conn->clip_owned_text = NULL;
    fdk_free(conn->primary_owned_text);
    conn->primary_owned_text = NULL;
    fdk_free(conn->clip_owned_png);
    conn->clip_owned_png = NULL;
    conn->clip_owned_png_len = 0;
}

/* One ownership+store, parameterized over the selection. verify:
 * CLIPBOARD checks the server actually moved ownership (the public
 * set's documented contract); PRIMARY skips the round-trip — the
 * classic model re-owns per selection change and a verification per
 * drag-motion would be absurd. */
static fdk_result selection_set_text(fdk_platform_connection *conn,
                                     sel_desc sel, const char *text,
                                     bool verify) {
    if (text == NULL) {
        text = "";
    }
    size_t len = strlen(text);
    char *copy = fdk_alloc(len + 1);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, len + 1);

    /* Abort an in-flight incremental serve that streams the content
     * being replaced (graceful zero chunk) BEFORE the bytes vanish. */
    if (conn->clip_incr_tx.active && !conn->clip_incr_tx.owns_bytes &&
        conn->clip_incr_tx.bytes == (const unsigned char *)*sel.owned) {
        incr_tx_end(conn, true);
    }

    /* ICCCM: acquiring ownership with CurrentTime is legal for
     * programs that have not seen a timestamp; owners should treat
     * requests with older timestamps as stale. (The replace-ownership
     * race this window technically allows is the same one every
     * toolkit accepts for clipboard sets.) */
    XSetSelectionOwner(conn->display, sel.atom, conn->clip_helper,
                       CurrentTime);
    if (verify && XGetSelectionOwner(conn->display, sel.atom) !=
                         conn->clip_helper) {
        fdk_free(copy);
        FDK_WARN("%s: XSetSelectionOwner did not take effect", sel.name);
        return FDK_ERR_PLATFORM;
    }
    fdk_free(*sel.owned);
    *sel.owned = copy;
    XFlush(conn->display);
    return FDK_OK;
}

fdk_result fdk_x11_clipboard_set_text(fdk_platform_connection *conn,
                                      const char *text) {
    /* One content at a time (1.4.9): text replaces the clipboard's
     * image. (PRIMARY's set does NOT touch the clipboard image —
     * different selection, different content.) */
    fdk_free(conn->clip_owned_png);
    conn->clip_owned_png = NULL;
    conn->clip_owned_png_len = 0;
    return selection_set_text(conn, sel_of_clipboard(conn), text, true);
}

fdk_result fdk_x11_clipboard_set_primary_text(
    fdk_platform_connection *conn, const char *text) {
    return selection_set_text(conn, sel_of_primary(conn), text, false);
}

/* ---- serving (SelectionRequest / SelectionClear) ---- */

/* Appends `count` atoms to the TARGETS reply. */
static void serve_targets(fdk_platform_connection *conn, Window requestor,
                          Atom property) {
    Atom targets[5];
    int n = 0;
    targets[n++] = conn->utf8_string;
    targets[n++] = conn->atom_text_plain;
    targets[n++] = conn->atom_text;
    targets[n++] = XA_STRING;
    if (conn->clip_owned_png != NULL) {
        targets[n++] = conn->atom_image_png; /* 1.4.9 */
    }
    XChangeProperty(conn->display, requestor, property, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)targets, n);
}

/* Latin-1 re-encoding of our UTF-8 text for the legacy XA_STRING /
 * TEXT targets (both are defined as Latin-1). Unencodable codepoints
 * become '?'. Returns a malloc'd NUL-terminated buffer (Xlib frees
 * property data with XFree, so plain malloc/free is the right pair
 * here — same discipline as conn->app_id). */
static char *latin1_from_utf8(const char *utf8, size_t *out_len) {
    size_t in_len = strlen(utf8);
    char *out = malloc(in_len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < in_len;) {
        unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80) {
            out[o++] = (char)c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < in_len &&
                   ((unsigned char)utf8[i + 1] & 0xC0) == 0x80) {
            unsigned cp = ((unsigned)(c & 0x1F) << 6) |
                          (unsigned char)(utf8[i + 1] & 0x3F);
            /* 2-byte UTF-8 encodes U+0080..U+07FF; Latin-1 covers the
             * U+0080..U+00FF slice of it. */
            out[o++] = (cp <= 0xFF) ? (char)cp : '?';
            i += 2;
        } else {
            /* 3/4-byte sequences (and any malformed byte): not in
             * Latin-1. '?' is the honest per-character fallback. */
            out[o++] = '?';
            i += 1;
            while (i < in_len &&
                   ((unsigned char)utf8[i] & 0xC0) == 0x80) {
                i += 1; /* skip continuation bytes of the sequence */
            }
        }
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

/* ---- INCR serving (1.4.12): we own a payload beyond the atomic cap --
 *
 * The ICCCM incremental dance, owner side: seed the requestor's
 * property with type INCR and a 32-bit byte estimate; watch the
 * requestor's window (PropertyChangeMask — per-client masks, so this
 * never disturbs the requestor's own selections); each PropertyDelete
 * of the property answers with the next chunk; a zero-length chunk
 * ends the flight. All chunk writes are event-driven from dispatch —
 * the serve side never blocks. A dead requestor surfaces as a
 * tolerated protocol error (the connection's 1.4.12 error handler)
 * and the flight simply stalls until the next content change
 * supersedes it. */

/* Ends the in-flight transfer (if any) with the graceful zero-length
 * chunk, deselects the requestor's window, and clears the session.
 * `write_end` false skips the terminating write (shutdown / dead
 * display): the requestor's read times out instead. */
static void incr_tx_end(fdk_platform_connection *conn, bool write_end) {
    if (!conn->clip_incr_tx.active) {
        return;
    }
    if (write_end && !conn->display_dead) {
        XChangeProperty(conn->display, conn->clip_incr_tx.requestor,
                        conn->clip_incr_tx.property,
                        conn->clip_incr_tx.type, 8, PropModeReplace,
                        NULL, 0);
        XSelectInput(conn->display, conn->clip_incr_tx.requestor, 0);
        XFlush(conn->display);
    }
    if (conn->clip_incr_tx.owns_bytes) {
        /* The Latin-1 legacy path: the session owned its copy. The
         * launder goes through unsigned char * (the session's own
         * non-const storage discipline — borrowed flights never take
         * this branch). */
        fdk_free((unsigned char *)(uintptr_t)conn->clip_incr_tx.bytes);
    }
    conn->clip_incr_tx.active = false;
    conn->clip_incr_tx.bytes = NULL;
}

/* Seeds a new incremental serve of `len` bytes at `bytes` (borrowed
 * from the owned content unless `owns_bytes`). Supersedes any flight
 * in progress. The caller still sends its SelectionNotify naming the
 * property — the INCR seed is what the requestor reads first. */
static void incr_tx_begin(fdk_platform_connection *conn, Window requestor,
                          Atom property, Atom type,
                          const unsigned char *bytes, size_t len,
                          bool owns_bytes) {
    incr_tx_end(conn, true); /* graceful end for the superseded flight */

    /* Watch the requestor's property deletes. Per-client event masks
     * make this safe against the requestor's own (and third parties')
     * selections on the same window. */
    XSelectInput(conn->display, requestor, PropertyChangeMask);

    /* The INCR seed property: type INCR, format 32, one value = the
     * byte estimate, rounded up to a multiple of 4 (the format's
     * unit) as the protocol's estimate semantics expect. */
    unsigned long estimate = (unsigned long)((len + 3u) & ~(size_t)3u);
    XChangeProperty(conn->display, requestor, property, conn->atom_incr,
                    32, PropModeReplace,
                    (const unsigned char *)&estimate, 1);

    conn->clip_incr_tx.requestor = requestor;
    conn->clip_incr_tx.property = property;
    conn->clip_incr_tx.type = type;
    conn->clip_incr_tx.bytes = bytes;
    conn->clip_incr_tx.len = len;
    conn->clip_incr_tx.pos = 0;
    conn->clip_incr_tx.owns_bytes = owns_bytes;
    conn->clip_incr_tx.active = true;
}

static void incr_serve_begin(fdk_platform_connection *conn, Window requestor,
                             Atom property, Atom type,
                             const unsigned char *bytes, size_t len) {
    incr_tx_begin(conn, requestor, property, type, bytes, len, false);
}

static void incr_serve_begin_owned(fdk_platform_connection *conn,
                                   Window requestor, Atom property,
                                   Atom type, unsigned char *bytes,
                                   size_t len) {
    incr_tx_begin(conn, requestor, property, type, bytes, len, true);
}

/* One PropertyDelete from the active requestor: stream the next chunk
 * (or the zero-length terminator when the bytes ran out). */
static void incr_tx_next_chunk(fdk_platform_connection *conn) {
    if (!conn->clip_incr_tx.active) {
        return;
    }
    size_t remain = conn->clip_incr_tx.len - conn->clip_incr_tx.pos;
    if (remain == 0) {
        /* The requestor deleted the LAST data chunk: the zero-length
         * terminator ends the flight per the ICCCM. */
        incr_tx_end(conn, true);
        return;
    }
    size_t chunk = remain > FDK_CLIP_INCR_CHUNK_BYTES
                       ? FDK_CLIP_INCR_CHUNK_BYTES
                       : remain;
    XChangeProperty(conn->display, conn->clip_incr_tx.requestor,
                    conn->clip_incr_tx.property, conn->clip_incr_tx.type,
                    8, PropModeReplace,
                    conn->clip_incr_tx.bytes + conn->clip_incr_tx.pos,
                    (int)chunk);
    XFlush(conn->display);
    conn->clip_incr_tx.pos += chunk;
}

static void serve_text(fdk_platform_connection *conn, sel_desc sel,
                       Window requestor, Atom property, Atom target) {
    const char *owned = *sel.owned;
    if (owned == NULL) {
        return; /* property left unset -> refusal */
    }
    if (target == conn->utf8_string || target == conn->atom_text_plain) {
        size_t len = strlen(owned);
        if (len > FDK_CLIP_ATOMIC_MAX_BYTES) {
            /* 1.4.12: beyond the atomic cap, stream incrementally. */
            incr_serve_begin(conn, requestor, property, target,
                             (const unsigned char *)owned, len);
            return;
        }
        XChangeProperty(conn->display, requestor, property, target, 8,
                        PropModeReplace,
                        (const unsigned char *)owned,
                        (int)len);
    } else { /* XA_STRING or TEXT: Latin-1 */
        size_t len = 0;
        char *latin = latin1_from_utf8(owned, &len);
        if (latin == NULL) {
            return;
        }
        if (len > FDK_CLIP_ATOMIC_MAX_BYTES) {
            /* The Latin-1 re-encode is a temporary buffer — the INCR
             * flight cannot point into it. Stream it through a
             * session that OWNS its bytes (the tx takes ownership of
             * the malloc'd copy and frees it at flight end — the one
             * place a second copy exists, and only for legacy
             * targets larger than the atomic cap, a pathological
             * corner by any measure). */
            incr_serve_begin_owned(conn, requestor, property, XA_STRING,
                                   (unsigned char *)latin, len);
            return;
        }
        XChangeProperty(conn->display, requestor, property,
                        (target == conn->atom_text) ? XA_STRING : XA_STRING,
                        8, PropModeReplace,
                        (const unsigned char *)latin, (int)len);
        free(latin);
    }
}

int fdk_x11_clipboard_handle_event(fdk_platform_connection *conn,
                                   const XEvent *xevent) {
    /* INCR chunk pump FIRST (1.4.12): PropertyNotify arrives on the
     * REQUESTOR's (foreign) window for an in-flight serve, and on
     * the helper for read-side traffic — both BEFORE the helper
     * guard, which would otherwise discard foreign-window events. */
    if (xevent->type == PropertyNotify) {
        const XPropertyEvent *pe = &xevent->xproperty;
        if (pe->window == conn->clip_helper) {
            /* Read-side strays (the deletes our own chunk reads
             * generate): consumed by the reader's wait loop when it
             * is running, swallowed here when it is not. */
            return 1;
        }
        if (conn->clip_incr_tx.active &&
            pe->window == conn->clip_incr_tx.requestor &&
            pe->atom == conn->clip_incr_tx.property &&
            pe->state == PropertyDelete) {
            incr_tx_next_chunk(conn);
            return 1;
        }
        return 0; /* someone else's property traffic: not ours */
    }
    if (conn->clip_helper == None ||
        xevent->xany.window != conn->clip_helper) {
        return 0;
    }
    if (xevent->type == SelectionRequest) {
        const XSelectionRequestEvent *req = &xevent->xselectionrequest;
        Atom property = req->property != None ? req->property : req->target;
        /* Which selection is being asked of us? Both selections share
         * the helper window, so the ATOM is the discriminator. */
        sel_desc sel;
        if (req->selection == XA_PRIMARY) {
            sel = sel_of_primary(conn);
        } else if (req->selection == conn->atom_clipboard) {
            sel = sel_of_clipboard(conn);
        } else {
            sel.atom = req->selection; /* never owned; refuse politely */
            sel.owned = NULL;
            sel.name = "selection";
            property = None;
        }

        bool have_content =
            (sel.owned != NULL && *sel.owned != NULL) ||
            (req->selection == conn->atom_clipboard &&
             conn->clip_owned_png != NULL);
        if (!have_content) {
            /* Not the owner anymore (a stale request raced our
             * SelectionClear), or an unknown selection: refuse per the
             * ICCCM — the reply must name property None, so the
             * requestor does not mistake an unwritten property for
             * content. */
            property = None;
        } else if (req->target == conn->atom_targets) {
            serve_targets(conn, req->requestor, property);
        } else if (req->target == conn->atom_image_png &&
                   req->selection == conn->atom_clipboard &&
                   conn->clip_owned_png != NULL) {
            /* 1.4.9: the image target. 1.4.12: beyond the atomic cap,
             * stream incrementally. */
            if (conn->clip_owned_png_len > FDK_CLIP_ATOMIC_MAX_BYTES) {
                incr_serve_begin(conn, req->requestor, property,
                                 conn->atom_image_png,
                                 conn->clip_owned_png,
                                 conn->clip_owned_png_len);
            } else {
                XChangeProperty(conn->display, req->requestor, property,
                                conn->atom_image_png, 8, PropModeReplace,
                                conn->clip_owned_png,
                                (int)conn->clip_owned_png_len);
            }
        } else if (req->target == conn->utf8_string ||
                   req->target == conn->atom_text_plain ||
                   req->target == conn->atom_text ||
                   req->target == XA_STRING) {
            serve_text(conn, sel, req->requestor, property, req->target);
        } else {
            property = None; /* unknown target: explicit refusal */
        }

        XSelectionEvent reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = SelectionNotify;
        reply.display = req->display;
        reply.requestor = req->requestor;
        reply.selection = req->selection;
        reply.target = req->target;
        reply.property = property;
        reply.time = req->time;
        XSendEvent(conn->display, req->requestor, False, 0,
                   (XEvent *)&reply);
        XFlush(conn->display);
        return 1;
    }
    if (xevent->type == SelectionClear) {
        /* Ownership is server truth, not event-order truth: a
         * SelectionClear that was QUEUED before we re-acquired the
         * selection (e.g. the previous owner died, then we called
         * set_text, then the queue drained) must not drop the copy of
         * the NEW ownership epoch. Asking the server who owns it NOW
         * resolves the race the ICCCM way — for whichever selection
         * the clear names. */
        if (xevent->xselectionclear.selection == conn->atom_clipboard) {
            if (XGetSelectionOwner(conn->display, conn->atom_clipboard) !=
                conn->clip_helper) {
                /* Abort any in-flight incremental serve of the content
                 * being dropped (graceful zero chunk) BEFORE the bytes
                 * it points at vanish. A session-owned flight (the
                 * Latin-1 re-encode) cannot be told apart per slot —
                 * ending it is conservative and always safe. */
                if (conn->clip_incr_tx.active &&
                    (conn->clip_incr_tx.owns_bytes ||
                     conn->clip_incr_tx.bytes ==
                         (const unsigned char *)conn->clip_owned_text ||
                     conn->clip_incr_tx.bytes ==
                         (const unsigned char *)conn->clip_owned_png)) {
                    incr_tx_end(conn, true);
                }
                fdk_free(conn->clip_owned_text);
                conn->clip_owned_text = NULL;
                fdk_free(conn->clip_owned_png);
                conn->clip_owned_png = NULL;
                conn->clip_owned_png_len = 0;
            }
        } else if (xevent->xselectionclear.selection == XA_PRIMARY) {
            if (XGetSelectionOwner(conn->display, XA_PRIMARY) !=
                conn->clip_helper) {
                if (conn->clip_incr_tx.active &&
                    !conn->clip_incr_tx.owns_bytes &&
                    conn->clip_incr_tx.bytes ==
                        (const unsigned char *)conn->primary_owned_text) {
                    incr_tx_end(conn, true);
                }
                fdk_free(conn->primary_owned_text);
                conn->primary_owned_text = NULL;
            }
        }
        return 1;
    }
    if (xevent->type == SelectionNotify) {
        /* Our own converts (from get_text) are consumed by that
         * function's wait loop; anything arriving here is a stray
         * (e.g. delivered after a timeout). Swallow it so it never
         * leaks into the normal dispatch path. */
        return 1;
    }
    return 0;
}

/* ---- reading (convert + bounded wait) ---- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Waits up to FDK_CLIP_WAIT_MS for the SelectionNotify answering a
 * convert of `selection` by `requestor` (1.4.14: parameterized — the
 * clipboard's helper window and a DnD drop target share this). 
 * Non-matching events are left in Xlib's queue untouched
 * (XCheckTypedWindowEvent removes only the match), so no event is
 * ever dispatched re-entrantly or lost. Returns 1 with *out_notify
 * filled on success, 0 on timeout. `wait_ms` is the caller's bound:
 * the clipboard's 250 (FDK_CLIP_WAIT_MS), the DnD drop fetch's 600
 * (FDK_XDND_WAIT_MS — a file manager under load answers slower than
 * toolkit-to-toolkit peers; a drop that vanishes because the source
 * was busy for 300 ms is a real report). */
static int wait_selection_notify_on(fdk_platform_connection *conn,
                                     Window requestor, Atom selection,
                                     int wait_ms, XEvent *out_notify) {
    uint64_t deadline = now_ms() + (uint64_t)wait_ms;
    XFlush(conn->display);
    for (;;) {
        XEvent ev;
        if (XCheckTypedWindowEvent(conn->display, requestor,
                                   SelectionNotify, &ev)) {
            if (ev.xselection.selection == selection) {
                *out_notify = ev;
                return 1;
            }
            /* Notification for the OTHER selection (we now convert
             * both): keep waiting on the remaining budget — 1.3.4
             * closed the "can't happen today" gap this comment used
             * to guard. */
            continue;
        }
        uint64_t now = now_ms();
        if (now >= deadline) {
            return 0;
        }
        struct pollfd pfd;
        pfd.fd = ConnectionNumber(conn->display);
        pfd.events = POLLIN;
        int r = poll(&pfd, 1, (int)(deadline - now));
        if (r < 0 && errno != EINTR) {
            return 0;
        }
        /* Readable (or spurious): pull bytes into Xlib's queue and
         * rescan. QueuedAfterReading flushes output first, which also
         * keeps our convert request moving. */
        XEventsQueued(conn->display, QueuedAfterReading);
    }
}

/* Property bytes -> NUL-terminated UTF-8. type may be UTF8_STRING /
 * text-plain (bytes are UTF-8 already) or XA_STRING (Latin-1 ->
 * widen to UTF-8). Returns a fdk_alloc'd string, or NULL. */
static char *utf8_from_property(Atom type, const unsigned char *data,
                                unsigned long len) {
    if (type == XA_STRING) {
        /* Worst case: every Latin-1 byte >= 0x80 becomes 2 UTF-8
         * bytes. */
        char *out = fdk_alloc(len * 2 + 1);
        if (out == NULL) {
            return NULL;
        }
        size_t o = 0;
        for (unsigned long i = 0; i < len; i++) {
            unsigned char c = data[i];
            if (c < 0x80) {
                out[o++] = (char)c;
            } else {
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        out[o] = '\0';
        return out;
    }
    /* UTF-8 passthrough. Byte-wise validity is NOT re-checked: the
     * owner declared the type, and a malformed payload is the kind
     * of hostile input docs/security.md says to contain, not
     * re-parse — the string just round-trips as bytes. */
    char *out = fdk_alloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

/* Waits for one PropertyNotify (NewValue) on `requestor`'s selection
 * property — the INCR read pump (1.4.14: parameterized requestor).
 * Delete notifications are SKIPPED (each chunk we read+delete
 * generates one); foreign events are left in Xlib's queue
 * untouched. Returns 1 with *out filled, 0 on the deadline. */
static int wait_property_new_value(fdk_platform_connection *conn,
                                    Window requestor, uint64_t deadline,
                                    XEvent *out) {
    XFlush(conn->display);
    for (;;) {
        XEvent ev;
        if (XCheckTypedWindowEvent(conn->display, requestor,
                                   PropertyNotify, &ev)) {
            if (ev.xproperty.atom == conn->atom_fdk_selection &&
                ev.xproperty.state == PropertyNewValue) {
                *out = ev;
                return 1;
            }
            continue; /* our own deletes / other properties: skip */
        }
        uint64_t now = now_ms();
        if (now >= deadline) {
            return 0;
        }
        struct pollfd pfd;
        pfd.fd = ConnectionNumber(conn->display);
        pfd.events = POLLIN;
        int r = poll(&pfd, 1, (int)(deadline - now));
        if (r < 0 && errno != EINTR) {
            return 0;
        }
        XEventsQueued(conn->display, QueuedAfterReading);
    }
}

/* The INCR read loop (1.4.12; requestor-parameterized 1.4.14): the
 * seed property (type INCR, a 32-bit byte estimate) is already
 * consumed by the caller; chunks arrive as NewValue notifications on
 * `requestor`'s selection property, each read WITH delete (the
 * delete is what tells the owner to send the next chunk), until the
 * zero-length terminator. Bounded by a per-chunk deadline and a
 * whole-flight deadline; the total is bounded by
 * FDK_CLIP_INCR_MAX_BYTES. Returns the concatenated bytes
 * (fdk_alloc'd, *out_len set) or NULL.
 *
 * A FOREIGN requestor (a DnD drop target — anything but the
 * clipboard helper) has not selected PropertyChangeMask; the flight
 * adds it for the duration and restores the window's own mask
 * afterwards (XGetWindowAttributes reports OUR selection — one
 * display connection, one client). */
static unsigned char *incr_read_all(fdk_platform_connection *conn,
                                     Window requestor,
                                     unsigned long estimate,
                                     size_t *out_len) {
    *out_len = 0;
    bool foreign = (requestor != conn->clip_helper);
    long saved_mask = 0;
    if (foreign) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(conn->display, requestor, &wa)) {
            return NULL; /* dying window: not our problem to solve */
        }
        saved_mask = (long)wa.your_event_mask;
        XSelectInput(conn->display, requestor,
                     saved_mask | PropertyChangeMask);
    }
    unsigned char *result = NULL;
    size_t cap = (size_t)estimate;
    if (cap == 0 || cap > FDK_CLIP_INCR_MAX_BYTES) {
        cap = FDK_CLIP_INCR_MAX_BYTES; /* estimate is a HINT, not truth */
    }
    unsigned char *buf = fdk_alloc(cap + 1); /* +1: text NUL room */
    if (buf == NULL) {
        goto out;
    }
    size_t got = 0;
    uint64_t flight_deadline = now_ms() + FDK_CLIP_INCR_TOTAL_WAIT_MS;

    for (;;) {
        uint64_t chunk_deadline = now_ms() + FDK_CLIP_INCR_CHUNK_WAIT_MS;
        uint64_t deadline = chunk_deadline < flight_deadline
                                ? chunk_deadline
                                : flight_deadline;
        XEvent ev;
        if (!wait_property_new_value(conn, requestor, deadline, &ev)) {
            FDK_WARN("clipboard: INCR read stalled (owner silent past "
                     "the deadline after %zu bytes)",
                     got);
            goto out;
        }
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(conn->display, requestor,
                               conn->atom_fdk_selection, 0, 0x400000, True,
                               AnyPropertyType, &type, &format, &nitems,
                               &bytes_after, &data) != Success) {
            goto out;
        }
        if (type == None) {
            /* A stale NewValue: the owner's SEED WRITE generates one
             * before our delete of the seed, and it is still queued
             * when the chunk loop starts — reading then finds the
             * property GONE (we deleted it). A vanished property is
             * not a chunk, not an error: skip and keep waiting under
             * the same deadlines. (The terminator, by contrast, is a
             * TYPE-CORRECT zero-length property, handled below.) */
            if (data != NULL) {
                XFree(data);
            }
            continue;
        }
        if (data == NULL || format != 8) {
            if (data != NULL) {
                XFree(data);
            }
            goto out;
        }
        if (nitems == 0) {
            /* The zero-length terminator: the flight is complete. */
            XFree(data);
            *out_len = got;
            result = buf;
            buf = NULL;
            goto out;
        }
        if (got + (size_t)nitems > FDK_CLIP_INCR_MAX_BYTES) {
            FDK_WARN("clipboard: INCR payload exceeds the %u bound; "
                     "dropping the transfer",
                     FDK_CLIP_INCR_MAX_BYTES);
            XFree(data);
            goto out;
        }
        if (got + (size_t)nitems > cap) {
            /* The owner sent more than its estimate (a HINT — the
             * ICCCM allows it): grow to fit, still under the bound. */
            size_t ncap = got + (size_t)nitems;
            unsigned char *grown = fdk_realloc(buf, ncap + 1);
            if (grown == NULL) {
                XFree(data);
                goto out;
            }
            buf = grown;
            cap = ncap;
        }
        memcpy(buf + got, data, (size_t)nitems);
        got += (size_t)nitems;
        XFree(data);
    }

out:
    fdk_free(buf);
    if (foreign) {
        XSelectInput(conn->display, requestor, saved_mask);
    }
    return result;
}

/* One convert attempt for `target` against `sel`, returning the RAW
 * bytes plus the property type (UTF8_STRING / XA_STRING / image/png
 * / or an INCR flight's already-concatenated payload — the type the
 * owner NAMED). Returns NULL on refusal/timeout/malformation.
 * Thin wrapper: the engine (below) is requestor-parameterized since
 * 1.4.14 — the DnD drop fetch shares it with the app-window drop
 * target as requestor. */
static unsigned char *convert_selection_bytes(
    fdk_platform_connection *conn, sel_desc sel, Atom target,
    Atom *out_type, size_t *out_len) {
    return fdk__x11_read_selection(conn, conn->clip_helper, sel.atom,
                                   target, CurrentTime,
                                   FDK_CLIP_WAIT_MS, out_type, out_len);
}

/* THE selection-read engine (1.4.14): convert `selection`/`target`
 * onto `requestor`'s atom_fdk_selection property, wait the bounded
 * notify window, then read the answer — atomic (possibly multi-part)
 * or INCR (the seed's 32-bit value is the estimate; the read-with-
 * delete of the seed is the go signal; chunks pump until the
 * zero-length terminator). Foreign requestors get PropertyChangeMask
 * for the flight and their own mask back afterwards (incr_read_all).
 *
 * Every user of a selection READ lands here: the clipboard's text
 * and image gets (helper as requestor) and the XDND drop fetch (the
 * drop-target window as requestor — external file managers serve
 * text/uri-list via INCR for larger lists, which is exactly the drop
 * that used to vanish before this engine existed). `timestamp` is
 * the XDND drop timestamp for DnD, CurrentTime for the clipboard.
 *
 * Returns the RAW bytes (fdk_alloc'd, *out_len set, *out_type the
 * answer's property type — for an INCR flight, the type the owner
 * NAMED for the target), or NULL on refusal (property None in the
 * notify), timeout, or malformation. */
unsigned char *fdk__x11_read_selection(fdk_platform_connection *conn,
                                       Window requestor, Atom selection,
                                       Atom target, unsigned long timestamp,
                                       int notify_wait_ms, Atom *out_type,
                                       size_t *out_len) {
    *out_type = None;
    *out_len = 0;
    XConvertSelection(conn->display, selection, target,
                      conn->atom_fdk_selection, requestor,
                      (Time)timestamp);
    XEvent notify;
    if (!wait_selection_notify_on(conn, requestor, selection,
                                  notify_wait_ms, &notify)) {
        FDK_WARN("selection read: owner did not answer within %d ms",
                 notify_wait_ms);
        return NULL;
    }
    if (notify.xselection.property == None) {
        return NULL; /* owner refused this target */
    }
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(conn->display, requestor,
                           conn->atom_fdk_selection, 0, 0x400000, True,
                           AnyPropertyType, &type, &format, &nitems,
                           &bytes_after, &data) != Success) {
        return NULL;
    }
    if (type == conn->atom_incr) {
        /* Incremental transfer (1.4.12): the seed's single 32-bit
         * value is the byte estimate; the delete above (read with
         * delete=True) is already the protocol's go signal. Pump the
         * chunks. */
        unsigned long estimate = 0;
        if (data != NULL && nitems >= 1) {
            estimate = ((unsigned long *)data)[0];
        }
        if (data != NULL) {
            XFree(data);
        }
        unsigned char *all = incr_read_all(conn, requestor, estimate,
                                           out_len);
        if (all != NULL) {
            *out_type = target; /* the type the owner NAMED */
        }
        return all;
    }
    if (data == NULL || type == None || format != 8) {
        if (data != NULL) {
            XFree(data);
        }
        return NULL;
    }
    /* Atomic read — possibly multi-part when a foreign owner wrote
     * more than one read window (the delete lands with the LAST
     * part). Bounded by the incremental payload bound. */
    {
        size_t got = (size_t)nitems;
        unsigned char *out = fdk_alloc(got > 0 ? got : 1);
        if (out == NULL) {
            XFree(data);
            return NULL;
        }
        memcpy(out, data, got);
        XFree(data);
        while (bytes_after > 0 && got <= FDK_CLIP_INCR_MAX_BYTES) {
            data = NULL;
            if (XGetWindowProperty(conn->display, requestor,
                                   conn->atom_fdk_selection,
                                   (long)(got / 4), 0x400000, True,
                                   AnyPropertyType, &type, &format,
                                   &nitems, &bytes_after,
                                   &data) != Success ||
                data == NULL) {
                fdk_free(out);
                return NULL;
            }
            if (got + (size_t)nitems > FDK_CLIP_INCR_MAX_BYTES) {
                XFree(data);
                fdk_free(out);
                return NULL;
            }
            unsigned char *grown = fdk_realloc(out, got + (size_t)nitems);
            if (grown == NULL) {
                XFree(data);
                fdk_free(out);
                return NULL;
            }
            out = grown;
            memcpy(out + got, data, (size_t)nitems);
            got += (size_t)nitems;
            XFree(data);
        }
        *out_type = type;
        *out_len = got;
        return out;
    }
}

/* One convert attempt for `target` against `sel`. Returns the UTF-8
 * text (fdk_alloc) or NULL (refused / timeout / malformed). */
static char *convert_selection(fdk_platform_connection *conn, sel_desc sel,
                                Atom target) {
    Atom type = None;
    size_t len = 0;
    unsigned char *raw = convert_selection_bytes(conn, sel, target, &type,
                                                 &len);
    if (raw == NULL) {
        return NULL;
    }
    char *out = utf8_from_property(type, raw, len);
    fdk_free(raw);
    return out;
}

/* Shared reader: fast path (we own it — the server never round-trips
 * a selection to its own owner, which is also what makes the
 * no-other-client case work under bare Xvfb), the empty case, then
 * convert UTF-8 first with a Latin-1 STRING fallback for ancient
 * owners. Anything else (COMPOUND_TEXT owners) is refused by the
 * owner itself and reads as NULL. */
static char *selection_get_text(fdk_platform_connection *conn,
                                sel_desc sel) {
    if (XGetSelectionOwner(conn->display, sel.atom) ==
        conn->clip_helper) {
        const char *owned = *sel.owned;
        if (owned == NULL || owned[0] == '\0') {
            return NULL;
        }
        size_t len = strlen(owned);
        char *copy = fdk_alloc(len + 1);
        if (copy != NULL) {
            memcpy(copy, owned, len + 1);
        }
        return copy;
    }
    if (XGetSelectionOwner(conn->display, sel.atom) == None) {
        return NULL; /* nobody owns it: it is empty */
    }

    char *text = convert_selection(conn, sel, conn->utf8_string);
    if (text == NULL) {
        text = convert_selection(conn, sel, XA_STRING);
    }
    return text;
}

char *fdk_x11_clipboard_get_text(fdk_platform_connection *conn) {
    return selection_get_text(conn, sel_of_clipboard(conn));
}

char *fdk_x11_clipboard_get_primary_text(fdk_platform_connection *conn) {
    return selection_get_text(conn, sel_of_primary(conn));
}

/* ---- clipboard images (1.4.9) ---- */

fdk_result fdk_x11_clipboard_set_image(fdk_platform_connection *conn,
                                       const unsigned char *png,
                                       size_t len) {
    if (png == NULL || len == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (len > FDK_CLIP_INCR_MAX_BYTES) {
        /* 1.4.12: the atomic cap lifted — payloads up to the hard
         * 64 MiB bound are owned and streamed incrementally past the
         * atomic threshold. Beyond THAT is not a clipboard. */
        FDK_WARN("clipboard: image payload %zu bytes exceeds the "
                 "incremental bound (%u); refusing",
                 len, FDK_CLIP_INCR_MAX_BYTES);
        return FDK_ERR_UNSUPPORTED;
    }
    unsigned char *copy = fdk_alloc(len);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, png, len);
    /* Abort an in-flight serve of the image being replaced. */
    if (conn->clip_incr_tx.active && !conn->clip_incr_tx.owns_bytes &&
        conn->clip_incr_tx.bytes == conn->clip_owned_png) {
        incr_tx_end(conn, true);
    }
    XSetSelectionOwner(conn->display, conn->atom_clipboard,
                       conn->clip_helper, CurrentTime);
    if (XGetSelectionOwner(conn->display, conn->atom_clipboard) !=
        conn->clip_helper) {
        fdk_free(copy);
        FDK_WARN("clipboard: image ownership did not take effect");
        return FDK_ERR_PLATFORM;
    }
    /* One content at a time: the image replaces the owned text. */
    fdk_free(conn->clip_owned_text);
    conn->clip_owned_text = NULL;
    fdk_free(conn->clip_owned_png);
    conn->clip_owned_png = copy;
    conn->clip_owned_png_len = len;
    XFlush(conn->display);
    return FDK_OK;
}

/* Raw bytes from a property read: retired in 1.4.12 — the shared
 * convert_selection_bytes (atomic, multi-part atomic, and INCR)
 * replaced it; image reads are just typed raw reads now. */

unsigned char *fdk_x11_clipboard_get_image(fdk_platform_connection *conn,
                                           size_t *out_len) {
    if (out_len == NULL) {
        return NULL;
    }
    *out_len = 0;
    /* Fast path: we own it. */
    if (XGetSelectionOwner(conn->display, conn->atom_clipboard) ==
        conn->clip_helper) {
        if (conn->clip_owned_png == NULL) {
            return NULL;
        }
        unsigned char *copy = fdk_alloc(conn->clip_owned_png_len);
        if (copy == NULL) {
            return NULL;
        }
        memcpy(copy, conn->clip_owned_png, conn->clip_owned_png_len);
        *out_len = conn->clip_owned_png_len;
        return copy;
    }
    if (XGetSelectionOwner(conn->display, conn->atom_clipboard) == None) {
        return NULL; /* nobody owns it: empty */
    }
    /* Convert image/png from the current owner (bounded wait; the
     * shared raw reader handles the atomic case AND, since 1.4.12,
     * an INCR offer from owners past the atomic cap). */
    Atom type = None;
    unsigned char *raw = convert_selection_bytes(
        conn, sel_of_clipboard(conn), conn->atom_image_png, &type,
        out_len);
    if (raw == NULL) {
        return NULL;
    }
    if (type != conn->atom_image_png) {
        /* The owner answered with a different type than it was asked
         * for — not a PNG seam; refuse rather than feed garbage to
         * the decoder. */
        fdk_free(raw);
        return NULL;
    }
    return raw;
}
