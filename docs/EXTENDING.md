# Extending FDK — the extension API (fdk/extend.h)

FDK stays small and stays the ROOT: the core and the fdk tools ship
together as one package, and everything else — panel layers,
notification shells, custom surface roles, input methods, whatever a
desktop grows — is a SEPARATE package that depends on FDK and uses
exactly what `include/fdk/fdk_extend.h` exposes. The first such
package is **fdk-shell** (wlr-layer-shell / EWMH dock surfaces); its
source is the reference consumer of every function here and the
proof the API is complete enough to build on.

This document is the contract. The header carries the per-function
reference comments; this file is the narrative: the model, the
ownership rules, the recipes, and the honest limits.

---

## 1. The model in one paragraph

An add-on REGISTERS a **surface kind** — a name plus a small table
of callbacks (`fdk_ext_register_kind` + `fdk_ext_surface_ops`).
Creating a surface of that kind (`fdk_ext_surface_create`) calls the
add-on's own `create` callback, which builds the NATIVE surface
itself — an X11 window through the `Display*` this API hands out, or
a `wl_surface` through the `wl_display*` plus whatever protocol role
the add-on speaks — and returns the native handle. FDK then ADOPTS
that surface: it joins the event routing (the add-on receives the
same backend-neutral `fdk_event_data` stream applications get), it
renders through the ordinary public `fdk_surface` API, it can parent
popups, and the add-on's own file descriptors can join FDK's event
loop. FDK never learns what the role IS; the add-on never touches an
FDK private struct.

## 2. Versioning — how an add-on declares what it needs

The extension API carries its own version, independent of FDK's
package version. Additions bump `FDK_EXT_API_VERSION`; nothing is
ever removed or renamed (the same append-only discipline as
docs/abi-policy.md applies to the core). An add-on pins what it was
written against — compile-time AND run-time, both:

```c
#include <fdk/fdk_extend.h>

#if !defined(FDK_EXT_API_VERSION) || FDK_EXT_API_VERSION < 1
#error "this add-on needs FDK's extension API v1 (FDK 1.1.0)"
#endif

int addon_init(fdk_context *ctx) {
    if (fdk_get_version() < FDK_VERSION_ENCODE(1, 1, 0)) return -1;
    if (fdk_ext_api_version() < 1) return -1;   /* the header/lib pair */
    ...
}
```

A newer FDK keeps old add-ons working: the runtime number only
grows. The package version check matters when the add-on needs a
core feature that predates the extension API's bump cadence — use
`FDK_VERSION_ENCODE` comparisons, never string parsing.

## 3. Ownership and lifetimes — read this once, then trust it

- The ADD-ON owns its **ROLE objects** (the protocol objects it
  created on the native surface: a `zwlr_layer_surface_v1`, a set of
  X11 properties, anything). `ops.destroy` is where it tears them
  down; FDK calls it BEFORE destroying the native surface, so the
  role teardown runs against a live surface.
