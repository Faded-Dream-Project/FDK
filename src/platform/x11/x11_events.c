#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

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
 * (arrows, F-keys, bare modifiers). */
static fdk_u32 x11_lookup_codepoint(fdk_platform_window *pwindow,
                                    XKeyEvent *xkey) {
    char buf[16];
    KeySym keysym = NoSymbol;
    int len = 0;
    if (pwindow != NULL && pwindow->xic != NULL) {
        Status status = 0;
        len = Xutf8LookupString(pwindow->xic, xkey, buf,
                                (int)sizeof(buf) - 1, &keysym, &status);
        if (status == XBufferOverflow) {
            len = 0; /* a single key event cannot overflow 15 UTF-8
                      * bytes honestly; treat as no text */
        }
    } else {
        len = XLookupString(xkey, buf, (int)sizeof(buf) - 1, &keysym,
                            NULL);
    }
    if (len <= 0) {
        return 0;
    }
    buf[len] = '\0';

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
        unsigned char c0 = (unsigned char)buf[0];
        if (c0 < 0x80) {
            return (fdk_u32)c0;
        }
        if ((c0 & 0xE0u) == 0xC0u && len >= 2) {
            return (fdk_u32)(((c0 & 0x1Fu) << 6) |
                             ((unsigned char)buf[1] & 0x3Fu));
        }
        if ((c0 & 0xF0u) == 0xE0u && len >= 3) {
            return (fdk_u32)(((c0 & 0x0Fu) << 12) |
                             (((unsigned char)buf[1] & 0x3Fu) << 6) |
                             ((unsigned char)buf[2] & 0x3Fu));
        }
        if ((c0 & 0xF8u) == 0xF0u && len >= 4) {
            return (fdk_u32)(((c0 & 0x07u) << 18) |
                             (((unsigned char)buf[1] & 0x3Fu) << 12) |
                             (((unsigned char)buf[2] & 0x3Fu) << 6) |
                             ((unsigned char)buf[3] & 0x3Fu));
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
            out->type = (xevent->type == KeyPress) ? FDK_EVENT_KEY_DOWN : FDK_EVENT_KEY_UP;
            out->key.scancode = x11_keycode_to_scancode(xevent->xkey.keycode);
            out->key.modifiers = fdk__x11_translate_modifiers(xevent->xkey.state);
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
            out->key.codepoint = (xevent->type == KeyPress)
                ? x11_lookup_codepoint(pwindow, &xevent->xkey)
                : 0; /* KeyRelease codepoint lookup is meaningless */
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
             * event inside the tree. */
            if (pwindow->popup && xevent->type == ButtonPress &&
                (xevent->xbutton.x < 0 ||
                 xevent->xbutton.y < 0 ||
                 xevent->xbutton.x >= (int)pwindow->last_size.width ||
                 xevent->xbutton.y >= (int)pwindow->last_size.height)) {
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
                    pwindow->minimized);
                return 0; /* the FDK state event (if any) was already
                             dispatched by the helper */
            }
            if (atom == pwindow->conn->wm_state) {
                int iconic = fdk_x11_window_wm_state_iconic(pwindow);
                if (iconic >= 0) {
                    fdk_x11_window_update_state(pwindow,
                                                pwindow->maximized,
                                                iconic);
                }
                return 0;
            }
            return 0; /* some other property we don't model */
        }

        default:
            return 0;
    }
}
