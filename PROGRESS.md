# FDK — Progress Ledger (READ THIS FIRST)

Last audited: 2026-10-30, at milestone **1.7.7** (D-3 tablets/stylus,
both backends: FDK_EVENT_TABLET_* (enum 52-57) + fdk_tablet_event —
Wayland zwp_tablet_v2 (vendored generated protocol code) + the X11
valuator path through dlopen'd libXi (the XIQueryDevice scan); the
frame batching absorbed, the XI2-no-proximity gap documented (D-030);
855 exports unchanged; the full battery green incl. both suites' new
seam-driven legs. Prior state below.)

Last audited: 2026-10-30, at milestone **1.7.6** (D-2 subsurfaces,
extension API v2: `fdk_ext_subsurface_create`/`_create_ex` +
set_position/resize/set_desync/place_above/place_below — the
FDK-owned child-surface primitive, X11 child windows + Wayland
wl_subsurface DESYNC, the parent-managed tree-state rule absorbed
as the empty applying ancestor commit (DECISIONS D-029, the sway
rig's first-run lesson); 855 exports (+7); the full local battery
green incl. the new X11 leg and the sway suite's subsurface
section. Prior state below.)

**This file is the anti-duplication contract.** Before implementing
*anything*, check the "Implemented" inventory below and grep the public
headers — if a feature is listed here as SHIPPED, it exists in the tree;
do NOT re-implement, re-add files, or duplicate APIs for it. After
shipping any new feature, update this file **in the same commit**.
Full narrative history lives in `docs/roadmap.md` (the phase-by-phase
account and the GTK/Qt feature-parity ledger); this file is the fast,
authoritative index of that ledger.

Last audited: 2026-10-25, at milestone **1.7.2** (the rc1
packaging matrix's first live CI pass — the 1.7.1 commits were
pushed and four of the six CI jobs failed, every defect a
packaging-script defect, none in library code: the fdk-shell
install-test smoke needs X11 DEV headers as test infrastructure
[libx11-dev/libX11-devel now pre-installed by the deb/rpm
install-tests, the class of gcc/pkg-config/xvfb — the .pc chain
deliberately carries no x11 Requires, and CI's own deb BUILD
container had proven the identical smoke green where the headers
existed]; FDK's own rpm install-test selected `dist/*.rpm`, so the
add-on's packages swept into a test whose removal step was then
CORRECTLY refused by rpm — a working dependency blocking removal
of its base [now FDK's own rpms by name; arch's sort-order
`head -1` landmine made the deterministic `fdk-[0-9]*` glob]; the
fdk-shell apk build hand-fed a bare sha512 into sha512sums, but
abuild's verify needs the FILENAME in the array [now FDK's proven
`abuild checksum` sequence; the rewrite also fixed REPODEST
artifact collection and dropped the `abuild -r -F` cargo cult];
the fdk-shell xbps build checked dist/ while xbps-create wrote
into the CWD [the `mv ./*.xbps dist/` line restored]. Audited
alongside: the fdk-shell distro builds had copied FDK's
LSAN_OPTIONS=exitcode=0 leak policy without its comment [restored
with the observed numbers; the inert alpine/void copies removed —
those images run no sanitizer toolchain], and the rpm/alpine/void
install-tests' `fdk_start_xvfb || exit 1` turned the el10
honest-skip into a hard failure [now the helper's disclosed-skip
if/else contract, the same shape as FDK's own install-tests].
A second push exposed the fifth and deepest class: the GENERATED
fdk-shell.pc was wrong twice over — its Libs line carried the
library's own platform link deps (-lX11 -lwayland-client, forcing
every consumer to install the X11+Wayland dev packages and baking
build-machine -L paths into the artifact; the deb/rpm smokes died
at 'ld: cannot find -lwayland-client') and it referenced
${libdir}/${includedir} without defining them [undefined .pc
variables expand empty — it only worked because /usr/include and
the default linker path need no flags]. The .pc now mirrors
fdk.pc's contract [variables defined; Libs: -lfdk-shell +
Requires: fdk; platform flags in Libs.private], the smoke's own
Xlib readback carries -lX11 on its link line [modern ld refuses
transitive resolution: 'DSO missing from command line', reproduced
locally], and pkg-config discovery failures now print the real
diagnostic [2>/dev/null swallowed the void failure's evidence;
pkgconf 3.0.7 was built from source locally and PROVEN to accept
the old bytes, so the void cause is pending its diagnostic].
The third push closed void: the corrected .pc made the build-phase
smoke discover/link the installed fdk-shell (the old 'cannot find'
never recurred — the diagnostic never fired), and the remaining
failure was the void install-test's missing RUNTIME shlibs [xbps
resolves declared shlib-requires only against installed providers —
the combined-set transaction aborted on fdk-devel's libXext.so.6 /
libxkbcommon.so.0; now pre-installs the same hermetic shlib lines
FDK's own void install-test carries]. Proven locally: a mini
installed-prefix + the exact clean-container compile line = SMOKE
OK; the isolation proof (links the demo through the .pc alone)
still green. docs/testing.md's 1.1.0-rc1
packaging-matrix lesson + CHANGELOG rc1 Fixed section carry the
full narratives. Prior state below.)

Last audited: 2026-10-27, at milestone **1.7.5** (the owner-directed
verification pass + the CI-red day: the 0.2.0 packaging red's root
cause was fdk-shell's Makefile carrying its own hand-maintained
0.1.0 next to the header's 0.2.0 — every install-test caught the
staged .so.0.1.0 / Version: 0.1.0; the Makefile now derives the
version from the header like FDK's own [4d86464]; the alpine flake
was a present-then-readback RACE — readbacks ride a second X
connection and can beat the just-flushed frame, reading the empty
pre-paint entry — x11_frame_settle now guards all 18 unguarded
paint-then-readback sites [1373329]; the void reposync outage got
real mirror rotation before every remote install [c42418d]; the
release branch policy landed: a fast-forward-only consumer ref at
the v1.0.0 commit, RELEASING.md, CI on both branches, README +
packaging pointers [c8568ef]; and the alt/dbus-client test port
closed three genuinely-unique coverage gaps and found TWO real
accessor defects — fdk_dbus_message_signature and read_args were
builder-blind for locally built messages — both fixed at the root,
plus the FDK_LOG_TAG clobber in dbus_internal.h [ccafa7c]; DECISIONS
D-004/D-005 record the reconciliation and the core/add-on split;
ROADMAP-1.1.0 gained the verified-status table [this commit]. Prior
state below.)

Last audited: 2026-10-26, at milestone **1.7.4** (the push of the
1.7.3 fixes + the sixth CI defect class it exposed: the live-desktop
commits landed on origin [three commits, core 1041 lines / fdk-shell
947 / docs 247] and five of six CI jobs went red at the identical
line — `tests/test_x11.c: fatal error:
X11/extensions/XTest.h: No such file or directory`. Root cause: the
new passive-grab passthrough test synthesizes Super+number keys
through the XTEST extension, and the Xtst HEADER ships only with
each distro's dev package [libxtst-dev deb/apk, libXtst-devel
rpm/void, libxtst arch] — which none of the six build environments
pre-installed; the rpm job "passed" only through its disclosed el10
Xvfb skip [the suite build is Xvfb-gated there, so the missing
header was never compiled — a skip, not a pass, read at the JOB
level as always]. Fix: the dev package declared in all six install
lists [ci.yml's ubuntu apt + each family's build container], the
first matrix lesson's test-infrastructure class applied to the
builders themselves; the Makefile's XTest comment now names the
per-family packages and why there is no XWarpPointer dodge [a
passive grab activates only on synthesized KEY input — XSendEvent
delivery cannot]. The dependency is honest and permanent: the core
suite's XWarpPointer/XSendEvent tricks cannot verify that the WM's
Super bindings survive an open popup. The same run's void job died
one step earlier — a reposync connection timeout on the round-robin
mirror [an outage, not a code defect; the code was green on the five
other families] — so the critical xbps line now retries once after
10s, the tolerance the later lines already carried. Docs:
testing.md's sixth-class lesson, CHANGELOG's matrix section
[now "six defect classes"], fdk-shell/Makefile. No tags, no
releases; 1.0.0 untouched. Prior state below.)

