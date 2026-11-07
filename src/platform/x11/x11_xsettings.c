/*
 * x11_xsettings.c — live XSETTINGS reads for the icon-theme engine
 * (internal, X11 backend)
 *
 * XSETTINGS is the settings channel GTK3/Qt applications actually
 * consume on X11 (and XLibre — same protocol, same wire format): the
 * desktop's settings daemon (xfsettingsd on XFCE, gnome-settings-
 * daemon under GNOME/XWayland, mate-settings-daemon, Cinnamon's,
 * xsettingsd) owns the _XSETTINGS_S<n> selection on each screen and
 * publishes a _XSETTINGS_SETTINGS property on ITS OWN window. Reading
 * it needs nothing but Xlib — no D-Bus, no daemon libraries — which
 * keeps the toolkit's no-bus policy intact while finally hearing the
 * desktop's LIVE icon-theme statement instead of guessing from files.
 *
 * THE WIRE FORMAT was decoded byte-for-byte from a REAL xsettingsd
 * 1.0.2 (Debian package) running under Xvfb, cross-checked against
 * its own reference reader (dump_xsettings) — not from memory of the
 * spec. Layout, for a property of N bytes:
 *
 *   header (12 bytes):
 *     [0]      byte order: 0 = LSB-first, 1 = MSB-first
 *     [1..3]   unused
 *     [4..7]   card32 serial
 *     [8..11]  card32 number of settings that follow
 *   then that many settings, each starting 4-byte aligned:
 *     [0]      type: 0 = integer, 1 = string, 2 = color
 *     [1]      unused
 *     [2..3]   card16 name length
 *     [4..]    name (that many bytes, latin-1)
 *     ..       pad to a 4-byte boundary
 *     [..]     card32 last-change serial
 *     value:   string -> card32 length, bytes, pad to 4
 *              integer -> card32
 *              color   -> 4 x card16
 *
 * Reading discipline (an XSETTINGS property is daemon-authored data
 * the toolkit did not write — same trust level as a theme file):
 *   - every offset is bounds-checked BEFORE it is dereferenced; a
 *     truncated or lying count simply ends the walk (the honest
 *     miss — detection falls through to the file channels);
 *   - the property read is bounded (64 KiB): real XSETTINGS payloads
 *     are hundreds of bytes; anything larger is hostile and refused;
 *   - the reader opens its OWN short-lived Display (XOpenDisplay/
 *     XCloseDisplay around the read): no lifetime entanglement with
 *     the context's connection, correct even before fdk_init or in
 *     a purely offscreen process, and one round-trip only — the
 *     result is cached by the caller (the icon engine's g_current)
 *     so this runs once per detection cycle, not per lookup.
 *
 * Not part of the public API — never installed.
 */

#define FDK_LOG_TAG "x11"

#include "platform/platform_internal.h"
#include "platform/x11/x11_platform.h" /* the connection struct for
                                          * the live-follow arm      */

#include "core/log_internal.h"
#include "theme/theme_internal.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* now_ms for the re-arm throttle (mirrors fdk__x11_now_ms in
 * x11_dispatch.c — kept local so this file has no dispatch deps). */
static uint64_t xs_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The property is daemon data; 64 KiB is a hostile-input bound (real
 * payloads are ~100-1000 bytes for a full GTK settings set). */
#define FDK_XSETTINGS_MAX_BYTES (64u * 1024u)

/* Reading is bounded by the header's count AND the byte budget; a
 * name longer than this cannot be a setting we ask about. */
#define FDK_XSETTINGS_NAME_MAX 128

static unsigned long xs_read_card32(const unsigned char *d, bool be) {
    if (be) {
        return ((unsigned long)d[0] << 24) | ((unsigned long)d[1] << 16) |
               ((unsigned long)d[2] << 8) | (unsigned long)d[3];
    }
    return (unsigned long)d[0] | ((unsigned long)d[1] << 8) |
           ((unsigned long)d[2] << 16) | ((unsigned long)d[3] << 24);
}

static unsigned long xs_read_card16(const unsigned char *d, bool be) {
    if (be) {
        return ((unsigned long)d[0] << 8) | (unsigned long)d[1];
    }
    return (unsigned long)d[0] | ((unsigned long)d[1] << 8);
}

/* Rounds an offset up to the next 4-byte boundary. */
static size_t xs_pad4(size_t n) {
    return (n + 3u) & ~(size_t)3u;
}

