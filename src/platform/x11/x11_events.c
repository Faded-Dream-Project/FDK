#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/log_internal.h"

#include <X11/keysym.h>
#include <X11/Xutil.h>

/* Translates an XKeyEvent's state field (the modifier mask X reports
 * on THIS event, not a separate query) into fdk_key_modifier bits. */
static fdk_u32 translate_modifiers(unsigned int x_state) {
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
 * current modifier state, using Xlib's own layout-aware lookup
 * (XLookupString) rather than a hand-rolled keysym table — this
 * respects the user's actual configured keyboard layout. Returns 0
 * for keys with no textual result (arrows, F-keys, bare modifiers). */
static fdk_u32 x11_lookup_codepoint(XKeyEvent *xkey) {
    char buf[8];
    KeySym keysym = NoSymbol;
    int len = XLookupString(xkey, buf, (int)sizeof(buf) - 1, &keysym, NULL);
    if (len <= 0) {
        return 0;
    }
    buf[len] = '\0';

    /* XLookupString gives us Latin-1/local-encoding bytes, not
     * necessarily UTF-8 codepoints, for non-ASCII input; a fully
     * correct general Unicode result requires XmbLookupString with a
     * per-window XIC (input context) and X Input Method setup. That
     * is deliberately out of scope for Phase 2 — see
     * docs/platform-input.md's documented limitation — and ASCII
     * (which covers the common case and all of FDK's own tests) is
     * unaffected by the gap. */
    unsigned char c = (unsigned char)buf[0];
    if (c < 0x80) {
        return (fdk_u32)c;
    }
    return 0;
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
            out->configure.size = new_size;
            return 1;
        }

        case FocusIn:
            out->type = FDK_EVENT_WINDOW_FOCUS;
            out->focus.focused = 1;
            return 1;

        case FocusOut:
            out->type = FDK_EVENT_WINDOW_FOCUS;
            out->focus.focused = 0;
            return 1;

        case KeyPress:
        case KeyRelease: {
            out->type = (xevent->type == KeyPress) ? FDK_EVENT_KEY_DOWN : FDK_EVENT_KEY_UP;
            out->key.scancode = x11_keycode_to_scancode(xevent->xkey.keycode);
            out->key.modifiers = translate_modifiers(xevent->xkey.state);
            out->key.is_repeat = 0; /* X11 auto-repeat detection needs
                                        XkbSetDetectableAutoRepeat, set
                                        once at connection time — see
                                        x11_connection.c */
            out->key.codepoint = (xevent->type == KeyPress)
                ? x11_lookup_codepoint(&xevent->xkey)
                : 0; /* KeyRelease codepoint lookup is meaningless */
            return 1;
        }

        case MotionNotify:
            out->type = FDK_EVENT_POINTER_MOTION;
            out->pointer.position.x = (fdk_f32)xevent->xmotion.x;
            out->pointer.position.y = (fdk_f32)xevent->xmotion.y;
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
                out->scroll.position.x = (fdk_f32)xevent->xbutton.x;
                out->scroll.position.y = (fdk_f32)xevent->xbutton.y;
                out->scroll.delta_x = 0.0f;
                out->scroll.delta_y = (xevent->xbutton.button == Button4) ? 1.0f : -1.0f;
                return 1;
            }

            out->type = (xevent->type == ButtonPress)
                ? FDK_EVENT_POINTER_BUTTON_DOWN
                : FDK_EVENT_POINTER_BUTTON_UP;
            out->pointer_button.position.x = (fdk_f32)xevent->xbutton.x;
            out->pointer_button.position.y = (fdk_f32)xevent->xbutton.y;
            out->pointer_button.button = xevent->xbutton.button;
            return 1;
        }

        case EnterNotify:
            out->type = FDK_EVENT_POINTER_ENTER;
            return 1;

        case LeaveNotify:
            out->type = FDK_EVENT_POINTER_LEAVE;
            return 1;

        default:
            return 0;
    }
}