Last audited: 2026-10-26, at milestone **1.7.3** (the live-desktop
bar findings — sway/labwc + Openbox, all four reproduced in this
environment BEFORE fixing: [1] the Wayland layer-surface startup
sequence — the demo bar painted before its role's first configure
and wlroots killed the connection ["layer_surface has never been
configured"]; extension surfaces now ride FDK's first-configure
present gate, with notify_configure committing the deferred frame
post-ack — pinned by the eager-paint test, the bar rig's
WAYLAND_DEBUG trace assertions [no attach before the first
ack_configure, no wl_display.error], and alive/pixels legs against
BOTH headless sway AND labwc 0.8.3; [2] the fatal-dispatch
busy-loop — 2.2M identical log lines at 100% CPU after the
connection died; the core now logs a fatal dispatch once per code
and identical repeats sleep out the caller's remaining timeout, the
bar exits non-zero on a fatal pump, and the Wayland death line
carries the protocol error's interface/id/code — pinned by the bar
rig's compositor-kill leg; [3] the X11 popup keyboard grab vs the
WM's passive grabs — Super+number workspace switching died while a
dropdown was open [reproduced against real Openbox under Xvfb];
popups now grab POINTER-ONLY and route keys through saved+restored
XSetInputFocus, with explicit grab-result handling, dismissal
reasons, a not-viewable watchdog, and desktop-switch dismissal of
open popups — pinned by the in-test EWMH WM's passive-grab rig
[XTest Super+1 received by the WM while the popup is open and after
dismissal], the popup pixel-stays test, and the desktop-change
test; the GPU self-check retry budget became timer-based [fired
from the dispatch tail; the pump wakes via the X11 backend's new
next_wakeup_ms — both existing self-check tests still pass]; [4]
fdk-shell's layer windows now set USPosition/USSize hints [an
unpositioned dock is "unplaced" — Openbox centered the panel
mid-screen; confirmed in its debug log] and popups raise on map;
FDK_POPUP_TRACE=1 documents the popup lifecycle with reasons and
grab-held durations. make test, test-x11 [incl. both self-check
tests], FDK_SANITIZE=0 test, release+verify-exports [641],
examples, fdk-shell's 11-test X11 suite [twice], the Wayland suite
+ bar rig [sway+labwc+kill legs], and the isolation proof all
green. Prior state below.)

