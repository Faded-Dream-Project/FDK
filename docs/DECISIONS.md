# DECISIONS — the 1.1.0 implementation log

Every non-obvious call made while closing the roadmap, with the
reason. Append-only; the date is the decision's, not the file's.

---

## D-001 — No D-Bus library dependency; a native client in raw C
**2026-10-26 (Phase 2, M-7). Owner directive, executed as written.**

FDK implements its D-Bus client from the public specification alone
(src/dbus/, include/fdk/fdk_dbus.h): unix/tcp/nonce-tcp transports,
SASL EXTERNAL (ANONYMOUS fallback), the full type system
(marshaling + unmarshaling with alignment/endianness/depth limits),
method calls/replies/errors, match rules, name ownership and
watching, properties, introspection, and serving incoming calls —
asynchronous over FDK's own event loop (ext fd sources + timers).
No libdbus, sd-bus, or GLib anywhere; no new link-time OR run-time
dependency (the GLX/fontconfig dlopen posture was not even needed —
there is nothing to load).

Sub-calls worth recording:

- **Wire truth by capture, not memory.** The marshaler's byte
  layout was verified against the reference implementation's own
  socket writes: an LD_PRELOAD shim (scripts/wirepeek.c — test
  infrastructure only, never shipped) recorded what libdbus sends
  for Hello/ListNames/GetNameOwner/variant/int64-array bodies, and
  tests/test_dbus.c pins those exact bytes (the 169-byte
  GetNameOwner frame among them). The two layout rules everyone
  gets wrong from memory — VARIANT aligns 1, and an empty array
  still pads to its element alignment — are both pinned by test.
- **Reentrancy: zombies, not sweeps.** An earlier design reaped
  dead connections from a registry sweep; the live-bus test proved
  it frees handles the application still holds (a use-after-free
  the moment a Hello times out). The contract now: a connection
  that dies stays a VALID HANDLE (torn down: fd closed, sources
  and timers removed, buffers freed) until the app calls
  fdk_dbus_connection_close(); every public entry point answers a
  dead connection with a clean error. close() from inside the
  connection's own callback defers the free to the outermost
  pump-facing wrapper (the extfd self-removal discipline).
- **The pre-ready queue is one mechanism.** Calls made before the
  handshake completes park their message on a single deferred
  queue (the pending's marker owns it; the array holds
  non-owning pointers). The first flush fires at HELLO_WAIT — the
  Hello itself rides the queue — and again at READY. The original
  two-path design (pending->queued vs deferred array) never sent
  Hello at all: the live-bus test caught it as a 10-second timeout.
- **Untrusted input posture.** Every byte from the socket is
  hostile: framing, signatures, UTF-8 validity, name grammars,
  alignment, nesting (≤64), message size (≤8 MiB default) are
  validated before allocation; SCM_RIGHTS descriptors are closed
  on arrival (fd passing is never negotiated, so their only
  possible origin is a hostile peer). A violation kills exactly
  one connection, logged once. The parser is fuzzed on every
  build (20k seeded mutations + all truncation prefixes, under
  ASan/UBSan, in plain `make test`).
- **Match rules fail OPEN client-side** (keys the evaluator does
  not implement deliver rather than drop) — the server already
  filtered; dropping would hide signals. The well-known→unique
  sender rewrite is the trap the live test hit twice (signal
  rules and PropertiesChanged): the header documents it, and
  subscribe_properties_changed scopes by arg0 (the interface)
  instead of a sender that cannot match.

**Not decided here:** unix-fd passing (UNIX_FD not negotiated) —
nothing on the 1.1.0 list needs it; revisit with the portal work
if a file-transfer API ever lands.

## D-002 — fdk_ext_loop_update_fd (extension API stays at v1)
**2026-10-26 (Phase 2).**

The D-Bus connection needed to toggle its socket between
POLLIN-only and POLLIN|POLLOUT (a socket with buffer space is
always writable — a permanently-watched POLLOUT busy-spins the
loop). Options: (a) remove + re-add the fd source (churns the
array, fires no callbacks but reorders dispatch), (b) a new
update-mask call. Took (b): `fdk_ext_loop_update_fd` appends to
the extension API without touching the ops table — FDK_EXT_API_
VERSION stays 1 (a purely additive function, no struct growth;
the version bump is reserved for ops-table growth per the ABI
policy). fdk-shell 0.1.0 binaries keep working unchanged.

## D-003 — fdk_shutdown() sweeps D-Bus connections BEFORE timers/extfds
**2026-10-26 (Phase 2).**

A connection owns ext fd sources and fdk timers. The shutdown
order in src/core/context.c sweeps connections first (teardown,
not delivery — pending replies do not fire; the documented
posture), then the timer/extfd sweeps. The reverse order would
free records the connection teardown still touches. The registry
(fdk_context->dbus_conns) exists for exactly this ordering.

## D-004 — the two-client reconciliation: main's is canonical, the alt line is archived, its unique tests are ported
**2026-10-27 (owner directive, Phase 2).**

The repo briefly carried TWO native D-Bus clients: main's (D-001,
115 exported symbols, broader surface) and an older, smaller one
developed in parallel on the `alt/dbus-client` branch (41 exported
symbols, 3 commits, archived — not merged, not deleted). The owner
ruled main's client canonical and directed a test-suite comparison:
port anything the alt battery covered that main's did not, and fix
whatever the ports uncover.

The comparison: the hostile-input coverage needed no port (main's
20k-seed mutation fuzz and prefix-exhaustive truncation walk
strictly dominate the alt's fixed mutation table). Three things
were genuinely unique and were ported: the append all-or-nothing
discipline, the reply/error constructor routing facts, and the
live-bus cross-implementation leg (libdbus — dbus-send — parsing
FDK's served reply bytes and printing them). The ports found and
closed two real defects in main's accessors:
`fdk_dbus_message_signature()` returned the frozen received-message
field for every locally built message (read back "" after appending
a body), and `fdk_dbus_message_read_args()` consulted only the
received-message body store (a locally built message read back as
"no arguments"). Both now see the builder's live buffers — exactly
what serialize() puts on the wire. The alt's recon probe
(scripts/dbus_probe.c in the staging sandbox) was NOT ported: its
wire facts are byte-pinned in main's tests and it duplicates
diagnostics the live battery already gives.

## D-005 — the desktop-integration split: what is core vs what is fdk-shell
**2026-10-27 (owner decision, recorded verbatim in intent).**

Dark-mode following (B-06), the portal FileChooser fallback (B-09),
and AT-SPI interop (B-10) belong in FDK CORE, built on the native
D-Bus client — anything every FDK app needs to behave like a native
app. The tray host (A-11/S-1), notification display (A-12/S-2), and
media controls (MPRIS) belong in fdk-shell or add-on packages —
anything a shell or panel provides TO other apps, which is exactly
the add-on surface fdk/extend.h exists for. docs/dbus.md carries
the split table; the roadmap's A-07/B-09 notes are annotated
 accordingly.
