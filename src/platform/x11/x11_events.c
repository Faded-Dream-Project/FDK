#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include <string.h>
#include <time.h> /* X3's in-bounds click stamp */

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/keysym.h>
#include <X11/Xutil.h>

/* Translates an XKeyEvent's state field (the modifier mask X reports
 * on THIS event, not a separate query) into fdk_key_modifier bits.
 * Shared with the DnD layer (XQueryPointer's state field uses the
 * same mask encoding) — hence non-static (1.3.0). */
fdk_u32 fdk__x11_translate_modifiers(unsigned int x_state) {
    fdk_u32 mods = 0;
    if (x_state & ShiftMask)   mods |= FDK_MOD_SHIFT;
    if (x_state & ControlMask) mods |= FDK_MOD_CTRL;
    if (x_state & Mod1Mask)    mods |= FDK_MOD_ALT;   /* Mod1 is conventionally Alt */
    if (x_state & Mod4Mask)    mods |= FDK_MOD_SUPER; /* Mod4 is conventionally Super */
    return mods;
}

/* X11 keycodes are (per the X protocol) evdev keycode + 8 on every
 * Linux X server in practice, which is what lets us report the same
 * evdev-based fdk_scancode space fdk_event.h documents without a
 * dependency on XKB-specific keycode queries. This offset is a
 * long-standing, effectively-universal convention on Linux (not
 * guaranteed by the X11 spec itself, which treats keycodes as
 * opaque) — documented here rather than left as an unexplained "-8". */
static fdk_scancode x11_keycode_to_scancode(unsigned int keycode) {
    if (keycode < 8) {
        return 0;
    }
    return (fdk_scancode)(keycode - 8);
}

/* Resolves the Unicode codepoint an XKeyEvent produces given its
 * current modifier state. With a per-window input context (1.3.1)
 * this is Xutf8LookupString — layout-aware, full Unicode (the user's
 * ACTUAL keyboard, é/ä/中 included); without one it falls back to
 * XLookupString, which resolves ASCII reliably and approximates the
 * rest of Latin-1. Returns 0 for keys with no textual result
 * (arrows, F-keys, bare modifiers).
 *
 * M-1 (IME): `commit_buf`/`commit_cap` carry the event's FULL text out
 * (NUL-terminated, `*commit_len` its byte length) — the multi-
 * codepoint IME-commit case the single codepoint return cannot
 * represent. An IME commit CAN exceed 15 UTF-8 bytes (a pasted-
 * length phrase commit), so the XIC leg retries XBufferOverflow
 * with a heap buffer sized by the overflow answer; that heap string
 * is returned through `*heap_out` (caller frees) and is the same
 * text the stack copy holds. Pass commit_buf == NULL to skip the
 * side channel. */
