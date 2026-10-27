# FDK's D-Bus client — what the module is and isn't

FDK speaks the D-Bus **client** protocol itself: `src/dbus/` is a
complete wire-protocol implementation written from the [public D-Bus
Specification](https://dbus.freedesktop.org/doc/dbus-specification.html)
in raw C — no libdbus, no sd-bus, no GLib/GIO, no new link-time or
run-time dependency of any kind (docs/DECISIONS.md D-001, the owner's
hard rule). The public surface is `include/fdk/fdk_dbus.h`
(112 exported symbols; `fdk_dbus_connect*` is the entry).

## What the module does

- **Transports** — unix sockets (path, abstract, dir-watching), tcp,
  nonce-tcp, percent-decoding, multi-address fallback; address
  resolution from the standard environment (`DBUS_SESSION_BUS_ADDRESS`
  / `DBUS_SYSTEM_BUS_ADDRESS`, `FDK_DBUS_ADDRESS` override).
- **Authentication** — the SASL EXTERNAL handshake with the ANONYMOUS
  fallback ladder.
- **The full wire type system** — both endiannesses, every
  alignment/depth/size limit, strict parsing of untrusted input
  (bounds, grammar, NUL, duplicate-field rejection, unknown-field
  skipping), the value-tree reading and writing API.
- **Calls and replies** — async method calls with timeouts, method
  returns and error replies with correct routing (the reply carries
  the caller's destination — the bus silently drops
  destination-less replies), serving incoming calls with an
  UnknownMethod auto-reply for anything unhandled.
- **Signals and match rules** — refcounted AddMatch/RemoveMatch,
  client-side fail-open evaluation (argN/path/type filters), typed
  PropertiesChanged subscriptions.
- **Names** — RequestName with the full flag set, name watching with
  transition reconciliation.
- **Properties and introspection** — Get/Set/GetAll helpers over any
  peer, a lenient XML introspection parser and node model.
- **The event loop** — connections ride FDK's loop as fd sources with
  retargetable interest (the POLLOUT busy-wait trap's answer);
  timeouts are loop timers. A connection may live standalone (no
  display needed) or attached to an `fdk_context`. A callback may
  close its own connection (deferred free at the outermost wrapper);
  a DEAD connection stays a valid handle until the app closes it.

## What the module doesn't do — on purpose

- **It is not a bus daemon.** FDK speaks the bus *protocol*; running
  `dbus-daemon` (or a broker) is the desktop's job, out of scope.
  The tests drive a REAL throwaway daemon via `dbus-run-session`
  (`make test-dbus`), including a cross-implementation leg where
  libdbus itself (dbus-send) parses FDK's served reply bytes.
- **It doesn't hardcode desktop policy.** The client knows the wire,
  the auth ladder, and the standard peer names — nothing more. Which
  services to talk to, which names to own, which signals to answer:
  all decided by the calling layer.

## Who builds what on top (the 2026-10-27 owner decision, D-002)

The desktop-integration features split by *who owns the surface*:

| feature | layer | rides |
|---------|-------|-------|
| dark-mode following (`Gtk/ColorScheme`, `org.freedesktop.appearance`) | **FDK core** (shipped) | the native client |
| portal FileChooser fallback | **FDK core** | the native client |
| AT-SPI interop (screen readers) | **FDK core** | the native client |
| idle/screensaver inhibition (X11 side) | FDK core | `org.freedesktop.ScreenSaver` |
| StatusNotifier **tray host** | **fdk-shell** / add-ons | the client + ext surfaces |
| **notification display** | **fdk-shell** / add-ons | the client + the overlay layer — SHIPPED as `fdk-shell/examples/notifyd.c` (S-2/A-12; the rig is `make -C fdk-shell test-notifyd`) |
| **media controls** (MPRIS) | **fdk-shell** / add-ons | the client |

The rule behind the split: anything every FDK app needs to *behave
like a native app* (theme, dialogs, accessibility) is core; anything
a shell or panel *provides to other apps* (tray, notifications,
media) is add-on territory — exactly the add-on surface
`fdk/extend.h` exists for.

## Where the truth is pinned

- `make test` — the headless battery: byte-pinned frames (against
  captured reference-implementation output), 20k-seed mutation fuzz,
  prefix-exhaustive truncation, depth bombs, both endiannesses, the
  SASL/address/match-rule grammars, the append all-or-nothing and
  reply/error routing discipline (ported from the retired alt client,
  which found two real accessor defects on arrival).
- `make test-dbus` — the live bus: a real `dbus-daemon` through
  `dbus-run-session`, plus the libdbus cross-implementation leg.
  CI makes skips fatal where dbus exists.
- `docs/DECISIONS.md` — D-001 (native, no library), D-002 (the
  core/add-on split, and the alt-client reconciliation).
- `docs/EXTENDING.md` — the D-Bus chapter for add-on authors
  (ownership rules, the sender-rewrite trap, the degradation
  contract).