bool fdk__x11_xsettings_string(const char *setting_name, char *out,
                               size_t cap) {
    if (setting_name == NULL || out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';

    size_t want_len = strlen(setting_name);
    if (want_len == 0 || want_len > FDK_XSETTINGS_NAME_MAX) {
        return false;
    }

    Display *d = XOpenDisplay(NULL);
    if (d == NULL) {
        return false; /* no X11 session — files carry the answer */
    }
    bool found = false;
    unsigned char *data = NULL;

    do {
        /* The manager owns _XSETTINGS_S<screen>; the property lives
         * on ITS window (verified against real xsettingsd — a root-
         * window read finds nothing). Screen 0 is the convention
         * every daemon serves; multi-screen desktops mirror their
         * settings anyway. */
        char sel_name[32];
        int scr = DefaultScreen(d);
        snprintf(sel_name, sizeof(sel_name), "_XSETTINGS_S%d", scr);
        Atom sel = XInternAtom(d, sel_name, False);
        Window owner = XGetSelectionOwner(d, sel);
        if (owner == None) {
            break; /* no settings daemon running — the honest miss */
        }

        Atom prop = XInternAtom(d, "_XSETTINGS_SETTINGS", False);
        Atom type = None;
        int fmt = 0;
        unsigned long nitems = 0, bytes_after = 0;
        int rc = XGetWindowProperty(d, owner, prop, 0,
                                    (long)(FDK_XSETTINGS_MAX_BYTES / 4),
                                    False, AnyPropertyType, &type, &fmt,
                                    &nitems, &bytes_after, &data);
        if (rc != Success || data == NULL || fmt != 8 || nitems == 0 ||
            nitems > FDK_XSETTINGS_MAX_BYTES ||
            bytes_after != 0) {
            break; /* absent, oversized, or truncated — not trusted */
        }
        size_t len = (size_t)nitems;
        if (len < 12) {
            break;
        }

        bool be = data[0] == 1; /* 0 = LSB first, 1 = MSB first */
        unsigned long count = xs_read_card32(data + 8, be);
        size_t off = 12;

        for (unsigned long i = 0; i < count && !found; i++) {
            if (off + 4 > len) {
                break;
            }
            unsigned char type_byte = data[off];
            size_t name_len =
                (size_t)xs_read_card16(data + off + 2, be);
            size_t name_off = off + 4;
            if (name_len > FDK_XSETTINGS_NAME_MAX ||
                name_off + name_len > len) {
                break;
            }
            size_t after_name = xs_pad4(name_off + name_len);
            if (after_name + 4 > len) {
                break; /* no room for the setting's serial + value */
            }
            size_t value_off = after_name + 4;

            if (type_byte == 1 && name_len == want_len &&
                memcmp(data + name_off, setting_name, want_len) == 0) {
                /* The setting we asked for: string value. */
                if (value_off + 4 > len) {
                    break;
                }
                unsigned long vlen =
                    xs_read_card32(data + value_off, be);
                size_t voff = value_off + 4;
                if (vlen >= cap || vlen > len ||
                    voff + vlen > len) {
                    break; /* lying length or won't fit — refuse */
                }
                memcpy(out, data + voff, vlen);
                out[vlen] = '\0';
                found = true;
                break;
            }

            /* Skip a setting we do not care about, by type. */
            size_t setting_size;
            if (type_byte == 1) {
                if (value_off + 4 > len) {
                    break;
                }
                unsigned long vlen =
                    xs_read_card32(data + value_off, be);
                if (vlen > len) {
                    break;
                }
                setting_size =
                    xs_pad4(value_off + 4 + (size_t)vlen) - off;
            } else if (type_byte == 0) {
                setting_size = (value_off + 4) - off;
            } else if (type_byte == 2) {
                setting_size = (value_off + 8) - off;
            } else {
                break; /* unknown type: the walk cannot continue
                          safely — treat as unreadable */
            }
            if (setting_size == 0 || off + setting_size > len) {
                break;
            }
            off += setting_size;
        }
    } while (false);

    if (data != NULL) {
        XFree(data);
    }
    XCloseDisplay(d);
    if (found) {
        FDK_DEBUG("XSETTINGS live read: %s = '%s'", setting_name, out);
    }
    return found;
}

/* ---- the live follow (1.1.0) --------------------------------------------
 *
 * The short-lived-display reader above answers queries; this half
 * keeps the answer FRESH: the settings daemon rewrites its
 * _XSETTINGS_SETTINGS property on ITS manager window when any
 * setting changes, and a PropertyChangeMask selected on that window
 * (per-connection, server-side — the owner is not modified) routes
 * the notification into our dispatch, which re-runs the theme
 * resolution (color-scheme variant selection included). The manager
 * window can be REPLACED (daemon restart): a throttled re-arm
 * re-looks-up the selection owner and re-selects when it moved —
 * one cheap GetSelectionOwner round trip per 2 s, nothing when the
 * owner is stable.
 */

#define FDK_XSETTINGS_REARM_MS 2000

void fdk_x11_xsettings_arm(fdk_platform_connection *conn) {
    if (conn == NULL || conn->display == NULL) {
        return;
    }
    Display *d = conn->display;
    char sel_name[32];
    snprintf(sel_name, sizeof(sel_name), "_XSETTINGS_S%d", conn->screen);
    Atom sel = XInternAtom(d, sel_name, False);
    Window owner = XGetSelectionOwner(d, sel);
    if (owner == conn->xsettings_owner) {
        return; /* unchanged (including both None) */
    }
    /* Selecting on the NEW owner (even None is a no-op select — skip
     * it) and drop interest in the old one by... nothing to drop:
     * masks are per-connection-per-window; the old window's events
     * simply stop mattering (the dispatch filters by the CURRENT
     * owner + atom). */
    if (owner != None) {
        XSelectInput(d, owner, PropertyChangeMask);
        /* SYNC, not just flush: the mask change must REACH the
         * server before this call returns, or a property rewritten
         * between the arm and the next pump's flush generates its
         * PropertyNotify for a connection whose select has not
         * landed yet — the event is simply never sent (found live
         * by the color-scheme rig: the flip publish raced the
         * first pump). A round trip here runs once per owner
         * change, never per property change. */
        XSync(d, False);
        FDK_INFO("XSETTINGS live follow armed on the settings "
                 "daemon's window 0x%lx",
                 (unsigned long)owner);
    }
    conn->xsettings_owner = owner;
}

void fdk_x11_xsettings_rearm_tick(fdk_platform_connection *conn) {
    if (conn == NULL || conn->display == NULL) {
        return;
    }
    uint64_t now = xs_now_ms();
    if (now - conn->xsettings_rearm_ms < FDK_XSETTINGS_REARM_MS) {
        return; /* paced: one round trip per 2 s, at most */
    }
    conn->xsettings_rearm_ms = now;
    fdk_x11_xsettings_arm(conn);
}