static fdk_u32 x11_lookup_codepoint(fdk_platform_window *pwindow,
                                    XKeyEvent *xkey,
                                    char *commit_buf, size_t commit_cap,
                                    int *commit_len, char **heap_out) {
    char buf[16];
    KeySym keysym = NoSymbol;
    int len = 0;
    const char *text = buf; /* what the decode legs read below */
    if (pwindow != NULL && pwindow->xic != NULL) {
        Status status = 0;
        len = Xutf8LookupString(pwindow->xic, xkey, buf,
                                (int)sizeof(buf) - 1, &keysym, &status);
        if (status == XBufferOverflow) {
            /* The commit does not fit 15 bytes — a REAL IME shape
             * (phrase commits). Xlib's contract: the return value is
             * the needed byte count; retry with exactly that. */
            int want = (len > 0) ? len : 256;
            char *grown = fdk_alloc((size_t)want + 1u);
            if (grown != NULL) {
                len = Xutf8LookupString(pwindow->xic, xkey, grown,
                                        want, &keysym, &status);
                if (len > 0) {
                    grown[len] = '\0';
                    text = grown;
                    if (heap_out != NULL) {
                        *heap_out = grown;
                    } else {
                        fdk_free(grown);
                    }
                } else {
                    fdk_free(grown);
                    len = 0;
                }
            } else {
                len = 0;
            }
        }
    } else {
        len = XLookupString(xkey, buf, (int)sizeof(buf) - 1, &keysym,
                            NULL);
    }
    if (len <= 0) {
        return 0;
    }
    /* NUL-terminate the stack path (the heap path did its own). */
    if (text == buf) {
        buf[len] = '\0';
    }

    /* The side channel: the full text copied into the caller's
     * buffer (clipped to its declared capacity — truncation is the
     * honest degradation of a too-small caller buffer); the heap
     * string, when the overflow path allocated one, goes to
     * *heap_out and the caller prefers it. */
    if (commit_buf != NULL && commit_len != NULL) {
        size_t n = (size_t)len;
        if (n + 1u > commit_cap) {
            n = commit_cap - 1u;
        }
        memcpy(commit_buf, text, n);
        commit_buf[n] = '\0';
        *commit_len = (int)n;
    }

    /* Ctrl+letter (Phase 9): the lookup reports the CONTROL
     * character (^X = 0x18) for these, but the codepoint contract is
     * "what the key produces" and every shortcut reader (Entry's
     * Ctrl+X/C/V/A) wants the LETTER — text entry ignores
     * ctrl-combos anyway (FDK's Entry refuses control codepoints as
     * inserts). The Wayland backend already reports the letter via
     * xkb_state_key_get_one_sym; this makes the backends agree. */
    if ((xkey->state & ControlMask) != 0 &&
        keysym >= 0x61 && keysym <= 0x7A) { /* XK_a..XK_z */
        return (fdk_u32)keysym;
    }
    if ((xkey->state & ControlMask) != 0 &&
        keysym >= 0x41 && keysym <= 0x5A) { /* XK_A..XK_Z */
        return (fdk_u32)keysym;
    }

    /* Decode UTF-8 (Xutf8) or Latin-1 (XLookupString fallback) to a
     * codepoint. */
    {
        unsigned char c0 = (unsigned char)text[0];
        if (c0 < 0x80) {
            return (fdk_u32)c0;
        }
        if ((c0 & 0xE0u) == 0xC0u && len >= 2) {
            return (fdk_u32)(((c0 & 0x1Fu) << 6) |
                             ((unsigned char)text[1] & 0x3Fu));
        }
        if ((c0 & 0xF0u) == 0xE0u && len >= 3) {
            return (fdk_u32)(((c0 & 0x0Fu) << 12) |
                             (((unsigned char)text[1] & 0x3Fu) << 6) |
                             ((unsigned char)text[2] & 0x3Fu));
        }
        if ((c0 & 0xF8u) == 0xF0u && len >= 4) {
            return (fdk_u32)(((c0 & 0x07u) << 18) |
                             (((unsigned char)text[1] & 0x3Fu) << 12) |
                             (((unsigned char)text[2] & 0x3Fu) << 6) |
                             ((unsigned char)text[3] & 0x3Fu));
        }
        /* XLookupString's Latin-1 single byte (the no-XIC fallback):
         * the honest best answer that path can give. */
        if (len == 1 && c0 >= 0x80 &&
            (pwindow == NULL || pwindow->xic == NULL)) {
            return (fdk_u32)c0;
        }
    }
    return 0;
}

/* HiDPI (1.4.5): physical window coordinates -> logical tree
 * coordinates. The X window is logical x conn->scale pixels; every
 * coordinate handed to the core (and thus to widget hit-testing)
 * divides back down. Integer division by an integer scale is the
 * exact inverse of the paint path's integer block scaling: a widget
 * at logical x occupies physical [x*s, (x+1)*s), and any physical
 * pixel in that range maps back to x. */
static fdk_f32 x11_to_logical_x(fdk_platform_window *pwindow, int px) {
    int s = pwindow->conn->scale;
    return (s > 1) ? (fdk_f32)(px / s) : (fdk_f32)px;
}
static fdk_f32 x11_to_logical_y(fdk_platform_window *pwindow, int py) {
    return x11_to_logical_x(pwindow, py);
}