Last audited: 2026-10-25, at milestone **1.7.1** (the rc1
desktop-integration hardening, found live on a real NVIDIA GTX
1050 Ti desktop: the GPU present lane's self-check now decides
INCONCLUSIVE vs PASS vs REAL-FAILURE per connection with a
present-latency retry budget [not-viewable windows and XGetImage
BadMatch never downgrade; the verdict is process-wide and logged
once — FDK_GL_SELFCHECK_POISON is the test seam]; fdk-shell's X11
backend is sticky across workspaces [_NET_WM_DESKTOP 0xFFFFFFFF +
_NET_WM_STATE_STICKY, pre-map properties + post-map client
messages] with per-level stacking state [ABOVE for TOP/OVERLAY,
SKIP_* for OVERLAY; the fullscreen-over-docks EWMH ceiling and the
custom-WM contract documented in fdk-shell's README]; the demo bar
repaints on EXPOSE [the ghost-content fix]; fdk-shell's shared
library carries NEEDED libfdk.so.1 and its release config drops
sanitizers; fdk-shell is PACKAGED in all five families with a real
versioned FDK dependency [fdk-shell/packaging/, the install-tests
prove the dependency is enforced by each native package manager,
and every build compiles against the INSTALLED FDK — the isolation
proof re-run per packaging container; CI's ubuntu job runs the
fdk-shell suites + the isolation proof]. Prior state below.)

Last audited: 2026-10-25, at milestone **1.7.0** (the extension API
+ fdk-shell — the 1.1.0-rc1 candidate: the owner directed FDK to
grow a public extension API ["add an extension API to FDK so
add-on packages can be built on top of it, and prove it by building
a layer-shell add-on as a SEPARATE package that uses only that
public API"] and set the version to 1.1.0-rc1 [minor, not 1.0.1,
because public API is added; a CANDIDATE — no git tag, the v1.0.0
release and its tag untouched, the owner decides the final 1.1.0].
What landed: include/fdk/fdk_extend.h [the versioned extension API
— surface kinds with create/destroy/popup_attached callbacks whose
create builds the native surface through the documented
native-display accessor and FDK ADOPTS it into the full
event/render/popup machinery; loop fd sources in the pump's poll
set with the timer fire discipline; the public fdk_surface render
path + native display/handle accessors; popups on extension
surfaces with the zwlr get_popup hook on Wayland; backend +
capability queries; docs/EXTENDING.md is the contract narrative];
src/window/extend.c + src/core/extfd.c + the ext_surface_adopt /
ext_surface_native / ext_surface_resized / native_display platform
ops wired into BOTH backends; 20 new exported symbols, 641 total,
SONAME unchanged (append-only). fdk-shell 0.1.0 [the first add-on
package: fdk-shell/ — own build, own version, own pc file, ready to
move repo; wlr-layer-shell v4 via vendored scanner bindings on
Wayland, _NET_WM_WINDOW_TYPE_DOCK + _NET_WM_STRUT_PARTIAL on
X11/XLibre, the XOR-anchoring zone math, popups, the bar demo;
tests/isolation.sh is the prefix-isolation PROOF]. Tests:
test_extend.c headless + five X11-integration sections + the two
fdk-shell suites, all ASan/UBSan green; two latent FDK bugs found
by the rigs on the way (uninitialized fullscreen/geo_repaint fields
on the create path; the wayland NULL-proxy destroy + the ext-popup
handshake commit). CHANGELOG.md created [1.0.0 + the candidate].
2026-10-25, later (push addendum): the candidate was pushed to the
remote, which had just been transferred to the Faded-Dream-Project
organization — canonical repository URLs across the docs and every
package's metadata now point at github.com/Faded-Dream-Project/FDK
(GitHub redirects the old FemBoyGamerTechGuy/FDK locations; the
LICENSE's Licensor and the packages' Maintainer/copyright identity
lines are the owner's person and deliberately unchanged). The first
CI run on the candidate failed four of five packaging jobs on the
rc version string — rpm/apk/arch reject the hyphen, xbps's
name-version split went ambiguous, deb ordered the rc above the
future final — fixed by fdk_packver (packaging/ci/common.sh): the
per-family pre-release spelling at the metadata layer only, raw
string everywhere the library reports itself. The second push
then failed ALPINE at the repository index with "package file
format error": abuild derives the apk so:provides version from
the .so FILENAME tail (sover=${file##*.so.}), so the raw-named
libfdk.so.1.1.0-rc1 produced provides = so:libfdk.so.1=1.1.0-rc1
which apk index rejects (reproduced locally with apk-tools
2.14.12 — even hyphenless 1.1.0rc1 is rejected; only numeric
tails are valid) — fixed at the root: the Makefile now installs
the real .so as the numeric FDK_SO_VERSION (libfdk.so.1.1.0;
suffixes never appear in the .so file name), and the debian/void
packaging scripts follow via fdk_sover(). The third push then
exposed an independent 1-in-400 CI flake: the HiDPI test's private
192-dpi Xvfb hard-coded :98 — inside the make test-x11 harness's
own display range (90 + pid%400) — and on collision it unlinked
the SUITE server's socket, probed the 96-dpi suite server via the
TCP fallback, and aborted the debian job on the scale assertion;
the private display now lives below every harness range, is checked
free first, and is verified ours by its 800x600 geometry (proven
both ways locally, docs/testing.md 1.1.0-rc1 lesson). See
CHANGELOG's rc1 Fixed section and docs/packaging.md's
pre-release-versions section.

Prior audit at milestone **1.6.1** (the one-face
alignment — the 1.0.0 re-cut: the owner inspected the live v1.0.0
release and set the product posture ["it needs to ship only one the
next ones must be in (.FDKthemes) btw the default one being
Faded-dream"] — FDK ships EXACTLY ONE theme, `faded-dream.fdk`, the
packaged copy of the built-in default/fallback; the next faces are
.FDKThemes drop-ins the user installs, never package content;
the Makefile's SHIPPED_THEMES is PINNED to that single file [the
wildcard is gone — a stray .fdk can no longer ride into a release],
the source tree's `.FDKThemes/` carries only the one face, and the
nine faces FDK grew between 1.4.13 and 1.4.17 are preserved as
engine fixtures in `tests/data/faces/` [never installed:
test_shipped_themes.c pins BOTH the one-face shipped contract —
opendir scan of `.FDKThemes/` included — AND the nine-fixture
corpus with every color/metric pin carried over; the X11
titlebar-theming rig loads mono-chromatic + pink-rave from the
fixtures; example 05 cycles the built-in + two examples/data
fixtures + the one shipped face]; README, docs/build.md,
docs/cli.md, docs/fdk-theme-format.md, docs/packaging.md,
include/fdk/fdk_theme.h, the tools' usage lines, and
packaging/ci/release-notes.sh all record the one-face posture; the
v1.0.0 tag was re-created on this commit and the release assets
replaced on the main repository), on top of the prior audit at
milestone **1.6.0** (the public 1.0.0
decision — the real release: the project owner, having validated
the 1.5.x picker overhaul on real hardware [native Wayland under
labwc included — "works native wayland", the FDK-drawn decorations
carrying move/resize without compositor shortcuts], explicitly
demanded the release ["i want a perfect release and the release
needs to be to the main release FDK not FDK test"], which is
exactly the explicit-owner-decision input docs/versioning.md's
enforcement section requires; the decision was recorded there
FIRST per policy, then fdk_version.h moved to 1.0.0 with the
test_core pin following in the same commit; the versioned-SONAME
scheme re-activated in the Makefile [-Wl,-soname,libfdk.so.1 on
the link line; install stages libfdk.so.1.0.0 + libfdk.so.1 SONAME
symlink + libfdk.so dev symlink, uninstall removes all three];
README badge + status, abi-policy SONAME paragraphs, packaging.md
banner, roadmap convention note all synced; the v1.0.0 tag was
re-created on the decision commit and pushed to the MAIN
repository, release.yml's matrix building all five families —
deb, rpm, pkg.tar.zst, apk, xbps — plus the pristine source
archive into the release; the "success but no packages" mystery
the owner reported was root-caused on the way: since the
2026-10-11 retraction every packaging job had been exiting 0 via
the disclosed PACKAGING-SKIP guard [green checkmark, zero
packages], and the Void family's CI-env failure the owner
remembered was from the retracted attempt — its fixes landed
afterward and were proven green in the matrix before this
release), building on **1.5.4** (the picker's
crash fix — the maintainer's fourth report ["it crashes anytime I
try to open anything that has to do with the filepicker", an ASan
heap-buffer-overflow inside nsvg__parseNameValue] resolved at the
root, not the symptom: the vendored upstream nanosvg PARSES CSS
<style>+class= rules — the repo's own docs had wrongly claimed it
did not, and that misdocumented blind spot is precisely how an
upstream one-byte OOB read shipped for two milestones — and the
name right-trim dereferenced *str with str == end, one byte past
the NUL of the strndup'ed class-properties text, whenever a CSS
declaration block ends in ';' + trailing whitespace (the standard
multiline rule every real theme authors, so EVERY dialog open on
a class-using theme aborted; 1.5.3's live XSETTINGS detection had
just made FDK finally select those real themes, which is why the
crash surfaced exactly then); fixed with a marked one-function
bounds patch in the vendored header [trim by str[-1] — identical
output for well-formed declarations], the vendor README's
provenance now recording the local diff against the upstream
sums, and the stale capability claims corrected in fdk_dialog.h /
icon_svg.c / roadmap; regression-pinned THREE ways: a headless
class-fixture group [eight shapes incl. both overflow forms and
positive fill-application pins], an X11 dialog round where a
class-only theme's fills are read back from the X SERVER through
the real open-file dialog [the maintainer's exact crash flow as a
permanent assertion], and the full headless + X11 suites green;
honest fidelity boundary documented [class rules with
presentation properties apply; color:/currentColor-only rules
leave the default fill]; public API and its 621 exports
unchanged), building on **1.5.3** (the picker's
polish pass — the maintainer's third report ["black boxes or bars
around folder icons, worst in the grid, present in the list; the
active desktop theme still not detected without an override; the
sidebar too"] resolved into two root causes, both fixed internally
[public API and its 621 exports unchanged]: (1) icon COMPOSITING
violated the blit contract on the exact-size fast paths
[fdk_surface_blit_transformed's identity and integer-translation
paths routed ARGB8888 icon sources through fdk_surface_blit — the
documented OPAQUE raw copy — so every icon decoded at exactly its
cell size (16-px list rows, 48-px grid cells, the sidebar's places)
painted its transparent margins as opaque BLACK; scaled blits
composited correctly, which is exactly why 1.5.2's verifications
missed it — the fixture icons were full-bleed squares and the
template matches sampled only the reference's opaque pixels; now
ARGB sources composite source-over on both fast paths, XRGB sources
keep the raw copy; the same defect class swept through every
consumer of the primitive: the About dialog's logo (its own plain-
blit call site) and the public Picture widget (inherits the fix)];
regression guards: a render-level transparent-margin test at
identity AND translation, transparent-margined fixture icons +
margin ring checks in the picker rigs, and the SVG grid proof now
sampling the cairosvg reference's TRANSPARENT region; (2) theme
detection now hears the LIVE desktop through XSETTINGS
[Net/IconThemeName read from the settings manager via one
short-lived X connection — pure Xlib, no D-Bus, the no-bus policy
intact; the channel the desktop's own GTK apps obey on X11/XLibre:
xfsettingsd, gsd-under-XWayland, mate/cinnamon, xsettingsd; the
wire format decoded byte-for-byte from a REAL xsettingsd 1.0.2 and
the reader verified end-to-end against that daemon including a
live theme change; precedence: FDK_ICON_THEME > XSETTINGS > user
gtk ini > xfconf xsettings.xml > kdeglobals > classic icon-theme
file > SYSTEM gtk ini (XDG_CONFIG_DIRS + /etc + /usr/share) >
default/index.theme > hicolor; pure-Wayland sessions have no
manager and fall to the file channels — proven under sway with
DISPLAY unset]; tests: headless detection groups for the new file
channels, an X11 XSETTINGS group with an in-repo spec-conformant
fake manager (both byte orders, int/string/color skip, precedence,
fallbacks), and both picker rigs re-verified pixel-level + VLM on
X11 AND Wayland), building on **1.5.2** (the exhaustive
pre-1.0 audit pass — the maintainer's second desktop report
["only recognizable folder icons; text files indistinguishable"]
traced to THREE root causes, all fixed internally [public API and
its 621 exports unchanged]: (1) theme detection missed the system
default channel [the icon search path's own default/index.theme
Inherits= line — the channel stock GNOME/XFCE systems publish; the
engine fell to hicolor on stock systems and every file type dropped
to the procedural glyphs, the reported symptom exactly], (2) PNG-
only lookups missed every SVG-only size [Adwaita's entire 48-px
scalable tree — now rasterized at the REQUESTED size through the
vendored nanosvg (third_party/nanosvg/, zlib license, the stb
precedent; librsvg rejected with its Cairo/GLib stack), same-dir
PNG preferred, undrawable SVGs are honest misses], (3) a broken
asset could shadow the whole chain behind it [the new decode-
refusal set: exists-but-will-not-raster paths are refused, probes
skip them, the search restarts — the shadowing regression test
pins an empty SVG in the own theme losing to the parent's valid
PNG]; verification is pixel-level AND independent [cairosvg/cairo
renders the reference from Adwaita's own SVG; the screenshot grid
must match within AA tolerance; VLM review of both views: "no
defects found"]; the audit also fixed make install PREFIX=X
installing a stale fdk.pc [prefix-stamped regeneration; caught by
the new external-application test — a standalone app compiled
through pkg-config against the INSTALLED tree, run under Xvfb with
the file dialog + icon engine live]; repo-wide TODO/stub/no-op
sweep clean, both-backend batteries green, sway/X11 example rigs
refreshed for the 1.4.16 theme colors and 1.4.17 scrolled catalog;
full narrative: docs/roadmap.md "1.5.2"), building on **1.5.1**
(the picker
stabilization — the file/folder pickers wear the desktop's icon
theme through a new internal freedesktop icon-theme engine
[src/theme/icon_theme.c: XDG discovery, index.theme parsing with
real-theme directory subpaths, inheritance, size matching, PNG-only
raster, symbolic tinting, 3-layer caches reset per dialog]; List and
IconView grew borrowed raster-icon seams (fdk__list_row_set_icon_
image / fdk__iconview_item_set_icon_image / fdk__row_icon_paint_
image); every picker row now classifies by content [folders, images,
audio, video, archives, documents, fonts, source, executables —
extension table + exec-bit split]; tests/test_icon_theme.c pins the
engine headlessly with solid-color fixture themes; the X11 suite
gained three file-dialog-icons groups reading the X SERVER's own
pixels [per-row identity at 16 and 48 px + theme-switch cache drop];
real-theme verification against the system's Adwaita caught two
spec bugs [directory names contain slashes; hicolor's 11-KiB
Directories line] and produced the docs/screenshots gallery pair;
also fixed: FDK_ROW_ICON_RECENT silently reading as NONE, the
dialog icon-grid's one-column layout [fdk__iconview_reflow], and
the engine's uninitialized chain-slot read [found by a 30-run
sweep]), building on **1.5.0** (the
release-engineering pass — RETRACTED same-day, see below; the
packaging/CI infrastructure below remains on main as dormant
release tooling): the version decision itself [1.0.0 adopted, then
**retracted the same day** — the premature v1.0.0 tag and its
never-published draft release deleted; version back to 0.0.1 per
docs/versioning.md's retraction record; SONAME scheme back to
dormant, the Makefile installs the single unversioned libfdk.so;
the packaging scripts gained a pre-1.0 PACKAGING-SKIP guard so CI
stays green AND honest; ABI unchanged at 621 exports]; the
packaging tree
[packaging/: dpkg-deb + rpmbuild + makepkg + abuild + xbps-create,
five families, no libfdk.a anywhere per the license's 4(d)]; GitHub
Actions [ci.yml push/PR validation, reusable distro-packages.yml
docker matrix, release.yml tag → draft release via GITHUB_TOKEN
with minimal permissions and SHA-pinned actions]; docs/packaging.md
the full packaging+release reference; the Debian and RPM pipelines
validated END-TO-END locally [build, package, extraction-based
graphical smoke under Xvfb], musl compile probe 80/80 — then the CI iterations themselves took the matrix to fully green: all five distro pipelines + the ubuntu home-ground job pass end-to-end on main [build, native package, clean-container install-test, graphical smoke, removal] — all before the retraction; the v1.0.0 tag's DRAFT release (four validated families + source + SHA256SUMS, void joining on main after the tag) was deleted as part of it), building on
**1.4.19** (the drop that
landed — the maintainer's live report that real-file-manager drops
hover fine but never complete; three XDND receiver bugs found and
pinned: the XdndStatus action atom was always None, which
action-strict sources [Qt family, several XWayland bridges] read as
a refusal and cancel the drop — now answers XdndActionCopy, and
XdndFinished carries the performed action too [v5]; a failed payload
transfer silently vanished with NO event — now delivered as an
empty-payload DRAG_DROP, the contract Wayland always had; the
XdndTypeList read capped at 64 atoms with the remainder ignored —
now slab-read with a containment cap; drop-fetch budget 600->1000ms;
the capabilities example now prints every decoded payload entry
including folders; verified by a new GTK-faithful rig [8-type
TypeList with uri-list at index 4, real timestamps, strict and
refusing source modes] across a 6-mode x 2-lane app-level matrix and
three new suite groups), building on **1.4.18** (the GPU present
lane — the maintainer's hardware-acceleration ask, verified-absent
first, then implemented as a hybrid: CPU rasterization unchanged and
byte-identical, GLX presentation runtime-loaded via dlopen with a
hand-declared API subset [no new build deps], policy-gated [hardware
renderers only; llvmpipe/softpipe/swrast refused because the SHM path
is faster there; FDK_NO_GL / FDK_GL_FORCE knobs], self-verifying on
its first present, permanently downgrading with full frame salvage on
any failure, observable via fdk_window_get_render_path; the whole
160-group X11 suite additionally ran three times with FDK_GL_FORCE=1
so every widget/dialog/DnD test went through the lane, and a new
examples rig proves byte-identity at the app level), building on
**1.4.17** (the smooth
pass: the maintainer's polishing report — every themed control now
paints through the antialiased primitives so corners and circle
indicators read smooth; the hover-fade use-after-free fixed with
the done-callback handle clear every other animation owner already
had; example 04 opens screen-sized and scrolls its whole catalog
through the new `fdk_scrollview_set_vertical_only` recipe; the five
data-only faces from the previous task are now pinned in the
shipped-theme suite), building on **1.4.16** (the three faces:
Mono Chromatic, Pink Rave, and Faded Dream as the new built-in
default — the maintainer's theming-ability test, shipped as three
.fdk files in the custom `.FDKThemes/` folder with the TITLEBAR
token family that themes the decoration band and the `button_shape`
metric that turns every button circular; plus FIVE MORE faces
shipped data-only at the same audit: Catppuccin Mocha, Catppuccin
Latte, Nordic Frost, AMOLED Void, Solar Flare — ten stems total in
.FDKThemes/), 1.4.15 (the chrome uniformity tail: the toolkit's
own dialogs wear the FDK title bar), 1.4.14 (the live settings
desktop — per-app themes, LIVE re-theming when the command runs,
the X11 INCR-drop fix, the FDK title bar on every example, the
file-picker size retune, the developer theme whitelist) and the
1.4.12 full in-depth code review — every claim below was
re-verified against the tree, not copied from docs. THIS AUDIT'S
OWN FIRST CATCHES: 1.4.15's "X11 suite all-pass" claim was stale —
the decorated dialogs had shifted every dialog test's child
navigation one slot (the band became root child 0) and the suite
had not actually been run; worse, all five custom dialog-body
arrange hooks (message/prompt, About, font chooser, color chooser,
file picker) were double-offsetting their children by the band
height — every dialog's content rode 28px too low since 1.4.15.
Both fixed and pinned at 1.4.16; the tree is always the truth,
including over this file. THE AUDIT'S OWN FIRST CATCH: the ledger's "themed
tooltips" candidate was stale (the 1.3.2 tooltip has been fully
themed since birth — tokens, parser, Modern retune, tests); the tree
is always the truth, including over this file.

Same-date **v1.0.0-prep documentation pass** (docs/branding only —
zero toolkit code touched): the Faded Dream moon logo replaced
`examples/data/fdk_logo.png` (SAME 96x96 RGBA format and size —
examples 02 and 04 decode it at that size; do not resize the asset
without checking their panel layout); the README was rewritten
concise with the Faded Dream Project banner and badge row; `LICENSE`
was rewritten as the FDK Proprietary License v1.0 (restrictive on the
toolkit, applications expressly excluded — its Section 5);
`docs/screenshots.md` now indexes the captured gallery;
`docs/licensing-policy.md` updated to match.

Same-date **v1.0.0 release-engineering pass** (build system +
packaging + CI + docs): public version 0.0.1 → 1.0.0 (fdk_version.h
+ its test pin; the Makefile now derives SONAME libfdk.so.1 and
installs the standard versioned layout libfdk.so.1.0.0 + symlinks;
fdk.pc/install paths unchanged in spirit — LIBDIR override already
existed); packaging/ tree with per-distro native recipes (debian
control+dpkg-deb, fedora spec, arch PKGBUILD, alpine APKBUILD, void
xbps) + ci/ orchestrators + common/smoke.c public-API smoke app;
.github/workflows/ (ci, distro-packages, release); docs/packaging.md
new, versioning.md rewritten as the 1.0.0 decision record, abi-
policy/build/licensing-policy/README synced. THE CONFIG-STAMP
FOOTGUN pinned: `make install` after `make release` silently
rebuilds debug+ASan into the payload — all recipes pin
FDK_CONFIG=release on install (caught live by the Debian NEEDED
scan failing on libasan.so.8).

Same-date **license finalization pass** (docs/licensing only — again
zero toolkit code touched): the license's open decisions were closed
by the Licensor — governing law and venue set (laws of Romania,
exclusive venue Bucharest, with the EU consumer-forum savings);
Section 4(d) rewritten from runtime-accompaniment to the
**separate-runtime model** (applications are distributed WITHOUT the
FDK runtime — no bundling, embedding, static linking, or packaging of
it without further written authorization; end users install the
runtime themselves from an authorized channel); the draft banner and
every [REVIEW] flag resolved into operative text; Section 5 gained
5(f) stating the model restricts the toolkit, never the application;
the OPEN REVIEW ITEMS list replaced by a NOTICE recording the
counsel-free adoption; README, `docs/licensing-policy.md`, and the
roadmap's Phase 1 line synced to match. Rights holder unchanged:
FemBoyGamerTechGuy, by established handle.

Same-date **v1.0.0 retraction** (release-state only): the v1.0.0
release-engineering pass was undone hours after it ran — manual
testing showed the file/folder pickers were not release-ready
(uniform page glyphs for every file, no system icon-theme
consultation, visible picker defects). The premature `v1.0.0` tag
was deleted from the remote, the never-published draft GitHub
Release (9 assets: four distro families + source + SHA256SUMS)
was deleted with it, the public version returned to **0.0.1**
(docs/versioning.md carries the full retraction record), the
SONAME scheme returned to its documented dormant state (the
Makefile builds and installs the single unversioned libfdk.so),
and the packaging scripts gained the pre-1.0 PACKAGING-SKIP guard.
Nothing else was rewritten: the license finalization, the
packaging tree, the CI/release workflows, and all source commits
remain on main. The picker + icon-theme overhaul that motivated
the retraction is the next ledger entry.

## Current state (verified this audit)

- **HEAD:** milestone **1.4.19** (the drop that landed — the XDND
  receiver-faithfulness pass: XdndStatus now answers the action atom
  so action-strict sources [Qt family, several XWayland bridges]
  commit their drops, a failed payload transfer delivers an
  empty-payload DRAG_DROP instead of silently vanishing, the
  XdndTypeList slab-read replaced the 64-atom truncation; the full
  narrative is docs/roadmap.md "1.4.19 — the drop that landed").
  Before it: **1.4.18** (the GPU present lane — see above;
  the full narrative is docs/roadmap.md "1.4.18 — the GPU present
  lane (hybrid rendering)"; the one-sentence architecture: FDK is now
  a hybrid renderer — every pixel rasterized on the CPU exactly as
  before, presentation on the GPU where one exists. Verified
  GPU-accelerated operations: the damaged-rect texture upload, the
  fullscreen composite, and the buffer swap — i.e. the entire
  pixels-to-screen half of every frame). Before it: **1.4.17** (the
  smooth pass — see above;
  the full narrative is docs/roadmap.md "1.4.17 — the smooth
  pass"). Before it: 1.4.16 (the three faces — the theming
  ability test the maintainer asked for: **Faded Dream** is the new
  built-in default (the calm gray-with-violet retune; the packaged
  copy ships as .FDKThemes/faded-dream.fdk and the pre-1.4.16
  "FDK Modern" values survive as a documented recipe), **Mono
  Chromatic** (black-and-white discipline: white world, black
  speech, a near-black titlebar family, CIRCLE buttons) and **Pink
  Rave** (the all-pink club floor: plum surfaces, hot-pink accent,
  a hot-pink band) ship in the source tree's **`.FDKThemes/`**
  folder — the custom folder, also live as the per-user
  `~/.FDKThemes` (outranking every system location) and the
  canonical `<datadir>/fdk/.FDKThemes` install target (the legacy
  `fdk/themes` still scanned). The engine grew the **TITLEBAR
  token family** (five FALLBACK tokens: unset keys read through to
  control/text/border, so pre-1.4.16 themes keep their exact band;
  the band's fill, text, rule, and button hover/pressed now come
  from the theme — the title label follows TITLEBAR_TEXT on every
  switch), the **`button_shape` metric** (0 rounded / 1 circle /
  2 square — applied to the catalog Button, MenuButton, AND the
  decoration band's window buttons), and
  **`fdk_widget_set_background_token`** (the token-following
  background: a widget whose fill resolves a theme token at PAINT
  time, so app surfaces track every theme switch live — every
  example's panels now ride it, and the examples' hardcoded root
  backgrounds are gone so all twelve demo apps re-face under every
  theme). Also this milestone: the 1.4.15 dialog double-offset bug
  fixed in all five custom body-arrange hooks + the stale dialog
  X11 groups repaired and re-run for real. 1.4.15 the chrome
  uniformity tail; 1.4.14 the live settings desktop; 1.4.13 the
  command-line face; 1.4.12 INCR clipboard transfers; 1.4.11
  markup; 1.4.10 fullscreen; 1.4.9 PNG codec + Picture +
  clipboard images)
- **Build:** green in debug (ASan+UBSan) and release, X11 + Wayland
  backends both linked (Wayland protocols wired: xdg-shell,
  xdg-decoration, viewporter, **fractional-scale**,
  primary-selection)
- **Public API:** **621 exported symbols** (620 +
  fdk_surface_draw_rounded_rect_aa +
  fdk_scrollview_set_vertical_only/get_vertical_only +
  fdk_window_get_render_path),
  `make verify-exports` OK in debug AND release
- **Tests:** headless suite all-pass — **570 [ok] checks** (incl.
  the 1.4.4 + iconview suites, the eight 1.4.8 textview groups,
  the 1.4.9 png/picture suite, the 1.4.11 markup suite, the
  1.4.13 theme-discovery + CLI-tools suites, the 1.4.14
  per-app-priority / whitelist / live-recheck / fdk-set groups,
  and the 1.4.16 additions: the titlebar-fallback, button-shape
  (arithmetic + pixel proof), widget-background-token groups in
  test_theme, the ~/.FDKThemes + XDG-canonical-vs-legacy
  discovery groups, and the whole test_shipped_themes suite —
  the five faces pinned: faded-dream byte-equal to the built-in,
  mono's black band + circle buttons, rave's hot-pink band);
  X11 integration suite all-pass (**160 [ok] groups**, real Xvfb,
  real input, incl. the four 1.4.18 GPU-lane groups — policy matrix,
  byte-identity of GPU vs CPU presents, the poisoned-present
  downgrade salvage, resize-under-the-lane — plus the whole suite
  green three times over with FDK_GL_FORCE=1; the MIT-SHM
  double-buffer test is lane-aware),
  the modern-batch, HiDPI (private 192-dpi server), iconview GUI,
  file-dialog icon-mode, textview GUI, clipboard-image, the
  1.4.12 INCR read/serve/image groups, the 1.4.14 INCR-DROP
  + settings-live-follow groups against the external rigs, and
  the 1.4.16 titlebar-theming group — Mono Chromatic's black
  band over white content, Pink Rave's hot-pink band over plum,
  the built-in's fallback gray, each on the server's own pixels);
  39 test files (40 .c in tests/ counting bench.c, which is the
  perf harness, not a suite)
- **Scale:** ~58k lines of C in `src/` (+ generated Wayland
  protocols), 22 public headers, 12 examples

## Implemented — DO NOT re-add any of these

### Branding, docs, and release assets (v1.0.0-prep pass)

- **Faded Dream logo** — `examples/data/fdk_logo.png`, 96x96 RGBA:
  crescent moon centered over a night sky fading soft purple (lower)
  to deep dark blue (upper), rounded badge, transparent outside,
  no text/UI elements. Consumed by examples 02 (image panel) and
  04 (picture fits) and displayed at the top of README.md. Do NOT
  create a second/competing logo asset; the generator lives outside
  the repository (scripts/make_fdk_logo.py in the worklog's
  workspace).
- **README.md** — the concise front page (banner, badges, features,
  build/install, first-window example, examples table, themes,
  screenshots, docs table, principles, license notice). The long
  per-phase narrative it replaced lives on in `docs/roadmap.md`,
  `PROGRESS.md`, and `docs/screenshots.md` — linked, not deleted.
- **LICENSE** — the FDK Proprietary License v1.0, finalized: restrictive
  on the toolkit (no redistribute/modify/sublicense/publish/exploit
  without written authorization), with an explicit
  application-developer rights section (Section 5), the
  separate-runtime distribution model (Section 4d — applications ship
  WITHOUT the FDK runtime; end users install it separately), and
  Romanian governing law with Bucharest venue (Section 14). NOT an
  open-source license.
- **docs/screenshots.md** — the gallery index for every capture in
  `docs/screenshots/`.
- **packaging/** — the ONE packaging tree: debian/ (control
  templates + build-deb.sh + install-test.sh), fedora/ (fdk.spec +
  scripts), arch/ (PKGBUILD + scripts), alpine/ (APKBUILD +
  scripts), void/ (xbps template + scripts), ci/ (host-side docker
  orchestrators per distro + release-notes.sh + common.sh),
  common/smoke.c (the public-API package smoke test). Do not create
  packaging metadata anywhere else.
- **.github/workflows/** — ci.yml (push/PR validation), distro-
  packages.yml (the reusable docker packaging matrix), release.yml
  (v-tag → draft GitHub Release via the ephemeral GITHUB_TOKEN,
  minimal permissions, SHA-pinned actions).
- **docs/packaging.md** — the packaging/release reference (install
  per distro, reproduce, runbook, troubleshooting, signing posture).

### Widget catalog (all SHIPPED, class names from `src/widget/*.c`)

button (with **MARKUP since 1.4.11** — fdk_button_set_markup:
styled label widths, run colors, decoration bars; the role
machinery is markup-agnostic), toggle, checkbox, radio, label
(with **MARKUP since 1.4.11** — rich spans in all three display
modes: NOWRAP measures styled, WRAP breaks by styled widths,
ELLIPSIZE cuts by styled metrics; get_text reports the PLAIN
text), entry, progress (determinate +
busy), separator, frame, notebook, list (multi-select, rubber-band +
auto-scroll, row icons), tree (row icons; multi-select + rubber band
+ auto-scroll), scrollview + scrollbar (smooth wheel; 1.4.17:
`fdk_scrollview_set_vertical_only` — content arranged at the
viewport's width, horizontal range collapsed, the recipe for a
scrolling whole-window catalog), menu /
menu-bar / context menu (accelerators + mnemonics), combo (editable),
slider (marks/ticks + labels; vertical orientation), **iconview**
(the 1.4.5 item grid: glyph cells in a re-flowing column field,
the List's selection model, grid keyboard nav, batch fills; the
1.4.6 rubber band + themed cell metrics; the 1.4.7 band
edge-chasing auto-scroll BOTH axes + cell double-click
activation),
spinbutton,
**picture** (1.4.9 — the image display: an owned surface under
NONE/CONTAIN/COVER/FILL fit policies, scaled paints on the
transformed-blit engine, the IMAGE a11y role),
**textview** (1.4.8 — the multi-line editor: word-wrap through the
break engine + no-wrap mode, caret/selection across visual lines
with the goal column, PageUp/PageDown, paragraph triple-click,
the Entry's whole splice/undo/clipboard/PRIMARY/preedit
discipline, the full a11y text interface under TEXT_VIEW),
toolbar, tooltip (with **MARKUP since 1.4.11** —
fdk_widget_set_tooltip_markup: the tip wraps by styled widths and
paints run colors + decoration bars; get_tooltip reports the
PLAIN text), spinner, link-button (role on button), paned,
expander, statusbar, stack + stackswitcher, revealer (slide modes;
crossfade via the paint group), levelbar, canvas (drawing area),
search entry (preset; debounced search-changed; BUILT on the
entry's icon slots since 1.4.4), **menu button**
(attached-popup hybrid — the hamburger), dialog (+ modal run),
**file dialog** (OPEN/SAVE, places sidebar, filters, breadcrumb path
bar, Ctrl+L location entry — modernized 1.4.0; XDG recently-used
place + accept-time recording since 1.4.4; **icon mode since
1.4.7** — options.view seeds the IconView browsing surface, the
toolbar's Icons checkbox flips it live, and every accept path
reads through the surface abstraction; **the 1.4.14 size retune**
— 866x614 window, 15px face, 34px rows, 184px places, 640x420
list: the "small picker, small text" report closed), **color button** (the
color-well swatch that opens the ColorChooser — 1.4.4; alpha
checkerboard, heap-token lifetime guard), path-bar composite,
window decorations (the FDK-drawn title band — themed fill, title,
  **its every pixel theme data since 1.4.16: the TITLEBAR token
  family (fallback semantics) paints the band, rule, glyphs, and
  title label, and the window buttons follow the theme's
  button_shape story (circle themes get circular window
  buttons)**,
close/min/max, band drag, FDK-driven resize; **worn by EVERY
example since 1.4.14** — example_window.h decorates the standard
window, 06/07/09/10 decorate their own, 02 stays bare because its
subject is raw-surface rendering that never paints the tree). Entry icon slots
(leading/trailing glyphs + custom painters + press callbacks,
1.4.4). List activate-on-single-click (1.4.4 — the places
sidebar rhythm).

### Dialogs & pickers (SHIPPED 1.4.3 — choosers.c; icons 1.5.1)

- **FilePicker / FolderPicker iconography (1.5.1)** — every row and
  grid cell wears the DESKTOP's icons via the internal icon-theme
  engine (src/theme/icon_theme.c): user-theme detection (GTK/KDE
  settings + the system default/index.theme channel + $FDK_ICON_THEME
  override, no bus), inheritance chains,
  Fixed/Scalable/Threshold size matching with a relaxed fallback,
  pixmaps last resort, per-type classification (extension table +
  executable-bit split), symbolic tinting, three cache layers plus
  the decode-refusal set, reset per dialog. PNG preferred per
  directory, SVG rasterized at the requested size via the vendored
  nanosvg (undrawable SVGs and XPM are honest misses), procedural
  glyphs as the floor. List rows 16 px, grid cells 48 px.
  Fixed on the way: the RECENT glyph's silent drop in
  fdk_list_row_set_icon's switch; the dialog icon-grid's stuck-at-
  one-column layout (fdk__iconview_reflow, the 1.4.7 lesson's third
  instance); the chain walk's uninitialized-slot read.

- **AboutDialog** — logo/name/version/comments/copyright, LINK
  website row (activating fires a callback with the URL — FDK never
  execs anything), scrolling license block
- **FontChooser** — face list from `fdk_font_enumerate`, size
  spinner (6..96), live preview reloading via `fdk_font_load_face`
- **ColorChooser** — the HSV wheel on the canvas: per-pixel hue ring
  + saturation/value triangle (barycentric), drag hit-testing on
  both regions, marker dots, #rrggbb entry, current/initial swatches

### Platform / input (SHIPPED)

- X11 backend: full window lifecycle, EWMH/ICCCM, XIM **full-Unicode**
  input, resize edges, decorations, **HiDPI** (1.4.5: Xft.dpi /
  screen-metric detection at connect, integer scale [1,3],
  physical-sized windows + divided input — the same core
  compositing the Wayland side rides)
- Wayland backend: xdg-shell lifecycle, libxkbcommon keymap, client
  key repeat (1.3.5), output hot-plug/unplug, compositor-death
  resilience, cursor shapes
- Clipboard: text + URI formats, **PRIMARY selection** (1.3.4)
- DnD: both directions (X11 + Wayland), `fdk_drag_begin`; **the
  1.4.14 INCR-drop fix** — X11 drops now read through the shared
  requestor-parameterized selection engine (atomic, multi-part,
  AND incremental transfers; what GTK/Qt file managers do for
  larger uri-lists — previously the drop read the INCR seed as
  garbage and vanished after a "drop it" hover), with format
  fallback (uri-list refused -> UTF-8 text) and a 1000 ms
  convert budget; **the 1.4.19 receiver-faithfulness pass** —
  XdndStatus answers the v2+ action atom (XdndActionCopy; action-
  strict sources like Qt read a None action as a refusal and cancel
  the drop: hover highlights, release vanishes — the maintainer's
  live report), XdndFinished carries the performed action (v5), a
  failed transfer delivers an EMPTY-payload DRAG_DROP instead of
  vanishing silently (Wayland's contract, now X11's too), the
  XdndTypeList read is slab-bounded past 64 atoms (1024 cap), and
  the GTK-faithful rig + three suite groups pin all of it
- Focus traversal, double/triple-click, shift-click range select
- **Window states**: maximize/minimize/restore (Phase 8) +
  **FULLSCREEN (1.4.10)** on both backends — EWMH client messages
  with the bare-X fallback; xdg_toplevel requests with the
  configure states array as truth
- **HiDPI on Wayland: SHIPPED** — fractional-scale-v1 listener,
  viewport source rectangles, `fdk_window_get_scale`
- **HiDPI on X11: SHIPPED (1.4.5)** — Xft.dpi / screen-metric
  detection, integer scales, physical windows, divided input
- **The AT-SPI text interface + relations export: SHIPPED (1.1.0,
  C-2 / B-10 level 2)** — org.freedesktop.atspi.Text on the bridge
  (the full method surface; CODEPOINT<->BYTE conversion with the
  multibyte pin; per-character extents + hit-test through the
  internal fdk__widget_text_char_rect/offset_at seam — Entry's caret
  arithmetic, TextView's line cache, whole-bounds fallback for
  read-only classes; the whitespace hop for word runs);
  FDK_A11Y_CARET_MOVED (the public enum append) as the signal source
  -> TextCaretMoved/TextSelectionChanged, VALUE_CHANGED-on-text ->
  TextChanged; GetRelationSet with the AT-SPI relation numbers pinned
  against at-spi2-core's constants. D-027; rigs: 3 live-bus legs + 2
  headless legs (incl. the no-op rule).
- **Color glyphs (emoji): SHIPPED (1.1.0, C-3 / B-02)** —
  COLRv0/v1 + CPAL, CBDT/CBLC, and sbix, all parsed natively
  (src/text/emoji.c) with layouts pinned against fontTools; the RGBA
  glyph cache entry (fdk_surface_blend_rgba composites it; the public
  API unchanged at 848); THE BITMAP-ONLY LOADER GATE —
  fonts-noto-color-emoji (no glyf at all) loads and draws; the
  COLRv1 paint engine (gradients in paint space, affine coverage for
  rotate/skew, all 28 W3C blend modes); strike selection + box-filter
  downscale; the Zsye shaping fallback bucket;
  $FDK_NO_COLOR_GLYPHS=1 escape hatch. D-026; rig: test_emoji
  (COLRv0 math, every v1 paint family, CBDT strikes, the real-Noto
  smoke leg, the truncation sweep).
- **Basic gestures (pinch + swipe): SHIPPED (1.1.0, C-6 / B-17)** —
  FDK_EVENT_GESTURE_PINCH (span arithmetic on the two-slot posture,
  scroll coexistence, re-baseline) + FDK_EVENT_GESTURE_SWIPE (the
  per-finger end-gesture verdict) on the pure touch engine; no
  backend moved. D-025; rig: 6 engine legs.
- **fdk-shell runtime re-anchoring + auto-hide: SHIPPED (1.1.0,
  C-5 / A-05 / A-08, fdk-shell 0.5.0)** — the LIVE desc (anchor/
  margin/zone/size/keyboard/level on a shown surface; the Wayland
  role swap with the wire-order lesson; the X11 state exchange) +
  the reveal-strip auto-hide (backend-native slide, withdrawn zone,
  the pointer state machine, reveal/hide_now). D-024; rigs: 9 X11
  legs + 4 sway legs.
- **The AT-SPI2 bridge, level 1: SHIPPED (1.1.0, S-8 / B-10)** —
  the accessibility WIRE over the internal a11y tree: the two-bus
  handshake (retry on a 2s throttle — late-starting screen readers
  are a real case), the object registry (virtual rows included), the
  Accessible/Component/Action interfaces + the properties, the
  Focus/State/Property/Children events, the discovery announcement.
  $FDK_ATSPI=0 off. D-023; rig: 5 live-bus legs against the mock
  stack (the premature-degradation bug the rig caught is the retry
  posture's origin story).
- **The xdg-desktop-portal FileChooser: SHIPPED (1.1.0, C-1 / B-09)**
  — fdk_dialog_open_file/save_file consult the desktop's own picker
  first (org.freedesktop.portal.FileChooser on the native D-Bus
  client; the spec's race-free request dance) and fall back to the
  toolkit's own browser on any failure; $FDK_FILE_DIALOG_PORTAL=0/1/
  auto. The fallback's re-entry guard (no second flight; the UAF the
  rig caught) and the null parent. 848 exports. D-022; rig: 5 live-
  bus legs against the mock portal.
- **Drag icons + widget-level drop targets: SHIPPED (1.1.0, S-9)**
  — `fdk_drag_begin_ex` (the icon: Wayland start_drag's own surface /
  X11's override-redirect tracker window, alpha-shaped) +
  `fdk_widget_set_drop_target`/`_get_drop_target`/`_set_drag_callback`
  (scoped ENTER/MOTION/LEAVE/DROP; per-position acceptance on both
  backends — XDND Status and wl_data_offer.accept re-evaluate per
  position; the window callback still sees everything). Same-
  connection drags fixed on Wayland (the ::send self-serve read +
  the drag_drop_active fence); the 250 ms EOF wart and the X11
  DROP-event position closed. 847 exports. D-021; rigs: 7 X11 legs
  + 2 sway legs incl. the FDK-source drag-with-icon end to end.
- **Window activation + startup notification: SHIPPED (1.1.0, S-6)**
  — `fdk_window_activate` (Wayland xdg-activation-v1 through a
  compositor token; X11 `_NET_ACTIVE_WINDOW` with the live
  last-user-event timestamp + the bare-X raise/focus fallback) and
  `fdk_window_activation_token` (the launcher half: a minted token /
  SN id handed to a spawned child via `XDG_ACTIVATION_TOKEN` /
  `DESKTOP_STARTUP_ID`; the X11 mint emits the chunked "new:" SN
  state client-side, the first show of an id-carrying window emits
  the one-shot "remove:"). Both vouchers are captured once at
  connect and unset immediately (the protocols' hygiene rule). The
  Wayland mint is reentrancy-safe (private event queue +
  roundtrip_queue). 843 exports. D-020; rigs: 6 X11 legs + 5 sway
  legs incl. the two-process launcher flow (held-button serial,
  re-exec'd child, focus flip through the voucher)
- **GPU present lane: SHIPPED (1.4.18, X11)** — hybrid rendering:
  CPU rasterization unchanged (byte-identical, pinned by tests),
  GLX presentation via runtime-loaded libGL (dlopen, hand-declared
  API subset, no new build deps); hardware-renderer policy with
  software refusal (llvmpipe/softpipe/swrast), FDK_NO_GL /
  FDK_GL_FORCE / FDK_GL_FAIL_AT knobs; first-present server-side
  self-verification; permanent downgrade with full frame salvage;
  `fdk_window_get_render_path()` introspection (621st export);
  `tests/data/lsan-gl.supp` for Mesa's exit-time allocations. See
  docs/rendering.md "The GPU present lane (1.4.18)". The Wayland/EGL
  sibling SHIPPED 2026-10-30 (D-1 / B-15): src/platform/wayland/
  wayland_egl.c — the same contract on dlopen'd libEGL.so.1 +
  libwayland-egl.so.1 (eglGetProcAddress for the GLES2 subset;
  zero new build deps), the quad shader pinning alpha 1.0 + the
  alpha-less EGLConfig (the staging XRGB8888 buffer's zero top
  byte would otherwise composite as a fully transparent window —
  the GLX lane's "X byte rides along" reasoning does NOT cross to
  Wayland's ARGB compositing), the RGBA-readback self-check (the
  only GLES2-guaranteed default-framebuffer readback) with the
  process-wide refusal + FORCE override + fdk__wayland_egl_test_reset
  seam, the permanent downgrade with frame salvage
  (fdk__wayland_surface_downgrade_from_lane), the deferred
  first-frame flush (lane_frame_deferred), the same knobs and
  small-surface gate; test_wayland_integration.c's GL-lane group
  (policy / present+health / downgrade / self-check poison), the
  sway-rig screenshot smoke proving compositor-side bytes, exports
  unchanged at 848. Real hardware GPUs: implemented — should work
  (untested on real hardware; the rig runs it through Mesa's
  swrast under FDK_GL_FORCE).

### Infrastructure (SHIPPED)

- **The AA chrome pass (1.4.17)**: every themed control paints
  through the antialiased primitives (fill/draw_rounded_rect_aa,
  fill_circle_aa — 57 call sites across the widget layer); interior
  pixels byte-identical to the crisp path, only the curved fringes
  blend; **fdk_surface_draw_rounded_rect_aa** completes the public
  AA family (1-px stroke through the edge rows' pixel centers —
  radius 0 is byte-identical to draw_rect, straight sides exactly
  the crisp outline's pixels, corners ramp)
- **Hover-fade handle lifetime (1.4.17)**: the shared
  fdk__hover_fade_arm's done callback clears the stored flight
  handle on completion AND cancel — the reaped-zombie
  use-after-free the settings demo found under ASan; the
  regression test pumps past the grace and arms again
- Theme engine: `.fdk` format, **33 color tokens + 15 metrics**
  (`FDK_TK_*` / `FDK_TM_*`), runtime switching, the Faded Dream /
  Modern / v1 recipes; button roles (suggested/destructive/link),
  placeholder text, hover fades (1.4.1/1.4.2). **The 1.4.16
  additions: the TITLEBAR family** (titlebar_background/text/
  border/button_hover/button_pressed — FALLBACK tokens: unset keys
  read through to control/text/border so pre-1.4.16 partial themes
  keep their exact band; the deco band, its rule, its glyphs, and
  the title label all resolve them; titlebar_button_* carry the
  band buttons' hover/pressed) **and the button_shape metric**
  (0 rounded / 1 circle / 2 square, applied by the catalog Button,
  MenuButton, and the deco band's window buttons via the shared
  fdk__button_shape_radius); **the built-in default is Faded
  Dream** (gray ramp #232329/#33333C + the violet accent family
  #8F79D9 — the packaged copy ships as faded-dream.fdk and the
  test suite diffs them byte-for-byte); **the override mask**
  (colors_set[] behind the fallback) also fixed a latent UB: the
  parser's duplicate-detection bitmask crossed 32 tokens
- **fdk_widget_set_background_token (1.4.16)**: the token-following
  background — a widget's fill resolves a theme token at PAINT
  time, tracking every theme switch live with no hook (explicit
  set_background still wins and cancels the mode); the examples'
  structural panels ride it
- **The three faces + the .FDKThemes folder (1.4.16)**: Mono
  Chromatic (white world / black speech / near-black titlebar /
  circle buttons), Pink Rave (plum floor / rose ramp / hot-pink
  accent and band), Faded Dream (the default's packaged copy);
  shipped in the source tree's `.FDKThemes/` (daylight + matrix
  alongside), installed to `<prefix>/share/fdk/.FDKThemes/`,
  discovered on the grown search path
  ($FDK_THEME_DIR > **$HOME/.FDKThemes** > $XDG_DATA_HOME/fdk/
  .FDKThemes > the legacy fdk/themes > each XDG_DATA_DIRS entry's
  pair) — test_shipped_themes pins all five
- **Theme discovery + the settings engine + the CLI (1.4.13,
  grown into its final shape 1.4.14)**: fdk_theme_find /
  fdk_theme_available_* over the search path above
  (where `make install` puts the shipped five faces),
  fdk_theme_file_path, the settings ENGINE (src/theme/settings.c:
  $FDK_THEME > the app's own <app_id>.prefs > the global fdk.prefs
  > built-in; the one-shot lazy boot; **the inotify live follow** —
  the pump polls the watch fd, re-resolves on watched-file changes,
  and repaints fdk_run()-shaped apps itself; explicit set_default
  opts the process out AND stands the watch down; fail-soft
  throughout), **fdk_theme_set_allowed_themes** (the developer
  whitelist: stems or internal names, first-allowed fallback,
  immediate re-clamp, settings-only — code is always honored), and
  the **fdk-theme / fdk-prefs / fdk-set** command-line tools
  (tools/, installed to $(PREFIX)/bin; fdk-set theme set NAME
  [--app APP] writes the per-app override) — docs/cli.md is the
  reference
- Animation: easing library (**11 functions**), animator, smooth
  scroll, menu/combo fades, revealer slides, expander door
- **Paint group (1.4.3): per-widget subtree opacity** — the ARGB
  offscreen + global-alpha source-over blit engine (the crossfade
  rides it; nested groups composite naturally)
- **Font surface (1.4.3): `fdk_font_enumerate`** (name-table parse,
  TTC faces, family/style/path sorted) + **`fdk_font_load_face`**
- Accessibility: a11y tree (**41 roles**), narrator, announcements,
  per-widget value interfaces
- i18n: catalog, plurals, dates, locale
- Undo/redo: generic undo stack + entry integration
- Preferences: `.fdk-prefs` persistence, atomic saves (1.3.7)
- Text: fontconfig scan, stb_truetype shaping, AA rendering,
  ellipsize, line breaking
- **Images (1.4.9)**: stb_image decode (file + memory), the PNG
  ENCODER over zlib (fdk_surface_save_png), the Picture widget,
  clipboard image/png (X11 atomic property + Wayland data-device
  pipe, one-content-at-a-time)
- **Rich text / markup (1.4.11)**: fdk_span (byte range +
  absolute style + optional color + underline/strikethrough), the
  span twins of measure/draw/break/ellipsize (zero spans == the
  classic behavior), fdk_markup_parse — a TAG SCANNER (<b> <i>
  <u> <s> <color=#rrggbb[aa]> + the six classic entities; nesting
  merges; unknown tags, stray '<', and unmatched closes stay
  LITERAL; unclosed tags close implicitly), the STYLE-VARIANT-
  keyed glyph cache (regular + bold rasters coexist in one font
  object), decoration bars in run colors, kerning resets at
  style-run boundaries. NO size markup (a font is one face at
  one pixel size — the documented tradeoff)
- Shortcuts/accelerators/mnemonics (1.3.3 / 1.3.6)
- Versioned shared-library export surface (version script +
  verify-exports gate)

## What's left (the authoritative backlog)

### NEXT — nothing

The backlog is EMPTY. The widget families shipped across the
board; rich text shipped in 1.4.11; INCR transfers shipped in
1.4.12; the command-line face shipped in 1.4.13; the live settings
desktop shipped in 1.4.14 (per-app themes, live re-theming, the
INCR-drop fix, the whitelist — the maintainer's own usage reports
were the backlog, and every item closed); the three faces shipped
in 1.4.16 (Mono Chromatic, Pink Rave, Faded Dream as the default,
the .FDKThemes folder, the titlebar family, button shapes, the
token-following background — the theming-ability test the
maintainer asked for, and with it the 1.4.15 dialog-offset bug
swept out); the five extra faces shipped data-only on top of it
(Catppuccin Mocha/Latte, Nordic Frost, AMOLED Void, Solar Flare);
and 1.4.17 closed the maintainer's polishing report in full (the
AA chrome pass over every themed control, the hover-fade
use-after-free, example 04's screen-sized scrolling catalog, the
ten-face enumeration pins). What remains is
LATER-by-policy (IME completion surface
waits for a protocol joining third_party/wayland-protocols) and
OUT-by-policy (the deliberate non-goals below) — the line where
adding anything more would make FDK something other than a
toolkit.

### LATER (real, not next)

- IME completion surface (preedit is display-only today)

### OUT (deliberate policy — do not chase)

GLArea (software renderer is the product) · CSS-like styling (`.fdk`
is the surface) · icon-theme loading (vector glyphs + app surfaces) ·
printing · a11y bus bridges (in-process narrator) · touch/gestures ·
Assistant/wizard · D-Bus anything.

## Verification protocol (run before claiming anything is done)

```sh
source /home/z/my-project/scripts/fdk-env.sh && cd $FDK_ROOT
make                 # debug build, ASan+UBSan (libs + the CLI tools)
make test            # headless suite
make test-x11        # X11 integration (auto-Xvfb)
make release && make verify-exports   # 612 symbols, both configs
```

Full battery (before each milestone commit): interop rig, X11 + sway
example rigs, both tooltip rigs, compositor-death rig — see
`docs/testing.md` and the session rigs in `/home/z/my-project/scripts/`.

## Protocol for future work

1. **Before** writing any widget/feature: read this file, then
   `rg -n '<name>' include/fdk/ src/widget/` to confirm absence.
2. **After** shipping: append to "Implemented", strike the backlog
   item, bump the milestone, update `docs/roadmap.md` narrative +
   ledger, and commit it all together.
3. Never trust a session hand-off summary over the tree — `git log
   --oneline -3` and this file are the truth.