- FDK owns the **NATIVE SURFACE ITSELF** once `ops.create` returns
  it (the X window is `XDestroyWindow`'d, the `wl_surface` is
  `wl_surface_destroy`'d, by FDK — never by the add-on). On adopt
  failure FDK still performs this destruction after calling
  `ops.destroy`, so every failure path is symmetric.
- An `fdk_ext_surface*` is valid from a successful create until
  `fdk_ext_surface_destroy()` — or until the owning context is shut
  down (the sweep covers leaks).
- Kinds are add-on-owned objects: `fdk_ext_kind_destroy()` frees
  one; a kind with live surfaces refuses (`FDK_ERR_INVALID_STATE`).

## 4. Surface kinds

```c
fdk_ext_surface_ops ops = {0};
ops.struct_size      = sizeof(ops);
ops.create           = my_create;      /* required */
ops.destroy          = my_destroy;     /* optional */
ops.popup_attached   = my_popup_hook;  /* Wayland roles that parent popups */

fdk_ext_surface_kind *kind = NULL;
fdk_result r = fdk_ext_register_kind(ctx, "org.me.mysurface", &ops,
                                     user_data, &kind);
```

`struct_size` makes the table append-only-safe: FDK copies exactly
that many bytes, so an add-on compiled against an older FDK keeps
working when fields are added later. Kind names are unique per
context (a duplicate fails with `FDK_ERR_INVALID_ARGUMENT` — the
double-init detector).

### The create callback — building the native surface

```c
static fdk_result my_create(fdk_ext_surface *surface,
                            fdk_ext_native_handle *out_native,
                            void *user) {
    fdk_context *ctx = fdk_ext_surface_context(surface);

    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd)))
        return FDK_ERR_NOT_INITIALIZED;

    if (nd.backend == FDK_EXT_BACKEND_X11) {
        Display *dpy = (Display *)nd.display;
        Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                       x, y, width, height, 0,
                                       BlackPixel(...), WhitePixel(...));
        /* ...your properties (window type, struts, protocols)... */
        out_native->backend = FDK_EXT_BACKEND_X11;
        out_native->xid = (fdk_u64)w;
        return FDK_OK;
    }

    /* Wayland: bind what you need through YOUR OWN registry on the
     * shared display (a second registry is ordinary Wayland —
     * events fan out per listener; FDK's registry is untouched),
     * create the surface from wl_compositor, give it your role. */
    ...
    out_native->backend = FDK_EXT_BACKEND_WAYLAND;
    out_native->pointer = my_wl_surface;
    return FDK_OK;
}
```

Rules that matter:

- **X11**: build the window UNMAPPED with your properties already
  set. FDK maps it at `fdk_ext_surface_show()` and installs its own
  event mask at adopt (the standard FDK input set — your
  Expose/StructureNotify/input all flow through the normal event
  stream afterwards).
- **Wayland**: do NOT commit in create. Your role's initial
  configure handshake comes first; commit ordering is yours to obey,
  and FDK's present path commits only when the add-on renders. (For
  layer-shell: create the role, set your parameters, and commit an
  EMPTY surface at show-time to start the configure — exactly what
  fdk-shell's `fdk_shell_layer_show` does.)
- **Wayland, the buffer side (1.1.0-rc1)**: FDK now ENFORCES the
  "no buffer before the first configure" rule on the present path —
  a present before your first `fdk_ext_surface_notify_configure()`
  is deferred and committed post-ack by that call (or re-driven via
  EXPOSE when the configure changed the size). An add-on that paints
  eagerly right after `show()` — the way every FDK example does —
  can no longer trip the protocol error that killed the demo bar on
  labwc and sway (`zwlr_layer_surface_v1: error 2: layer_surface has
  never been configured`). Your obligation narrows to: ack your
  role's configure, then report it.

### The destroy callback — role teardown

Runs on EVERY destroy path (your `fdk_ext_surface_destroy`, the
parent sweep of a popup, `fdk_shutdown`'s leak sweep), always while
the native surface is still alive. Tear down role objects; do not
destroy the native surface (FDK does that next); do not destroy
OTHER surfaces.

### The popup_attached hook — Wayland only

FDK created an `xdg_popup` parented to your surface's `wl_surface`
and hands the popup proxy over so your ROLE can adopt it — the
wlr-layer-shell pattern:

```c
static void my_popup_attached(fdk_ext_surface *surface,
                              void *native_popup, void *user) {
    /* native_popup is the struct xdg_popup* — cast it here, the
     * only place wayland-client types may appear. */
    zwlr_layer_surface_v1_get_popup(my_layer_surface(surface),
                                    (struct xdg_popup *)native_popup);
}
```

X11 never calls this (the X tree parents popups to your window
naturally). May be NULL — the popup then stands on its grab alone.

## 5. Events

```c
static void my_event(fdk_ext_surface *surface,
                     const fdk_event_data *event, void *user) {
    switch (event->type) {
    case FDK_EVENT_WINDOW_CONFIGURE: /* resize; redraw now */ break;
    case FDK_EVENT_WINDOW_EXPOSE:    /* repaint */             break;
    case FDK_EVENT_POINTER_BUTTON_DOWN: /* ... */              break;
    case FDK_EVENT_KEY_DOWN:         /* ... */                 break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST: /* environment closed us */ break;
    default: break;
    }
}
fdk_ext_surface_set_event_callback(surface, my_event, user);
```

The SAME backend-neutral stream applications receive on windows
(fdk_event.h documents every event). Configure arrives:

- **X11**: from the server's ConfigureNotify, automatically.
- **Wayland**: when the add-on reports its role's assigned size via
  `fdk_ext_surface_notify_configure(surface, w, h)` — call it from
  your role's configure callback, after acking (fdk-shell's layer
  configure listener is the reference). FDK resizes its render
  machinery and delivers the event.

**EXPOSE is a repaint obligation, not a notification** (found live,
2026-10-25, in fdk-shell's demo bar): an add-on that drives its
surface directly has no widget tree to auto-repaint it, so the
`FDK_EVENT_WINDOW_EXPOSE` branch must re-draw and re-present the
surface itself. The retained-damage model otherwise concludes
nothing changed and paints nothing — and on X11 (where an unmapped
or unoccluded window's pixels are gone) that leaves whatever the
desktop painted underneath showing in the surface's region. The
canonical shape is fdk-shell's demo bar handler: `EXPOSE -> re-fill
-> fdk_surface_present()` (see the desktop-switch rig in
fdk-shell/tests/test_x11.c for the end-to-end proof).

## 6. Rendering

```c
fdk_surface *s = NULL;
fdk_ext_surface_get_surface(surface, &s);

fdk_surface_info info;
fdk_surface_get_info(s, &info);   /* reacquire every frame   */
/* ...fdk_surface_fill / fill_rect / draw_line / blit / your pixels... */
fdk_surface_invalidate(s, rect);  /* after RAW pointer writes */
fdk_surface_present(s);           /* damage-tracked present  */
```

The ordinary PUBLIC `fdk_surface` API — the same objects, the same
lifetimes as an application's window surface (owned by the ext
surface; never passed to `fdk_surface_destroy`). `frame_ready()`
pacing applies on Wayland. The extension surface itself is never
exposed as an `fdk_window*`, so the window-API operations that would
be meaningless on a role-carrying surface (set_title, maximize,
resize) are unreachable by construction.

### The hover cursor (1.1.0-rc1)

```c
fdk_ext_surface_set_cursor(surface, FDK_CURSOR_POINTER);
```

The shape the pointer takes while hovering the surface — the standard
arrow or the pointing hand, exactly the two shapes a panel needs
(`fdk_cursor_shape` in `fdk/fdk_window.h`; unknown values are a
no-op). The shape is re-applied automatically on every pointer entry:
Wayland cursor state does not survive leaving a surface, so a client
that answers no `wl_pointer.enter` at all keeps whatever shape the
compositor last showed (the W2 live report: the resize cursor
"sticking" over the bar until the pointer left). On Wayland the
preferred transport is cursor-shape-v1 when the compositor offers it,
falling back to `wl_pointer.set_cursor` with an XCursor-theme image;
on X11 it is a `XDefineCursor` with the cursor-font hand. A backend
without the mechanism keeps the platform default — a hint, never an
error. Typical panel usage: the hand while the pointer hovers a
clickable region, the arrow otherwise (the demo bar's `set_cursor`
calls in `fdk-shell/examples/bar.c` are the worked example).

## 7. Popups (dropdowns)

```c
fdk_ext_surface *pop = NULL;
fdk_ext_popup_create(parent_surface, x, y, w, h, &pop);
fdk_ext_surface_set_event_callback(pop, popup_event, user);
fdk_ext_surface_show(pop);
```

Parent-relative positioning, no WM/compositor chrome, an input grab
while shown, and dismissal by outside click or Escape — delivered as
`FDK_EVENT_WINDOW_CLOSE_REQUEST` on the POPUP's callback (the
application responds by destroying it). The popup is itself an
extension surface: render and receive events exactly like the
parent. Popups must not outlive their parent (destroying the parent
sweeps them).

## 8. The event loop — fd sources (and timers)

```c
void my_fd_cb(fdk_ext_fd_source *src, uint32_t revents, void *user) {
    /* read your socket; may remove ANY source (including this one),
     * add sources (first fired on a LATER pump), destroy windows,
     * call fdk_quit() — the timer discipline. */
}

fdk_ext_fd_source *src =
    fdk_ext_loop_add_fd(ctx, my_socket_fd, FDK_EXT_FD_IN, my_fd_cb, user);
...
fdk_ext_loop_remove_fd(src);   /* safe inside its own callback */
```

The watched fd wakes `fdk_pump_events()` / `fdk_run()` exactly like
display input; the callback fires from inside the pump, after the
backend's events. `FDK_EXT_FD_IN/OUT` are the watch flags;
`ERR/HUP` are reported like poll(2) reports them. The fd is never
closed by FDK.

**Retargeting the watch mask** (1.1.0): a protocol with an output
buffer should watch `IN` alone while the buffer is empty and add
`OUT` only while bytes are pending —

```c
fdk_ext_loop_update_fd(src, FDK_EXT_FD_IN);              /* drained */
fdk_ext_loop_update_fd(src, FDK_EXT_FD_IN | FDK_EXT_FD_OUT); /* busy */
```

— because a socket with buffer space is ALWAYS writable: a
permanently-watched POLLOUT wakes every pump for nothing (the
busy-wait trap; the native D-Bus client is the first consumer).
Safe inside the source's own callback.

**Timers need no extension twin**: the public
`fdk_timer_add` / `fdk_timer_remove` / `fdk_timer_reset` API (fdk
1.3.1) IS the loop's timer surface — callable by any library on top
of FDK. One primitive, not two.

## 9. Backend + capability queries

```c
switch (fdk_ext_active_backend(ctx)) {
case FDK_EXT_BACKEND_X11:     ... break;
case FDK_EXT_BACKEND_WAYLAND: ... break;
default: /* no live connection */ break;
}

if (fdk_ext_has_capability(ctx, FDK_EXT_CAP_EWMH_DOCK)) { ... }
```

| Capability | X11 | Wayland | Meaning |
|---|---|---|---|
| `FDK_EXT_CAP_SHM_RENDER` | yes | yes | ext surfaces render via `fdk_surface` |
| `FDK_EXT_CAP_POPUP_GRAB` | yes | yes* | popups grab input + dismiss properly |
| `FDK_EXT_CAP_EWMH_DOCK` | yes | no | dock hints settable by the add-on |
| `FDK_EXT_CAP_FRAME_PACING` | no | yes | compositor frame feedback exists |

\* the Wayland caveat every toolkit documents: compositors may
refuse a grab whose serial predates all input (the popup still
shows; it dismisses on focus loss instead).

## 10. Native access — the two accessors

```c
fdk_ext_native_display nd;
fdk_ext_get_native_display(ctx, &nd);
/* nd.backend == FDK_EXT_BACKEND_X11     -> nd.display is Display*
 * nd.backend == FDK_EXT_BACKEND_WAYLAND -> nd.display is struct wl_display *
 * borrowed for the connection's lifetime */

fdk_ext_native_handle nh;
fdk_ext_surface_native(surface, &nh);
/* X11: nh.xid is the Window; Wayland: nh.pointer is the wl_surface*
 * valid while the surface lives */
```

No Xlib or wayland-client TYPE appears in any FDK public header
(the "no backend leakage" rule, docs/architecture.md) — the handles
are `void*`/XID with a tag saying which world they belong to, and
the ADD-ON includes the native headers it needs and casts to the
documented type. Xlib and wayland-client are then the add-on's own
dependencies, exactly as they would be for any native client — the
same license FDK itself grants its backends, handed outward through
documented accessors instead of private structs.

On Wayland, bind your own globals through your OWN
`wl_display_get_registry()` (see the create recipe above) — one
roundtrip collects them; FDK's listeners are dispatch-safe so the
trip is harmless at any time.

## 11. The quality gates an add-on inherits

- Threading: everything is UI-thread-only, like FDK (docs/threading.md).
- The isolation proof: build your package against an INSTALLED FDK
  in a throwaway prefix, with no other FDK include path on the
  compile line — a private reach is then a compile error, not a
  review finding. fdk-shell's `tests/isolation.sh` is the template
  (it also audits the dynamic boundary: every imported `fdk_*`
  symbol must resolve from `libfdk.so.1`).
- Your package's shared library should export only its own
  namespace (a version script — see fdk-shell's
  `src/libfdk-shell.exports.map`): the `fdk_*` namespace belongs to
  FDK alone.
- Run your tests under ASan/UBSan like FDK's own suites; suppress
  only third-party allocations by module (the GL-driver pattern in
  tests/data/lsan-gl.supp).

## 12. What FDK deliberately does NOT provide

- No GL/EGL surface access for extension surfaces — the extension
  render path is the software `fdk_surface` path (identical pixels
  everywhere, the hybrid renderer's CPU half).
- No wl_seat / wl_data_device handles: input and clipboard flow
  through FDK's translated events and the public clipboard API. An
  add-on needing RAW seat access would need a new accessor — raise
  it as an API proposal, never work around it.
- No per-output geometry on X11 (the honest single screen; RandR is
  a possible future capability).
- Layer stacking LEVELS are protocol truth on Wayland; on X11 they
  ride whatever the WM gives dock-typed windows (fdk-shell documents
  this delta instead of faking fidelity).

---

## 13. The D-Bus client — a first-class native subsystem (1.1.0)

`include/fdk/fdk_dbus.h` is FDK's own D-Bus client, implemented
from the public specification in raw C (src/dbus/) with **no
libdbus, sd-bus, or GLib anywhere** — the "no D-Bus library
dependency" decision (docs/DECISIONS.md D-001). It exists because
half the desktop-integration roadmap (dark mode on Wayland, tray,
notifications, portals, AT-SPI, idle inhibit on X11) is one bus
away, and FDK was not going to take a dependency to get it.

The shape an add-on sees:

```c
static void on_ready(fdk_dbus_connection *c, int ok, const char *err, void *u) { ... }
static void on_reply(fdk_dbus_connection *c, fdk_dbus_message *reply, void *u) {
    if (reply && !fdk_dbus_message_is_error(reply)) {
        fdk_dbus_value **args = NULL; size_t n = 0;
        if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK && n >= 1) {
            const char *s = fdk_dbus_value_string(args[0]);
            ...
            fdk_dbus_values_free_array(args, n);
        }
    }
}

fdk_dbus_connection *bus = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS,
                                            on_ready, user);
/* later, once ready: */
fdk_dbus_message *call = fdk_dbus_message_new_method_call(
    "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "ListNames");
fdk_dbus_connection_call(bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                         on_reply, user);   /* consumes `call` on success */
```

Everything is asynchronous and rides the SAME pump as your
surfaces: the socket is an ext fd source (the update_fd recipe
above), timeouts are fdk timers, and every callback fires between
the connection's enter/leave discipline — a callback may call any
fdk_dbus function, including `fdk_dbus_connection_close()` on the
connection whose callback is running (the free is deferred; do not
touch the handle afterwards, exactly like removing an fd source
inside its own callback).

**The three ownership rules** (the whole memory model):

1. `fdk_dbus_value` trees, messages, and the `read_args` array are
   owned by whoever last received them; `fdk_dbus_values_free_array`
   for read_args output, `fdk_dbus_message_free` otherwise.
2. `fdk_dbus_connection_call/send` CONSUME the message on success
   (and leave it with you on failure — check the return, then free
   on the failure paths only).
3. A connection that dies stays a valid handle (every call answers
   `FDK_ERR_INVALID_STATE`/NULL) until you `close()` it; close() is
   the only free.

**Degradation is the contract**: no bus reachable →
`fdk_dbus_connect()` returns NULL after ONE log line, immediately
(a refusing socket never blocks). Features built on it check for
the connection and disable themselves; nothing busy-loops, nothing
crashes. That is what makes the tray/notification/dark-mode layers
safe on machines without a session bus.

**Where the bodies are buried** (things the wire will teach you
the hard way; the header documents each, the tests pin each):

- The bus rewrites a well-known SENDER into the owner's unique
  name in message headers — a `sender='org.example.Thing'` match
  rule never fires. Scope rules by interface/member/arg0, or
  watch the name and match its unique owner
  (`fdk_dbus_connection_watch_name` gives you the transitions).
- VARIANT aligns 1, and an empty array still pads to its element
  alignment — the two marshaling rules everyone gets wrong; both
  are pinned byte-for-byte against captured reference-
  implementation output in tests/test_dbus.c.
- `a{sv}` dict values are VARIANTS: `fdk_dbus_value_dict_lookup`
  then `fdk_dbus_value_variant_value` is the reader idiom; the
  constructors validate and reject a bare value where a variant
  belongs (loudly, at construction — not silently on the wire).
- Replies are delivered at most once; a late reply to a canceled
  call is dropped silently (the specification's own rule).

**Test coverage**: tests/test_dbus.c (headless: the value tree,
marshaling with pinned reference bytes, the parser under every
truncation prefix and 20k seeded mutations — ASan/UBSan under
plain `make test`) and tests/test_dbus_live.c (`make test-dbus`:
a REAL dbus-daemon via dbus-run-session — SASL+Hello, method
calls routed through the daemon, error replies, timeouts,
signals with arg0-filtered match rules, Properties Get/Set +
typed PropertiesChanged, name watching through its full lifecycle,
introspection, and close-from-inside-its-own-callback). The
ubuntu CI job runs the live suite with skips made fatal.

## 14. Desktop notifications — the app side (1.1.0)

`include/fdk/fdk_notify.h` is the CLIENT half of
org.freedesktop.Notifications: send a notification to whatever
daemon the desktop runs, receive closed/action events, withdraw
early. It owns its session-bus connection, discovers the daemon
by name, and reports every absence (no bus, no daemon, daemon
restart) as a STATUS — never a crash, never a busy-loop.

```c
#include <fdk/fdk.h>

static void on_sent(fdk_notify *n, uint32_t id, void *note, void *u) {
    printf("shown as id %u\n", id);           /* the replace key */
}
static void on_action(fdk_notify *n, uint32_t id, const char *key,
                      void *note, void *u) {
    if (strcmp(key, "open") == 0) { /* the user clicked "Open it" */ }
}
static void on_closed(fdk_notify *n, uint32_t id, uint32_t reason,
                      void *note, void *u) { }

int main(void) {
    fdk_context *ctx = NULL;
    fdk_init(&ctx, NULL);

    fdk_notify_callbacks cb = {
        .on_sent = on_sent, .on_action = on_action,
        .on_closed = on_closed,
    };
    fdk_notify *n = fdk_notify_open(ctx, &cb);

    static const char *actions[] = {"open", "Open it", NULL};
    fdk_notify_request req = {
        .app_name = "myapp",
        .icon = "dialog-information",     /* theme name or file:// */
        .summary = "Download complete",
        .body = "<b>report.pdf</b> is ready",
        .actions = actions,
        .urgency = FDK_NOTIFY_URGENCY_NORMAL,
        .timeout_ms = -1,                 /* the daemon's default  */
    };
    fdk_notify_send(n, &req, NULL /* note_user travels back */);

    fdk_run(ctx);                          /* events fire from here */
    fdk_notify_close(n);
    fdk_shutdown(ctx);
}
```

The daemon's capabilities (`fdk_notify_capabilities`, once
`on_ready` fired) tell you what it renders ("body-markup",
"actions", "icon-static", ...). `image_data`/`image_width`/... in
the request carry a raw RGB/RGBA buffer as the image-data hint.
Replaces: pass the id `on_sent` gave you as `replaces_id`.
Every event about one notification carries the `note_user` its
send took — no id bookkeeping is forced on the app.

**Test coverage**: tests/test_notify_client.c (`make test-dbus`,
same dbus-run-session): an in-test mock daemon on a REAL bus —
the adoption flow, a full Notify round trip with EVERY marshaled
field validated at the mock (the image-data bytes read back
flat), withdraw/replaces, malformed-request refusal, the daemon
vanish/return cycle, and teardown-inside-its-own-event.

## 15. The tray — the app side (1.1.0)

`include/fdk/fdk_tray.h` is the ITEM half of the desktop tray:
a StatusNotifierItem export (both the org.kde and org.ayatana
spellings) plus a complete DBusMenu server. The item registers
with whichever StatusNotifierWatcher answers — KDE name first,
the ayatana twin as the fallback — re-registers through watcher
restarts, and reports HOSTED / UNHOSTED through `on_host`.

```c
fdk_tray_item_options opts = {
    .id = "myapp-net",                    /* stable, app-unique   */
    .title = "MyApp network",
};
fdk_tray_item *item = fdk_tray_item_open(ctx, &opts, NULL, NULL);

/* A pixmap: ARGB32 in NETWORK (big-endian) byte order. */
static const uint8_t px[16] = { 0xFF,0x00,0x00,0xFF, /* ... */ };
fdk_tray_item_set_icon_pixmap(item, 2, 2, px, sizeof px);
fdk_tray_item_set_tooltip(item, NULL, "MyApp", "All good");
fdk_tray_item_set_status(item, FDK_TRAY_STATUS_ACTIVE);

fdk_tray_menu *menu = fdk_tray_menu_create();
uint32_t quit = fdk_tray_menu_add(menu, 0, "_Quit");
fdk_tray_menu_on_click(menu, on_menu_click, NULL);
fdk_tray_item_set_menu(item, menu);
/* live updates: set_label/set_check/set_enabled emit
   LayoutUpdated; hosts re-fetch. Radio semantics are the
   toolkit-standard sibling clearing. */
```

The menu serves com.canonical.dbusmenu's five core calls
(GetLayout — the two-out-arg nested struct — GetGroupProperties,
GetProperty, AboutToShow with the dirty handshake, Event); the
optional bulk group forms are refused with UnknownMethod (an
honest refusal beats a guessed signature — FDK's own host never
calls them). Labels may carry the `_` mnemonic convention.

**Test coverage**: tests/test_tray_item.c (`make test-dbus`): an
in-test mock watcher/host on a REAL bus — registration, the
host's GetAll walk (pixmap bytes flat-read, the ToolTip shape),
the full GetLayout tree walk (labels, mnemonic, separator, check,
radio pair, 2-deep submenu, disabled), Event→click callback,
LayoutUpdated + the AboutToShow dirty handshake, New*/Properties-
Changed receipts, activation/scroll, and the watcher
vanish/return cycle with re-registration.

## 16. The UI font size — one number, everywhere (1.1.0)

`fdk_ui_font_size()` / `fdk_ui_scale()` / `fdk_ui_font_px()`
(fdk_theme.h) answer "what size is the UI text?" the same way
`fdk_system_color_scheme()` answers "dark or light?": the
`$FDK_UI_FONT_SIZE` / `$FDK_UI_SCALE` overrides, then FDK's
settings store (`ui.font_size` / `ui.scale` — `fdk-set fdk
ui.font_size 18`), then the desktop's statement (XSETTINGS
`Gtk/FontName` on X11; the portal's `font-name` and
`text-scaling-factor`), then the 16 px / 1.0 defaults. The
answer follows the desktop LIVE (the same events the color
scheme rides) unless the environment or the settings store owns
it. Getters never touch a socket.

The reference shells (notifyd, trayhost) scale EVERYTHING off
this number — card and bar geometry, paddings, icon boxes,
button and menu rows — with `--font-size N` pinning it for
tests; `FDK_UI_FONT_SIZE=24 notifyd` is the whole setup. Core's
own chrome follows with `fdk__ui_derived_px(ref)`: a
historically-fixed size re-derived at
`round(ref * fdk_ui_font_px() / 14)` (the design baseline), so
`FDK_UI_FONT_SIZE=14` lands every value on its historical pixel
size — the backward-compatible escape hatch.