/* Fills `out` for events that don't need cross-referencing against
 * connection-level state (WM_DELETE_WINDOW etc — those are handled in
 * x11_dispatch_pending directly). Returns nonzero if `xevent` produced
 * a translatable event, zero if it should be silently ignored (X
 * delivers many event types FDK doesn't currently model). */
int fdk_x11_translate_event(fdk_platform_window *pwindow, XEvent *xevent,
                             fdk_event_data *out) {
    switch (xevent->type) {
        case ConfigureNotify: {
            /* last_size stays PHYSICAL (the framebuffer pair sizes
             * from it); the event the CORE sees is logical (the root
             * widget and the paint intermediate size from that). */
            fdk_size new_size = {
                .width = xevent->xconfigure.width,
                .height = xevent->xconfigure.height,
            };
            if (new_size.width == pwindow->last_size.width &&
                new_size.height == pwindow->last_size.height) {
                return 0; /* no actual change; X sends these liberally */
            }
            pwindow->last_size = new_size;
            out->type = FDK_EVENT_WINDOW_CONFIGURE;
            if (pwindow->conn->scale > 1) {
                out->configure.size.width =
                    new_size.width / pwindow->conn->scale;
                out->configure.size.height =
                    new_size.height / pwindow->conn->scale;
            } else {
                out->configure.size = new_size;
            }
            return 1;
        }

        case Expose:
            /* Reported regions of the window lost their contents (it
             * was covered/uncovered, or is being shown for the first
             * time). fdk_event.h documents the contract: rendered
             * applications re-present; others can ignore. X sends a
             * series of Exposes for one damage event — each becomes
             * its own FDK event; coalescing is left to applications
             * (repainting the whole surface is what FDK's examples
             * do, which makes the series harmless). */
            out->type = FDK_EVENT_WINDOW_EXPOSE;
            out->expose.area.x = xevent->xexpose.x;
            out->expose.area.y = xevent->xexpose.y;
            out->expose.area.width = xevent->xexpose.width;
            out->expose.area.height = xevent->xexpose.height;
            return 1;

        case FocusIn:
        case FocusOut:
            out->type = FDK_EVENT_WINDOW_FOCUS;
            out->focus.focused = (xevent->type == FocusIn);
            /* Lazy IM/XIC (1.1.0-rc1, X1): focus precedes keys in any
             * real text-entry flow — bring the input context up HERE
             * so the first KeyPress already resolves through it. */
            if (xevent->type == FocusIn && pwindow != NULL) {
                fdk__x11_window_ensure_xic(pwindow);
            }
            /* Keep the input context's focus in step (XIM discipline):
             * composition state resets on blur exactly like the
             * widget layer's FOCUS_OUT semantics. */
            if (pwindow != NULL && pwindow->xic != NULL) {
                if (xevent->type == FocusIn) {
                    XSetICFocus(pwindow->xic);
                } else {
                    XUnsetICFocus(pwindow->xic);
                }
            }
            return 1;

        case KeyPress:
        case KeyRelease: {
            /* Lazy IM/XIC (1.1.0-rc1, X1): a key arriving without any
             * prior FocusIn (bare X, no WM) still gets a context —
             * the XIC resolves this event too, since the event struct
             * is already in hand when the context is born. */
            if (xevent->type == KeyPress && pwindow != NULL) {
                fdk__x11_window_ensure_xic(pwindow);
            }
            out->type = (xevent->type == KeyPress) ? FDK_EVENT_KEY_DOWN : FDK_EVENT_KEY_UP;
            out->key.scancode = x11_keycode_to_scancode(xevent->xkey.keycode);
            out->key.modifiers = fdk__x11_translate_modifiers(xevent->xkey.state);
            /* Popup dismissal reason (1.1.0-rc1): the window layer
             * consumes Escape for popups (its scancode check lives in
             * window.c, backend-neutral); stamping the reason HERE —
             * keycode 9 is the universal ESC keycode on Linux X
             * servers (evdev scancode 1 + the documented +8 offset) —
             * lets FDK_POPUP_TRACE attribute the release to "escape"
             * instead of a bare "destroy". */
            if (pwindow->popup && xevent->type == KeyPress &&
                xevent->xkey.keycode == 9) {
                fdk_x11_window_popup_set_release_reason(pwindow, "escape");
            }
            /* Repeat detection (1.3.0): with XKB detectable
             * auto-repeat (requested at connect), a KeyPress of an
             * already-down keycode IS the repeat — track the 256-bit
             * down bitmap. v1 hardcoded 0 while a comment claimed
             * the enabling call existed somewhere else. */
            {
                unsigned int kc = xevent->xkey.keycode;
                unsigned int byte = kc >> 3;
                unsigned int bit = kc & 7;
                if (byte < 32) {
                    if (xevent->type == KeyPress) {
                        out->key.is_repeat =
                            (pwindow->conn->key_down[byte] &
                             (unsigned char)(1u << bit)) != 0 ? 1 : 0;
                        pwindow->conn->key_down[byte] |=
                            (unsigned char)(1u << bit);
                    } else {
                        pwindow->conn->key_down[byte] &=
                            (unsigned char)~(1u << bit);
                        out->key.is_repeat = 0;
                    }
                } else {
                    out->key.is_repeat = 0;
                }
            }
            out->key.codepoint = 0; /* KeyRelease codepoint lookup is
                                     * meaningless */
            if (xevent->type == KeyPress) {
                /* M-1 (IME): the full-text side channel — a commit of
                 * MORE than one codepoint is the IME case (the CJK
                 * phrase an input method exists for): it dispatches
                 * FDK_EVENT_IME_COMMIT directly and the key event
                 * itself is swallowed (the IM already spoke; a
                 * KEY_DOWN with a truncated codepoint would be a
                 * lie). One codepoint (the compose-key/dead-key
                 * shape) stays the ordinary KEY_DOWN codepoint — the
                 * documented compatibility contract. */
                char commit[256];
                int commit_len = 0;
                char *heap = NULL;
                out->key.codepoint = x11_lookup_codepoint(
                    pwindow, &xevent->xkey, commit,
                    sizeof(commit), &commit_len, &heap);
                const char *commit_text = (heap != NULL) ? heap : commit;
                if (commit_len > 0) {
                    /* Count codepoints: lead (non-continuation)
                     * bytes. */
                    int cps = 0;
                    for (const unsigned char *p =
                             (const unsigned char *)commit_text;
                         *p != 0; p++) {
                        if ((*p & 0xC0u) != 0x80u) {
                            cps++;
                        }
                    }
                    if (cps > 1) {
                        fdk_x11_ime_dispatch(pwindow,
                                             FDK_EVENT_IME_COMMIT,
                                             commit_text, 0);
                        fdk_free(heap);
                        return 0; /* swallowed: IME_COMMIT replaces
                                   * the key event entirely */
                    }
                }
                fdk_free(heap);
            }
            return 1;
        }

        case MotionNotify:
            out->type = FDK_EVENT_POINTER_MOTION;
            out->pointer.position.x =
                x11_to_logical_x(pwindow, xevent->xmotion.x);
            out->pointer.position.y =
                x11_to_logical_y(pwindow, xevent->xmotion.y);
            return 1;

        case ButtonPress:
        case ButtonRelease: {
            /* Buttons 4/5 are the traditional X11 scroll-wheel
             * encoding (no separate scroll event type in core X11);
             * we translate those into FDK_EVENT_POINTER_SCROLL instead
             * of reporting them as buttons 4/5, since that's what
             * fdk_event.h's scroll event contract expects. */
            if (xevent->xbutton.button == Button4 || xevent->xbutton.button == Button5) {
                if (xevent->type != ButtonPress) {
                    return 0; /* only translate the press half */
                }
                out->type = FDK_EVENT_POINTER_SCROLL;
                out->scroll.position.x =
                    x11_to_logical_x(pwindow, xevent->xbutton.x);
                out->scroll.position.y =
                    x11_to_logical_y(pwindow, xevent->xbutton.y);
                out->scroll.delta_x = 0.0f;
                out->scroll.delta_y = (xevent->xbutton.button == Button4) ? 1.0f : -1.0f;
                return 1;
            }

            /* Popup dismissal (Phase 9): while a popup holds the
             * grab, a press OUTSIDE its bounds (negative or past-edge
             * coordinates — owner_events False reports everything
             * against the grab window) is the universal "click away"
             * gesture: it becomes a close request, never a button
             * event inside the tree.
             *
             * The release reason rides along (1.1.0-rc1): the
             * dismissal path through window.c destroys the popup, and
             * FDK_POPUP_TRACE's ungrab line then says WHY. */
            if (pwindow->popup && xevent->type == ButtonPress &&
                (xevent->xbutton.x < 0 ||
                 xevent->xbutton.y < 0 ||
                 xevent->xbutton.x >= (int)pwindow->last_size.width ||
                 xevent->xbutton.y >= (int)pwindow->last_size.height)) {
                fdk_x11_window_popup_set_release_reason(pwindow,
                                                        "outside-click");
                /* X3: the close-request this generates carries its
                 * reason — the single-slot channel window.c's
                 * dispatcher consumes when the event lands on the
                 * popup (was "(unknown)" before the generator noted
                 * it). */
                fdk__window_note_popup_close(FDK_POPUP_CLOSE_OUTSIDE_CLICK);
                out->type = FDK_EVENT_WINDOW_CLOSE_REQUEST;
                return 1;
            }
            out->type = (xevent->type == ButtonPress)
                ? FDK_EVENT_POINTER_BUTTON_DOWN
                : FDK_EVENT_POINTER_BUTTON_UP;
            out->pointer_button.position.x =
                x11_to_logical_x(pwindow, xevent->xbutton.x);
            out->pointer_button.position.y =
                x11_to_logical_y(pwindow, xevent->xbutton.y);
            out->pointer_button.button = xevent->xbutton.button;
            out->pointer_button.modifiers =
                fdk__x11_translate_modifiers(xevent->xbutton.state);
            if (xevent->type == ButtonPress && pwindow->popup) {
                /* X3: remember when a click LANDED INSIDE the popup —
                 * an application destroy that follows within a second
                 * is almost always an item selection. */
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                pwindow->popup_last_button_ms =
                    (long long)ts.tv_sec * 1000 +
                    (long long)ts.tv_nsec / 1000000;
            }
            return 1;
        }

        case EnterNotify:
            out->type = FDK_EVENT_POINTER_ENTER;
            out->pointer.position.x =
                x11_to_logical_x(pwindow, xevent->xcrossing.x);
            out->pointer.position.y =
                x11_to_logical_y(pwindow, xevent->xcrossing.y);
            return 1;

        case LeaveNotify:
            out->type = FDK_EVENT_POINTER_LEAVE;
            out->pointer.position.x =
                x11_to_logical_x(pwindow, xevent->xcrossing.x);
            out->pointer.position.y =
                x11_to_logical_y(pwindow, xevent->xcrossing.y);
            return 1;

        case PropertyNotify: {
            /* How a real WM talks window-state back: it rewrites the
             * _NET_WM_STATE (maximized) / WM_STATE (iconic) properties
             * ON OUR WINDOW; each rewrite is a PropertyNotify. The
             * state flip + event dispatch live in the shared compare-
             * and-flip helper, so a property touch that changes
             * nothing dispatches nothing. */
            Atom atom = xevent->xproperty.atom;
            if (atom == pwindow->conn->net_wm_state) {
                fdk_x11_window_update_state(
                    pwindow,
                    fdk_x11_window_net_state_maximized(pwindow),
                    pwindow->minimized,
                    fdk__x11_net_state_fullscreen(pwindow));
                return 0; /* the FDK state event (if any) was already
                             dispatched by the helper */
            }
            if (atom == pwindow->conn->wm_state) {
                int iconic = fdk_x11_window_wm_state_iconic(pwindow);
                if (iconic >= 0) {
                    fdk_x11_window_update_state(pwindow,
                                                pwindow->maximized,
                                                iconic,
                                                pwindow->fullscreen);
                }
                return 0;
            }
            return 0; /* some other property we don't model */
        }

        default:
            return 0;
    }
}
