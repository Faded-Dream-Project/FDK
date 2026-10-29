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

## D-006 — notifyd: the notification daemon's shape (S-2 / A-12)
**2026-10-27.**

The notification display is **one card per notification, one
zwlr-layer / EWMH-overlay layer surface per card, sized EXACTLY to
the card** — not one big stack surface. The alternatives traded
against:

- *One surface per output (the mako shape)*: repaint-on-any-change
  with zero flicker, but the inter-card gaps and side padding sit
  INSIDE the input surface — clicks there are eaten (the
  transparent-margin trap), and without arbitrary input regions
  (A-06 is still partial: full-surface transparency only) the
  stack surface cannot make its dead zones click-through. The
  per-card shape makes the gaps genuinely outside any surface;
  only the 4–8 px rounded CORNERS eat clicks, which is the honest
  limit until S-4 lands input regions.
- *Re-anchoring a card in place when the stack shifts*: layer
  margins are create-time parameters (A-05's documented
  destroy/re-create idiom), so a stack change re-creates the
  layers BELOW the change point. Same-size content updates (the
  common progress-bar replace) repaint in place with no flicker.

Other recorded calls: links are RENDERED (underlined, themed link
color) and never followed or fetched — `<img>` is stripped with its
src never opened; the body translator maps the spec's `<b> <i> <u>
<a>` onto FDK's markup vocabulary and filters everything else; the
summary is escaped plain (the spec allows markup in the BODY only).
The bus-name etiquette is the polite-daemon posture:
ALLOW_REPLACEMENT always, REPLACE_EXISTING only with `--replace`,
DO_NOT_QUEUE unless `--wait`; an incumbent is named through its own
GetServerInformation (one clear line) and never fought. Critical
urgency with the server-default timeout never expires (the spec's
recommendation); explicit timeouts are always honored. The rate
limit (25 Notify/s) and live cap (32) drop content but still answer
every call — a flooding client must never block or spin the daemon.
The rig (fdk-shell/tests/notifyd_rig.sh) pins all of this with
dbus-send (libdbus) and gdbus (GLib) as the cross-implementation
callers and gdbus monitor as the signal receiver.

## D-007 — trayhost: the StatusNotifier tray host's shape (S-1 / A-11)

**Context.** S-1 needs a full org.kde.StatusNotifierWatcher host:
the registration service, the item protocol (properties + New*
signals + menus), and a rendering surface. The sandbox rig proves
the whole path against a real dbus-daemon with a synthetic item
(the probe), but shape decisions were made that a real desktop
will rub against — recorded here, honestly.

- *One floating strip, not reserved space*: the tray renders as a
  TOP-layer pill at the top-right edge with
  `exclusive_zone = 0` — it floats OVER panels rather than
  reserving strut space. A panel embedding a tray wants the panel
  to own the strut and the tray to live inside it; that is an
  embedding API (the bar's pattern), not the standalone host's
  job. `--anchor/--margin/--cell/--bar-height` cover placement.
- *Passive fades, never hides*: a Passive item draws at alpha
  0.45 (a scratch-surface alpha multiply — the transform blit
  has no alpha knob). Hiding it would break the "still there,
  still clickable" contract the SNI status encodes.
  NeedsAttention swaps to the attention pixmap and adds the
  accent marker strip; the host does NOT animate urgency
  (blinking is a panel-policy decision, D-007's honest limit).
- *No tooltip windows*: ToolTip data is parsed and kept (title +
  description), but nothing renders it — a hover tooltip is a
  separate popup contract (S-4 input regions would sharpen its
  pointer targeting); the standalone host declines to fake it.
- *Both registration dialects*: the KDE form (a bus name whose
  object path is the canonical /StatusNotifierItem) and the
  Ayatana form (a bare object path, the caller being the item).
  Signal subscriptions are scoped to the item's UNIQUE bus name
  (":1.x" — the header value the bus rewrites well-known names
  into; a well-known-sender rule would never fire, the
  documented fdk_dbus.h trap).
- *Vanished items drop by NameOwnerChanged* (the owner_sub,
  held for the connection's lifetime — freeing a subscription
  REMOVES its match rule, a bug the rig's drop legs caught):
  hostile clients never unregister politely, so the host watches
  owner loss, not the Unregister call.
- *The dbusmenu contract, pinned*: GetLayout's reply is TWO
  out-arguments (u revision + the BARE root struct — not
  variant-wrapped; the av children ARE variant-wrapped);
  AboutToShow precedes each open; Event(id, "clicked") fires on
  row selection with NO_REPLY_EXPECTED; mnemonic underscores are
  stripped at parse ("Fi_rst" renders "First"). Menu refreshes
  arriving while a menu is OPEN are discarded (stale-safe): the
  open cascade's rows point INTO the old tree; replacing it
  mid-open would dangle every row — the next open re-fetches.
- *The cascade*: submenus open as child popups at the parent's
  right edge, flipping LEFT when the global right edge is near
  (the global-x chain is tracked at creation — popups carry no
  query API). Destroying the ROOT popup lets the backend's
  family sweep take the native children (destroying them again
  would double-free); the wrappers free by hand.
- *IconTheme truth*: the property answers the override channel
  the icon lookup itself honors first (FDK_ICON_THEME) or
  "default" — a public-API client cannot read the discovered
  theme name (the internal accessor is not exported), and
  pixmaps are the primary channel for real items anyway.
- *Activate coordinates*: SCREEN coordinates where the anchoring
  math makes them knowable (X11; the trace logs the computed
  bar origin), layer-local otherwise — Wayland has no global
  space, the documented layer-shell truth.
- *Self-registration over the real bus path*: after taking the
  Watcher name the host calls RegisterStatusNotifierHost through
  its own socket and is served by its own handler — exactly as a
  third party's registration would be (no shortcut).

The rig (fdk-shell/tests/trayhost_rig.sh, 62 checks) pins all of
this with dbus-send (libdbus) and gdbus (GLib) as the
cross-implementation callers, gdbus monitor as the signal
receiver, and the probe — a synthetic item with the
dual-resolution a(iiay) icon, the full dbusmenu tree, and a live
control interface — as the item side; X11 legs pixel-verify the
bar, the fade blend (0.45·icon + 0.55·bg), the amber attention
swap, and the cascade's edge flip under XTEST input; Wayland legs
prove the layer-shell handshake under headless sway.

## D-008 — the reusable notification/tray split: app-facing API in core, host-side model in fdk-shell

**2026-10-27 (TASK B, the owner's directive).**

Two audiences, two packages, one protocol each — the split
follows D-005's line (what every app needs vs what a shell
provides TO other apps):

- **The APP side lives in FDK core** (`fdk_notify.h`,
  `fdk_tray.h`, src/desktop/): a notification CLIENT (send /
  replace / withdraw / closed+action events) and a tray ITEM
  export (SNI properties + DBusMenu server). Any FDK app can now
  live in the tray and raise notifications against any desktop's
  daemon/host, on the native D-Bus client, zero new deps. Rigged
  against in-test mocks on a REAL bus (make test-dbus) — the
  mock daemon/watcher validates every marshaled field, including
  the flat byte-array image-data.
- **The HOST side stays in fdk-shell** (notifyd, trayhost — the
  reference UIs). The model/drawing split (the protocol and
  state machinery extracted from the renderers) is the next
  slice; the current examples remain the executable spec.

Recorded calls: the notification client owns its OWN bus
connection (a daemon's traffic and an item's export each keep a
socket — the bus is cheap, and one connection per service object
keeps method-serving unambiguous). The tray item registers
KDE-first, ayatana-fallback, and re-registers on watcher
restarts, but does NOT reconnect a dead bus (an icon has no
traffic to piggyback a retry on — the honest limit, documented
in the header; the notification client DOES retry, its sends
being the natural retry point). The dbusmenu server refuses the
optional bulk group calls with UnknownMethod. The SNI emission
discipline is dual: the New* family AND PropertiesChanged, both
interface spellings — misses nobody. Every a{sv} builder in the
new code follows the ownership rule pinned by the trayhost bug
hunt: containers TAKE their children on success only.

The byte-array bulk form (`fdk_dbus_value_new_byte_array` /
`_array_flat_bytes`) grew out of this: a 256x256 icon was 256K
heap nodes on the UI thread; it is now ONE allocation, marshaled
as one block, and the UNMARSHALER produces the same flat form —
every 'ay' on the wire is flat from here on.

## D-009 — the UI font size: one process-wide answer, everything scales off it

**2026-10-27 (TASK C, the owner's directive).**

The complaint: notifyd/trayhost rendered at a fixed 14 px. The
fix's shape mirrors the color-scheme module exactly — one cached
answer over four channels, live-refreshed by the same events:
`$FDK_UI_FONT_SIZE`/`$FDK_UI_SCALE` overrides > FDK's settings
store (`ui.font_size`/`ui.scale`) > the desktop's statement
(XSETTINGS `Gtk/FontName` parsed as points at 96 dpi; the
portal's `font-name` and `text-scaling-factor`) > the 16 px /
1.0 defaults. FDK settings override the desktop (the owner's
explicit choice wins); the desktop is followed live otherwise.

Scaling: the shells derive EVERY dimension from
`fdk_ui_font_px()` at the 14 px design baseline (k = px/14) —
card width and pads, icon box, buttons, bar height, cell, menu
rows — so text and geometry grow together (the rigs' XS sweeps
prove 12/16/24/32 with glyph-free side pads and on-screen
menus). Core chrome re-derives via
`fdk__ui_derived_px(ref) = round(ref·px/14)`: the historical
widget sizes keep their RATIOS (tooltip stays smaller than
dialog), and `FDK_UI_FONT_SIZE=14` lands every value on its
historical pixel size — the escape hatch. The rigs pin
`--font-size 14` for their pixel legs (byte-identical geometry)
and the sweep legs drive the sizes through the ENV channel —
proving the metrics chain end to end, not just the arithmetic.

## D-010 — text shaping: dlopen'd HarfBuzz + FriBidi, placements as the one seam

**2026-10-28 (M-2 / B-02, the owner's "real toolkit, not a toy"
directive).**

The gap: every text walk was codepoint-at-a-time LTR — Arabic,
Hebrew, Persian, Urdu rendered in LOGICAL order (backwards), Indic
conjuncts came out malformed, mixed-script strings needed manual
font switching. The decision has three parts:

**1. The dependency posture — the GLX/fontconfig rule, verbatim.**
HarfBuzz and FriBidi are dlopen'd at run time, never build
dependencies, never dlclose'd, and absent libraries degrade to the
1.4.x walk with IDENTICAL metrics (pinned by a self-re-exec'ing
test). The alternative — a hard link — would hand a C toolkit a
C++-runtime dependency chain (harfbuzz pulls glib on some distros)
for zero additional capability; the dlopen route gets the full
pipeline on every real desktop while a pristine container keeps
building and testing green. `$FDK_SHAPING=0/1` pins the posture for
tests and diagnostics. The vendored ABI declarations (no system
headers) mirror the fontconfig layer; the soname probe covers
Debian's `libharfbuzz.so.0b` and everyone else's `libharfbuzz.so.0`.

**2. The seam — placements, not callbacks.** The tempting design
was per-consumer shaping hooks; the shipped design is ONE shaped
structure every consumer reads: an array of placements (raster, x,
pen, cluster byte) in visual order, plus the per-codepoint logical
pen attribution. Measure, draw, span draws, wrap, ellipsize, the
Entry caret AND its hit-test all consume it — which is what makes
carets agree with ink and clicks agree with carets, and keeps the
"one rounding rule" invariant intact. The legacy walk emits the
same structure, so the consumers have exactly one code path.

**3. Level-1 scope, honestly fenced.** Each range is its own bidi
paragraph; line breaking stays logical; ligature clusters never
split (advance attributed to the cluster's last codepoint);
embedding controls FriBidi removes are invisible; the Entry caret
is exact for single-direction text and segment-granular for mixed
direction; color emoji stays out (C-3). The fallback chain is two
levels deep (fallbacks never get fallbacks), charset-filtered on a
probe codepoint — FC_LANG alone provably lets a Latin-only font win
the "zh" sort on this very machine — and dies with its primary
font. Synthetic bold's stem is added to shaped advances through the
same shared `fdk__text_bold_stem()` the rasterizer bakes in, so
bold measure and bold ink cannot drift.

The one re-baseline worth recording: the fontconfig e2e discovery
test now runs its crafted-config leg in a re-exec'd child —
fontconfig loads its config exactly once per process, and any
earlier fallback query (shaping made those routine) would otherwise
pin the system config for the whole test binary. The old in-process
form was an artifact of FDK never touching fontconfig before the
resolver; the fork-then-exec form is the honest contract.

## D-011 — CFF outlines: a native Type 2 module, gated ahead of stb

**2026-10-28 (the CFF-outline item / B-02 follow-up — the Arch tofu
report: a machine whose CJK fonts are all CFF-flavored (Noto Sans
CJK) rendered every CJK glyph as a `.notdef` box, because the loader
rejected 'OTTO' outright rather than ship silent tofu — the stance
text.md recorded at 1.0.x. The report's closure is the stance
changing: reject nothing the stack can render honestly.)**

**1. Native, not stb.** The vendored stb_truetype carries a Type 2
interpreter, but (a) it initializes its CFF view with a fabricated
512 MiB table extent (its own `@TODO`), so every operator read past
the real table is bounded only by that lie; (b) it has no CFF2
support; (c) it ASSERTS on malformed INDEX/DICT bytes — point (c)
was found live, not theorized: the new fuzz battery's first run
took `stbtt_InitFont` down through `stbtt__cff_int`'s assert on a
single mutated Private DICT byte. `src/text/cff.c` is an original
implementation from the public Adobe specs (5176, 5177, the CFF2
chapter) with real bounds, a per-glyph operator budget, depth caps
and stack limits; provenance in docs/THIRD-PARTY.md.

**2. The load order is the security posture.** For 'OTTO' faces the
hardened parser runs FIRST; a face whose CFF table fails to parse is
rejected BEFORE stbtt_InitFont ever sees the bytes (stb still owns
the format-agnostic cmap/hmtx/hhea side afterwards). The fuzz
battery that found the assert hazard is the fence that keeps the
order honest.

**3. One seam, one rasterizer.** The module hands glyph geometry
back as `stbtt_vertex` arrays in font units — the SAME public
`stbtt_Rasterize` call draws glyf and CFF outlines, so the flavors
cannot drift in look or metrics. HarfBuzz's extents callback routes
through the native ink box; advances stay with hmtx; maxp
`numGlyphs` must equal the CharStrings INDEX count or the face is
rejected (the OpenType CFF chapter's rule, enforced at load).

**4. The fence is dual-decoder agreement.** For every glyph
examined, the native module and stb's own Type 2 path must emit
byte-identical vertex streams — two independent decodings of the
same charstrings. The development session needed exactly this fence:
the interpreter shipped with its operator table mislabeled (18/19/
20/21), the flex numbers off by one, the subr bias applied
backwards, flex1's vertical end off its return axis, the vh/hv
trailing-arg axis pinned to the operator instead of the group, an
impossible rlinecurve arity, and no implicit contour close — every
one of these was found by diffing vertex streams against stb on
real fonts (Noto CID + URW name-keyed, ~200k glyph interpretations
before the push), and each fix is pinned by tests/test_cff.c. The
seac path (which stb does not implement at all) is pinned by a
crafted font — real foundry fonts no longer carry seac.

**5. CFF2 at the default instance, honestly fenced.** The CFF2
parse is real (4-byte INDEX counts, FDArray required, blend/vsindex
consumed) but renders the DEFAULT instance: blend deltas are
dropped, which is exactly the default-coordinate composition.
Variable-font instances beyond the default stay roadmap material.

---

## D-012 — IME: one event model, the v3 state machine honored, self-heal over protocol blame (M-1 / B-01)

**1. The event surface is two events and a borrowed pointer.**
`FDK_EVENT_IME_PREEDIT` / `FDK_EVENT_IME_COMMIT` with UTF-8 text
owned by FDK for the callback duration only (the drag-payload
rule — IME traffic is transient by nature; nobody needs to own a
composition after the next event revises it). The single-codepoint
compatibility rule: a one-codepoint commit ALSO rides the ordinary
KEY_DOWN codepoint (compose-key consumers keep working untouched),
multi-codepoint commits ride IME_COMMIT alone. Widget-level twins
deliver to the FOCUSED widget like keys; Entry/TextView consume
them (the groundwork preedit API renders composition inline;
commit splices through the single mutation primitive). Custom text
surfaces get the window-level pair — the same seam the drag model
uses for "the toolkit does not own your surface."

**2. The Wayland side honors the v3 batch semantics exactly.**
preedit/commit events LATCH; `done` APPLIES (a pending commit
clears the showing preedit first — v3's replace ordering). Acting
per-event would show preedit the compositor revises in the same
batch. `enter`/`leave` gate enable/disable; surrounding text is
pushed at focus gain and after IME routing (per-keystroke sync is
churn the protocol allows and no consumer needs — the honest
cadence recorded in the header).

**3. The compositor gap is closed CLIENT-SIDE, not blamed.**
wlroots drops the text-input's focused surface when a window dies
WITHOUT sending `leave`; the v3 input stays enabled; and
enable-on-enabled is a state-machine no-op — the enable EVENT
(which is what activates the input method) only fires on a
false→true commit transition. Result: after closing a window, no
IM ever re-activates for the next one. The fix tracks the enabled
session client-side and closes a stale one at the next `enter`
(disable + commit first, then the real enable) — spec-clean,
compositor-agnostic, and exactly what the sway rig now pins: the
deactivate→activate pair after a window switch. The tempting
alternative (destroy + recreate the text_input object on keyboard
focus changes) was built and REJECTED: sway sends wl_keyboard
enter BEFORE the relay's text-input leave/enter, so the recreation
raced the relay and discarded exactly the events it meant to
repair — the probe rig demonstrated the regression live.

**4. The destroy-path guard.** A window dying while holding IME
focus leaves its latched batch pointing at freed memory (a done
arriving after the destroy would dispatch into it) — the Wayland
destroy path now clears the focus and drops the batch. The session
stays marked enabled deliberately: the next enter's self-heal needs
to see it to close it (see 3).

**5. The X11 side asks for callbacks and degrades honestly.**
XIMPreeditCallbacks | XIMStatusNothing first (inline composition —
what CSD toolkits do); the PreeditNothing posture remains the
fallback for input methods that refuse the style (composition is
then unavailable but full-Unicode commits still flow
Xutf8LookupString — the 1.3.1 guarantee). The composition buffer
splice is a PURE function (character indices ↔ byte offsets —
where XIM clients historically corrupt their buffers; it was: the
first run of its own test battery caught the mid-character offset
bug) pinned headless, including the overflow grow-and-retry and
the hostile-draw reset. The XIM callback wiring itself has no
server in the sandbox: code-reviewed, owner-verified on a real
fcitx5 desktop (TESTING-ON-DESKTOP.md §11).

**6. The rig plays the input method.** The sway suite carries a
second wl_display connection binding zwp_input_method_manager_v2 —
the fcitx5 role — driving preedit revisions and commits through
sway's real relay and asserting the full loop: events at the
window callback (the custom-surface seam), the Entry's splice +
on_changed, and the surrounding text + caret arriving BACK at the
input method. The generated input-method-v2 protocol code is
test-only (compiled into the test binary, never the library); the
library ships the client side alone.

---

## D-013 — idle inhibition: one handle, two mechanisms, the deferred release (S-3 / B-11)

**1. The mechanism choice is per-desktop, not per-backend.** One
inhibitor handle picks whatever the desktop actually speaks:
Wayland compositors with `zwp_idle_inhibit_manager_v1` get the
compositor's own (surface-scoped) inhibitor through a new
window-scoped platform op; everything else — X11, Wayland without
the manager — rides `org.freedesktop.ScreenSaver` on the native
D-Bus client. The op answering FDK_ERR_UNSUPPORTED is the fallback
trigger, not an error.

**2. The compositor half is window-owned.** The engagement flag and
the inhibitor's back-reference live on the WINDOW: its destroy path
disengages the platform inhibitor before the platform window dies
(a surface-scoped proxy outliving its surface is a protocol error)
and severs the handle's back-reference — a released-late inhibitor
never touches a dead window. One inhibitor per window (a second
inhibit on an engaged window is FDK_ERR_INVALID_STATE).

**3. The D-Bus half's release is deferred, never cancelled.** The
cookie may arrive after the user released the handle. Cancelling
the pending Inhibit would orphan the inhibition on the desktop (the
call was already sent; only the answer is dropped) — so the release
is remembered and the matching UnInhibit goes out when the cookie
lands. Three reference slots own the memory (user handle, the bus
handshake, the pending call) — the handshake racing a release was a
live use-after-free the design caught before any rig did. The
live-bus mock pins the deferred path as the headline assertion.

**4. Scope fence.** GNOME's org.gnome.ScreenSaver variant (same
Inhibit shape) is deliberately not probed: on GNOME Wayland the
compositor protocol engages, and the X11-on-GNOME user base is
served by the freedesktop service GNOME's session still registers
alongside on modern systems — where it is not, the honest
UNSUPPORTED degradation is the documented answer, not a silent
failure.

## D-014 — the desktop models: protocol extracted from the renderers, one bus connection per model (T-B side 2)

**2026-10-29 (the host-side model split — the roadmap's T-B second
half: trayhost/notifyd carried their protocol and their rendering in
one file each; every future panel would have had to copy the
protocol half. The split is the answer: the protocol lives in the
library, the rendering stays in the examples, and the UNCHANGED rigs
are the proof the split changed nothing observable.)**

**1. Data and events, never pixels.** The two models
(fdk-shell/src/desktop/tray_model.c, notify_model.c;
include/fdk-shell/fdk-shell-desktop.h) own exactly what the roadmap
worded: the watcher service, the item registry, the per-item state,
the dbusmenu client; the Notifications server, the etiquette matrix,
the flood gate, the live-set cap, the expiration timers, the record
store. Their public surface is snapshots (struct_size structs with
caller buffers), one preorder menu walk, and action entry points the
UI forwards (Activate/SecondaryActivate/Scroll, menu
open/close/event, action_invoke/close). No surface, font, or pixel
type appears in the header — markup translation, icon-file/theme
resolution, and card/bar drawing stayed with the reference UIs,
which is what "thin" means here.

**2. One model per bus connection — and create is asynchronous about
the name.** A model owns its connection's single method handler and
its NameAcquired/NameLost events, so a second model on one
connection is the documented misuse. Name acquisition is protocol-
inherently asynchronous (RequestName's roundtrip), so create returns
after installing the surface and the outcome arrives as the
HOST_STATE/SERVER_STATE event with serving() as the query — a
synchronous FDK_ERR_EXISTS would have been a lie the bus can charge
for later. The etiquette matrix itself is the reference's verbatim:
refuse / --replace (REPLACE_EXISTING) / --wait (queue), the
self-registration as host over the real bus path, and — the leg the
split initially lost — the RequestName EXISTS reply must reach the
app as a refusal (the rig's squat-probe leg hung on a model that
ignored its own RequestName replies), and a refusal makes the model
DORMANT: it never grabs the name when the incumbent vacates (the
queue-lurker vandal posture the test pins).

**3. The rigs were the spec, and the trace channel moved house.**
Both rigs ran UNCHANGED against the split (notifyd 68, trayhost 103
— all three phases each: protocol with cross-implementation callers,
X11 pixels + XTEST, sway layer-shell). Where the rigs grep the host
log for protocol transactions (GetAll, GetLayout prefetch, nodes<=N,
menu labels, AboutToShow, the refusal lines), the models emit the
SAME lines on the SAME channel (stderr under FDK_TRAYHOST_TRACE /
FDK_NOTIFYD_TRACE): logging is not drawing, and the rigs' vocabulary
is now the models' contract. The new tests/test_desktop_model.c
(test-desktop) drives the models headlessly on a private
dbus-run-session bus with an in-test mock item and client — the
snapshot fields byte-exact, the etiquette matrix including
dormant-after-vacate, the deferred-release-adjacent refusal
split (incumbent-watcher vs RequestName-EXISTS, distinguished by the
app's own posture flags since the event is deliberately coarse).

**4. The decode is the flat form first.** The live-debugging lesson
that cost the most pixels: 'ay' arrays arrive on the wire as the
value model's FLAT bulk form — a tree-form per-index read returns
nothing there, which renders a fully transparent icon that every
pixel check sees as "no icon". Every byte-array decode in both
models (SNI pixmaps, image-data) reads flat-first, tree-second, and
refuses non-"y" element signatures (reading ints as bytes is the
silent all-zero icon again). The image-data field order is the
spec's SEVEN fields with the data at index 6 — an off-by-one there
is the same invisible-icon symptom.

## D-015 — X11 multi-monitor: the add-on's own X connection for output events, the core's table for placement (M-6 / A-03 / B-05)

**2026-10-29 (the last MUST item before the release gate: X11 knew
exactly one output — the whole screen — so per-monitor docks were
impossible and maximize filled the screen regardless of which
monitor held the window. The split that closes it follows the
existing architecture instead of inventing a core output API.)**

**1. The core's table is internal, like Wayland's.** FDK's Wayland
backend tracks outputs without an app-facing output API; the X11
backend now has the same shape: `src/platform/x11/x11_randr.c`
probes per-CRTC geometry at connect (XRRGetScreenResourcesCurrent,
sorted into reading order), RRNotify-masked on the root, refreshed
in the dispatch loop. Its consumer is window placement — the
bare-X maximize/fullscreen fallbacks fill the CONTAINING monitor
(the window's center), not the screen. No public API appears: the
append-only discipline would allow one, but parity with the Wayland
core's posture is the honest architecture (an app needing outputs
binds fdk-shell, exactly as the audit's fix wording assumed).

**2. fdk-shell owns a SECOND X connection for the events.** The
extension boundary has no generic X-event hook, and it should not
grow one: on Wayland fdk-shell binds its own wl_registry and gets
output events on its own protocol objects — the X11 twin is
XOpenDisplay(the same server) + XRRSelectInput + the extension
loop's fd source (the documented add-on-socket pattern). The
output table, the ADDED/CHANGED/REMOVED diffing, the gone-records
enumeration contract, and per-output layer anchoring (output-local
geometry offset into root coordinates) all live add-on-side. Two
probes on one server is one round trip each; the boundary stays
clean.

**3. The fallback is the regression pin.** No RandR (or a failed
own connection) = ONE output, the whole screen, real metrics, no
events — byte-identical to the pre-M-6 behavior, so every
single-monitor rig (the bar rig, both desktop rigs, the whole X11
suite) doubles as the no-regression proof. Xvfb reports one fixed
output ("screen"), which pins the probed-table identity; the
multi-output ORIGIN math runs through an internal test seam that
injects a synthetic two-monitor table (window geometry read back
through the protocol — the same end-to-end standard, minus the
second physical monitor). True output hotplug (ADDED/REMOVED,
per-monitor maximize on real hardware) is the owner's
TESTING-ON-DESKTOP step.

**4. Struts stay root-relative — the honest X11 residue.**
_NET_WM_STRUT_PARTIAL is screen-global by specification; an
exclusive-zone layer on a secondary monitor reserves space the way
the running WM interprets per-monitor struts (modern WMs clamp to
the window's monitor; simpler ones apply it screen-wide). That
limit is documented, not faked — the one place the X11 and Wayland
output stories legitimately diverge.

## D-016 — the cursor set: one resolution point, themes through RENDER by dlopen, the honest glyph substitutions (S-5 / B-12)

**Context.** The audit left the public cursor surface at exactly
two shapes (arrow, hand) while GTK4/Qt6/CSS all ship the full named
set — a panel cannot say "wait" while its app list loads, an image
editor cannot say "crosshair", a drag source cannot say "grab".
The rc1 hand was already wired through both backends' apply paths;
the question was how to grow the SET without growing the
dependencies or lying about shapes the platform cannot draw.

**1. One decision point, restructured.** The rc1 code had the
application request short-circuit the compass inside
window_update_cursor, with bookkeeping spread across three sites.
rc2 collapses the WHO-decides into a pure function —
`fdk__cursor_resolve(app, widget_hover, compass)` — with one
speak-path (`window_apply_cursor`) that ships only TRANSITIONS
(cursor_applied caches the last op sent). The priority: an explicit
application request owns the window; a widget hover shape (the
Entry/TextView/Spin I-beam, derived from root->hovered's class
pointer — the IME seam's identity discipline) rides below it; the
compass last. The headless suite pins every row of the table.

**2. X11 themes through RENDER, dlopen'd.** The core cursor font
has no copy/alias/zoom/context-menu glyphs at all, and its
GRABBING is the POINTING hand. Rather than adding libXcursor (a
new build+CI dependency for one call), the X11 backend dlopens
libXrender.so.1 (the house pattern — the GL lane, HarfBuzz,
FriBidi, libXi all went this way) and builds ARGB cursors from the
SHARED XCursor theme loader — the same no-libxcursor parser the
Wayland backend earned against Adwaita in 1.1.4, extracted to
src/platform/cursor_theme.c when X11 needed it. Which shapes try
the theme first is table-driven; shapes with exact font twins skip
the theme entirely (faster, and pixel-identical to the desktop).

**3. The substitutions are named, never silent.** Where both tiers
fail (no theme anywhere, no RENDER) the shape keeps the default
arrow — the no-lying-fallback posture the whole toolkit holds: a
cursor that misadvertises what the press will do is worse than the
plain arrow. The substitution table lives in docs/platform-input.md
and in x11_cursor.c's own comments; XC_X_cursor's VALUE being
literally 0 forced the -1 no-twin sentinel (the naive 0-sentinel
silently dropped NOT_ALLOWED — the rig caught it).

**4. The tests ride the real paths.** Headless: the mapping, the
resolution order, the name-table grammar, and the loader against a
synthetic XCursor file (the documented format written by hand —
happy path, candidate walk, hostile refusals, closest-size pick).
X11: all 22 shapes applied; the no-twin five built through RENDER
from a hermetic fixture theme (Xvfb always has RENDER — no skip
logic); the cache discipline; the Entry I-beam + priority order
through real XSendEvent motion. Wayland: all 22 shapes through the
production op with real injector serials, plus the full
cursor-shape-v1 enum table pinned row by row through the
test seam — real set_shape requests against sway 1.10.

## D-017 — input regions: logical rects at the window layer, the platform's native shape beneath (S-4 / A-06 / A-07)

**Context.** The 1.3.2 input_transparent popups proved the
whole-window degenerate case (an EMPTY input region); the audit's
A-06 gap was arbitrary partial regions — what notification
overlays with close buttons, auto-hide reveal strips (C-5's
prerequisite), and shaped panels need. D-002 had already decided
the seam: core-side, on windows AND extension surfaces, not an
ext-API version bump.

**1. Logical coordinates, one space.** The public API takes
window-LOCAL LOGICAL rectangles — the same space pointer events
deliver — and each backend converts once: X11 multiplies by the
connection's HiDPI factor into XShape's physical space (the
conversion ConfigureNotify applies in reverse); Wayland passes the
rects straight through, because wl_surface.set_input_region's
surface-local space IS the logical space (with integer buffer
scales and fractional viewports alike). No region re-application
machinery on resize: XShape regions are window-relative and
stable, and the Wayland region rides surface state that scale
changes do not move.

**2. The clear is a full-window rect, not a removal.** libXext has
no remove-the-shape idiom that does not dereference a NULL Region —
XShapeCombineRegion(display, w, ShapeInput, 0, 0, None, ShapeSet)
SEGVs (the XTEST rig caught it in the first cut). The clear
therefore SETS one full-window rect: behaviorally identical to no
shape (input everywhere the window draws), and protocol-observable
on readback. The zero-area rect stays the honest "empty region"
(fully click-through — input_transparent on demand).

**3. Commits stay gated.** Wayland region state is
double-buffered: the op commits immediately when the surface is
already configured (so a region change takes effect without
waiting for the app's next present) and rides the FIRST commit
otherwise — never a bare commit on an unconfigured surface (the
unconfigured-commit protocol error the ext-popup gate exists to
prevent).

**4. One proof per platform, at the wire.** X11: the region read
back through XShapeGetRectangles from a second connection (1 rect,
2 rects, empty, clear) and the click-through behavior driven with
REAL XTEST pointer events — a press in the hole lands on the
window beneath, a press in the region lands on top (XSendEvent
bypasses shape hit-testing, so only the true input path proves the
mechanism). Wayland: the injector's real pointer — no enter into
the hole, enter into the region, leave on exit, full input after
the clear. The add-on twin (fdk_ext_surface_set_input_region) is
pinned by fdk-shell's X11 suite through the same readback.

## D-018 — the calendar family: no clock reads, Monday-lead weeks, six rows always, pickers that do not parse (S-10 / B-18)

2026-10-29. The widget-set residue item: everything on the GTK4
checklist had an FDK counterpart except a date/time picker. The
close is three widgets in src/widget/calendar.c — fdk_calendar (the
month grid), fdk_date_picker (a formatted field + popup calendar),
fdk_time_picker (a segmented stepper) — riding the i18n module's
validated proleptic-Gregorian math for every date operation.

**1. The widgets NEVER read the wall clock or the environment.**
This is the i18n posture ("FDK formats the calendar numbers it is
given") applied to widget state: "today" is app-injected
(fdk_calendar_set_today), a date picker starts DATELESS (an em dash
until set), a time picker starts at 00:00:00. The alternative —
GtkCalendar's auto-today — would put a hidden clock read into the
toolkit's state, exactly the class of ambient-environment
dependence the locale system was designed to avoid. The fence is
named in fdk_widgets.h.

**2. Monday-lead weeks (ISO 8601).** The rules table carries no
CLDR first-day field, and fdk_date_weekday's 0 = Monday numbering
is the toolkit's established week arithmetic — so the grid leads
Monday and the weekday header reads days_abbr[0..6] in ISO order.
A first_day rules column is the honest future extension (US
Sunday-lead); until it exists the ISO default is the documented
behavior, not an ambient locale read.

**3. Six week rows, always.** A month spans 4..6 ISO weeks; the
grid lays out 6 unconditionally (GtkCalendar's discipline) so the
widget's measured size never changes as the month turns — layout
stability over two rows of occasional emptiness. Leading and
trailing cells carry the adjacent months' days in the disabled
color; selecting one (click or keyboard walk across the edge) both
selects the date and moves the browse month. The grid "scrolls."

**4. The pickers do not parse text (level 1).** There is no
fdk_format_parse; inventing a per-locale date parser inside a
widget would be the toolkit's worst kind of hidden magic. The date
field opens on press and the keyboard openers (the combo's set);
the time picker has no text mode at all. The fence is named in the
header; a future parse API is the level-2 path.

**5. The time picker renders latn digits.** The date/time
formatters emit latn numerals today (the digit-substitution tables
drive the NUMBER formatters); the segments match that output so a
time picker next to an fdk_format_time label never mixes digit
systems. If date formatting ever grows digit substitution, the
segments follow it in the same change.

**6. The popup is the combo's discipline, minus the menu.** One
toolkit-owned popup window (fdk_window_create_popup + auto-paint +
destroy-notify) hosting one calendar; CLOSE_REQUEST (the grab's
outside-click translation) and Esc (the window layer's universal
popup dismissal) close it; the combo_guard pattern covers a picker
destroyed while open. Picking applies, formats the field, fires
on_changed once, and closes — the app pumps events and never
learns the popup exists.
