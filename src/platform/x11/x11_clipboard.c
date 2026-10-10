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
 * Not supported, deliberately (see fdk_clipboard.h): INCR
 * incremental transfers (refused with a warning — local transfers
 * are atomic and always fit), COMPOUND_TEXT (we serve
 * UTF8_STRING/TEXT/STRING, which every modern client accepts).
 *
 * 1.4.9 adds the IMAGE surface: image/png as a served/read target,
 * carried in ONE atomic property (the documented 4 MiB practical
 * cap — INCR remains the deliberate later). One clipboard content
 * at a time: owning an image clears the owned text and vice versa.
 */

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xatom.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FDK_CLIP_WAIT_MS 250
/* The atomic property cap for image payloads: comfortably inside
 * every real server's max-request size while covering ordinary
 * screenshots and UI renders. Larger sets refuse with
 * FDK_ERR_UNSUPPORTED (the INCR later would lift this). */
#define FDK_CLIP_IMAGE_MAX_BYTES (4u * 1024u * 1024u)

/* ---- the selection descriptor (one machinery, two selections) ---- */

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

static void serve_text(fdk_platform_connection *conn, sel_desc sel,
                       Window requestor, Atom property, Atom target) {
    const char *owned = *sel.owned;
    if (owned == NULL) {
        return; /* property left unset -> refusal */
    }
    if (target == conn->utf8_string || target == conn->atom_text_plain) {
        XChangeProperty(conn->display, requestor, property, target, 8,
                        PropModeReplace,
                        (const unsigned char *)owned,
                        (int)strlen(owned));
    } else { /* XA_STRING or TEXT: Latin-1 */
        size_t len = 0;
        char *latin = latin1_from_utf8(owned, &len);
        if (latin == NULL) {
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
            /* 1.4.9: the image target, served atomically. */
            XChangeProperty(conn->display, req->requestor, property,
                            conn->atom_image_png, 8, PropModeReplace,
                            conn->clip_owned_png,
                            (int)conn->clip_owned_png_len);
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
                fdk_free(conn->clip_owned_text);
                conn->clip_owned_text = NULL;
                fdk_free(conn->clip_owned_png);
                conn->clip_owned_png = NULL;
                conn->clip_owned_png_len = 0;
            }
        } else if (xevent->xselectionclear.selection == XA_PRIMARY) {
            if (XGetSelectionOwner(conn->display, XA_PRIMARY) !=
                conn->clip_helper) {
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

/* Waits up to FDK_CLIP_WAIT_MS for the SelectionNotify answering our
 * convert of `sel`. Non-matching events are left in Xlib's queue
 * untouched (XCheckTypedWindowEvent removes only the match), so no
 * event is ever dispatched re-entrantly or lost. Returns 1 with
 * *out_notify filled on success, 0 on timeout. */
static int wait_selection_notify(fdk_platform_connection *conn, sel_desc sel,
                                 XEvent *out_notify) {
    uint64_t deadline = now_ms() + FDK_CLIP_WAIT_MS;
    XFlush(conn->display);
    for (;;) {
        XEvent ev;
        if (XCheckTypedWindowEvent(conn->display, conn->clip_helper,
                                   SelectionNotify, &ev)) {
            if (ev.xselection.selection == sel.atom) {
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

/* One convert attempt for `target` against `sel`. Returns the UTF-8
 * text (fdk_alloc) or NULL (refused / timeout / oversized). */
static char *convert_selection(fdk_platform_connection *conn, sel_desc sel,
                                Atom target) {
    XConvertSelection(conn->display, sel.atom, target,
                      conn->atom_fdk_selection, conn->clip_helper,
                      CurrentTime);
    XEvent notify;
    if (!wait_selection_notify(conn, sel, &notify)) {
        FDK_WARN("%s: owner did not answer within %d ms", sel.name,
                 FDK_CLIP_WAIT_MS);
        return NULL;
    }
    if (notify.xselection.property == None) {
        return NULL; /* owner refused this target */
    }
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(conn->display, conn->clip_helper,
                           conn->atom_fdk_selection, 0, 0x100000, True,
                           AnyPropertyType, &type, &format, &nitems,
                           &bytes_after, &data) != Success) {
        return NULL;
    }
    if (type == conn->atom_incr) {
        /* Incremental transfer: v1 refuses rather than half-receive.
         * (XGetWindowProperty with delete=True already consumed the
         * INCR property and its 0-length read is what the protocol
         * expects for a refusal.) */
        FDK_WARN("%s: INCR transfer offered — refusing "
                 "(oversized selection, not supported in v1)", sel.name);
        if (data != NULL) {
            XFree(data);
        }
        return NULL;
    }
    if (data == NULL || type == None || format != 8) {
        if (data != NULL) {
            XFree(data);
        }
        return NULL;
    }
    char *out = utf8_from_property(type, data, nitems);
    XFree(data);
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
    if (len > FDK_CLIP_IMAGE_MAX_BYTES) {
        FDK_WARN("clipboard: image payload %zu bytes exceeds the "
                 "atomic cap (%u); refusing (INCR is a later)",
                 len, FDK_CLIP_IMAGE_MAX_BYTES);
        return FDK_ERR_UNSUPPORTED;
    }
    unsigned char *copy = fdk_alloc(len);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, png, len);
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

/* Raw bytes from a property read: the atomic image read. */
static unsigned char *bytes_from_property(fdk_platform_connection *conn,
                                          size_t *out_len) {
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(conn->display, conn->clip_helper,
                           conn->atom_fdk_selection, 0, 0x400000, True,
                           AnyPropertyType, &type, &format, &nitems,
                           &bytes_after, &data) != Success) {
        return NULL;
    }
    if (type == conn->atom_incr) {
        FDK_WARN("clipboard: INCR image transfer offered — refusing "
                 "(oversized selection, not supported yet)");
        if (data != NULL) {
            XFree(data);
        }
        return NULL;
    }
    if (data == NULL || type == None || format != 8 || nitems == 0) {
        if (data != NULL) {
            XFree(data);
        }
        return NULL;
    }
    unsigned char *out = fdk_alloc(nitems);
    if (out == NULL) {
        XFree(data);
        return NULL;
    }
    memcpy(out, data, nitems);
    *out_len = (size_t)nitems;
    XFree(data);
    return out;
}

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
    /* Convert image/png from the current owner (bounded wait). */
    XConvertSelection(conn->display, conn->atom_clipboard,
                      conn->atom_image_png, conn->atom_fdk_selection,
                      conn->clip_helper, CurrentTime);
    XEvent notify;
    if (!wait_selection_notify(conn, sel_of_clipboard(conn), &notify)) {
        FDK_WARN("clipboard: image owner did not answer within %d ms",
                 FDK_CLIP_WAIT_MS);
        return NULL;
    }
    if (notify.xselection.property == None) {
        return NULL; /* owner has no image */
    }
    return bytes_from_property(conn, out_len);
}
