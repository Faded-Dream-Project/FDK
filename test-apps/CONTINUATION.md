# Round 17 (2026-11-04, evening) — the drag-highlight profile

## The owner's brief
"Very fast highlighting folders by using drag click": perf shows
53.7% blend_pixel (45 of it via fill_rect under row_paint, 8 via
list_paint), 15.5% fill_rect self, ASan still on the rig. "It feels
like we are going in circles" — fair; see the command list below.

## What shipped
- surface.c: fill_rect_translucent — the hoisted, 4-wide-unrolled
  translucent fill; EXACT vs blend_pixel on the 256-value sweep
  (both formats); 1.17-1.22x on -O2 scalar. The bench harness is
  #ifdef FDK_R17_BENCH inside surface.c (fdk__r17_bench; link any
  main against libfdk.a).
- list.c: the incremental band want-test (press = full window,
  motions = prev∪current band window only). The FIRST version
  seeded an empty window at press — test_row_press_band caught it
  (a new gesture must re-evaluate every row: outside-band rows
  clear). Lesson re-confirmed: never ship a band change without
  that test.
- run_scrollcheck.sh now uses the in-tree PPM xsnap (the sandbox's
  PNG py-tool was lost to a recycle; the C tool is the permanent
  one), and its stage-D unmap/map helper was rewritten (the ctypes
  XQueryTree argtypes were wrong and segfaulted).

## The owner's machine — the command list (BREAK THE CIRCLE)
The recurring mistake: profiling the ASan build. ASan doubles
raster costs and skews every ratio. On YOUR rig, run:

  # 1. build + run the RELEASE binary
  make release && FDK_FILES_STARTUP_DIR=$HOME test-apps/build-rel/fdk-files &
  # 2. profile THE RELEASE BINARY this time:
  sudo perf record -g -p $(pidof fdk-files) -- sleep 10
  sudo perf report --stdio --no-children -g none | head -40

  # 3. the present-volume counter (how many pixels per frame hit
  #    the wire — tells us damage quality, not just CPU):
  FDK_PRESENT_TRACE=1 test-apps/build-rel/fdk-files 2>&1 | grep PRESENT-TRACE | head

  # 4. the A/B switch for the blit path (is the strip path even
  #    active? FDK_SCROLL_NOBLIT=1 turns it OFF — if scrolling is
  #    EQUALLY slow both ways, the bottleneck is elsewhere):
  FDK_SCROLL_NOBLIT=1 test-apps/build-rel/fdk-files

Send the outputs of 2 (both with and without the scroll storm) and
3; if 4 shows no difference, the fill volume is damage-side, not
raster-side, and round 18 goes after the caller, not the pixel.

## NOT DONE (carried)
- SSE2 / 8-bit-LUT translucent fill (the scalar 1.2x is honest but
  modest; the LUT needs a rounding-proof against the float form).
- The 4K / scale>1 physical-space strip blit (round-16 carry).
- The release-config test_attr segfault + the sway button_hits
  assertion (both reproduce on the CLEAN round-16 tree — a config
  hygiene round of their own).
- The up-storm geometry-flip residue (round-16 carry; the current
  rig passes all stages on the PPM tool).

# Round 16 (2026-11-04, later) — the profile, the storms, the no-op clear

## The owner's brief
A perf profile of fdk-files on real hardware (perf record during
aggressive scrolling) + three live reports: the folder list
disappears after aggressive scrolling (nav click restores it), the
same after sitting on another window, and up-scrolling glitches
text into overlapping. The profile itself: 39.6% fdk_surface_fill_rect
+ 22.9% blend_pixel + 4.1% blend_mask, libasan loaded (an ASan
build on the rig!), libnvidia-eglcore present (the EGL GPU lane).

## What shipped
- tests/test_x11_integration.c: test_scroll_storm_identity_gui —
  the deterministic aggressive-scroll regression (striped content,
  two notches per pump, XGetImage identity vs forced full repaint;
  the vbar column excluded — the thumb's animation timing is the
  BAR's story, not the blit's).
- scroll.c: two strip-path fixes (clipped translate of the pending
  box; live re-read of the root box before the replace). See
  CHANGELOG for the mechanism.
- surface.c: 0-alpha fill on ARGB = memset clear (the HiDPI
  intermediate no-op bug — also a big perf win at scale > 1).
- scripts (sandbox, outside the repo): scrollcheck_r16.sh (the
  Xvfb visual rig — stages: 30 Hz down-storm, up-storm, jitter,
  unmap/remap sit, post-nav; pixel-identity after each), xsnap.py
  TWO bug fixes (missing XImage.bits_per_pixel field; the channel-
  shift formula computed the mask's top bit, not its shift — every
  old screenshot had scrambled channels; identity checks were still
  valid because both sides used the same deterministic tool),
  fdk-wl-inject.c gained a `wheel <notches> <ms>` verb (real
  axis + axis_discrete v120 events).
- run_scrollcheck.sh (in-tree): the visual rig, for any future
  scroll-path change.

## The ASan guidance for the owner (IMPORTANT for perf numbers)
The profiled fdk-files had libasan loaded — AddressSanitizer
instruments every memory access and roughly doubles raster-loop
cost. The 62% fill_rect+blend_pixel self-time is real work volume,
but the absolute numbers are inflated. Daily-driving: use the
release build (make release / build-rel); keep an ASan build for
repro sessions only.

## The rig's lessons (round 16's own four bugs)
1. The Wayland injector path never delivered wheels to the app in
   this rig (pointer/seat quirks under headless sway) — the Xvfb +
   XTEST path (stress-rig) is the reliable one for scroll storms.
2. My first "blank list" reading was a screenshot-tool channel bug
   (xsnap.py), not an app bug — verify the TOOL against a known
   color before trusting a blank verdict.
3. The storm snapshot vs full-repaint snapshot must exclude the
   CLASSIC bar column (thumb position differs by animation timing).
4. A canvas as scrollview content measures 8x8 — no scroll range;
   use a plain widget with real bounds (or set the canvas's min
   size) when you need a long striped content.

## Verification ladder
- make test: 763 [ok]. make test-x11: 229 [ok] (+1 storm test).
- make test-wayland (headless sway): 65 [ok].
- scrollcheck_r16.sh: down-storm identity PASS, unmap/remap PASS,
  jitter PASS; up-storm residue ~266 px (below).

## NOT DONE (do not lose)
- The up-storm 266 px residue: geometry changes mid-flight (the
  hbar visibility flip resizes the viewport between ticks) can
  still leave a stale band just below the list. Repro:
  run_scrollcheck.sh stage B. The fixed-geometry storm is green.
- The 4K / scale > 1 physical-space strip blit (P0 for the owner's
  rig — the profile's fill volume lives there).
- blend_pixel batch/hoist for translucent fills (22.9% self).

## Next round candidates
- The geometry-flip storm shape (fix the 266 px residue).
- The physical-space blit at scale > 1 (design already recorded in
  scroll.c's round-14 comment block).
- The frame profiler env var + p50/p95/max present-interval stats
  (round-15 carryovers).

# Round 15 (2026-11-04) — delete stability, the scroll notch

## The owner's brief and the round's shape
Priority one was STABILITY: "deleting a file leaves fdk-files broken"
(real-hardware report). Then scroll feel (viewport-proportional notch,
smooth handling, frame-time stats), the 4K physical-space blit, an
opt-in frame profiler, and the full verification ladder.

## What shipped
- **The delete-stability fix**: fdk_list_remove now clamps
  last_click_row and key_cursor like it always clamped anchor
  (src/widget/list.c). Enter-activate after a delete was silently
  dead — the stale past-end key_cursor read as "no cursor".
- **tests/test_list_delete.c** — the regression test. Fails on the
  pre-fix engine at the Enter leg (assertion at the "interaction
  alive after removals" step), passes after. The fail-before proof
  was run by temporarily rebuilding the engine without the clamp.
- **The delete rig** (test-apps/fdk-files/delete_rig.c +
  run_deleterig.sh): six XTEST shapes (Delete key, Shift+Delete +
  modal-confirm OK, multi-select, last row, scrolled offset,
  directory-via-async-engine) against the real app under
  ASan+UBSan. ALL GREEN — the X11 trash/delete paths did not
  reproduce the owner's breakage in this environment; the one real
  engine bug (the stale indices above) was found by CODE READING
  the removal path, not by the rig. The rig itself needed four
  fixes before its greens meant anything (below).
- **The scroll notch**: viewport-proportional wheel step
  (viewport/4, clamped 40..160, per-axis from live extents;
  fractional touchpad deltas multiply it directly) in
  src/widget/scroll.c. test_scroll.c + the x11 animation-gui legs
  re-pinned (150px viewport floors at 40; the x11 leg's
  180px-bounds minus 12px classic bars = 168 viewport, 42/notch).
- **App telemetry for rigs**: PHASE: nav <dir> (DRIG-gated),
  DRIG-GEO path, rowh= on the list GEO lines.
- **setup-sandbox.sh cwd bug fixed** (ROOT_DIR pinned absolutely at
  start; the fourth recycle had steps 2-4 running from
  /tmp/x11dev-debs and the Wayland toolchain never installing).

## The rig's own four bugs (the round's real lessons)
1. The DRIG boot WIPES /tmp/fdk-drig and rebuilds it — fixtures
   created before boot are gone by the time the list populates.
   Every shape was "green" deleting in an EMPTY directory until the
   fixture moved post-boot. Verify the deletion actually happened
   (files-left count) before trusting a green.
2. List rows start ~22px below the columns header (round 10's
   lesson, re-learned): raw list-local y lands on the header.
3. An if/else chain meant the delscroll shape never ran its click
   (the wheel block ate the branch) — no selection, Delete a no-op.
4. Hand-rolled probe scripts passed KEYCODES where KEYCODES belong
   but the nav probes passed raw keysyms via ord(ch) — the typed
   path never landed. The runner (XKeysymToKeycode per char) works;
   the quick probes were the broken ones. Lesson: use the rig, not
   ad-hoc ctypes, for anything with key synthesis.

## Verification ladder (post-recycle, final code)
- `make test`: **763 [ok]** (762 + the new regression test).
- `make test-x11`: **228 [ok]** (after re-pinning the animation-gui
  wheel legs to the proportional step).
- `make test-wayland` (headless sway, the pinned wl-dnd-source
  recipe): **65 [ok], all passed**.
- -Werror clean throughout (the suites build with it on).

## NOT DONE this round (honest ledger, do not lose these)
- **The 4K / scaled-display physical-space blit** (round 14
  admitted it degrades to full damage at 4K; the walk-time
  physical-space strip collapse is DESIGNED but NOT IMPLEMENTED).
- **The opt-in frame profiler env var** (per-frame rasterized
  pixels, damaged area, present time, frame interval — the owner
  will run it on real hardware).
- **Frame-time consistency stats** (inter-present p50/p95/max
  during fast scrolling) — PRESENT-TRACE exists (round 14) but the
  percentile report was not built.
- **The owner's real-hardware breakage was NOT reproduced**: the
  X11 sandbox paths are clean. If the hardware report persists
  after this fix, suspect environment differences (the XDG trash on
  a real disk with EXDEV, inotify storms on real folders) and add
  shapes for those.

## Next round candidates
- The 4K physical-space blit + the frame profiler (the two
  not-done items above, both spec'd).
- His X11 boot-slowness report is STILL OPEN (round-6 item C).

# §31 continuation — UPDATE (owner round 14: the scroll repaint — delta-strip damage, and the hover repaint gap)

## The owner's brief and the round's shape
The performance-audit thread's third round, aimed at the scroll path:
the engine invalidated the WHOLE scrollview on every offset change
(wheel flights, thumb drags, trough paging) and repainted the full
viewport, though a pure offset change leaves nearly every row painted
— just shifted. Fixed engine-level with a same-surface blit of the
preserved region + exposed-strip-only tree damage; along the way the
new rigs found and fixed a real HOVER repaint gap and two strip-path
correctness bugs. A full sandbox recycle mid-round (the third
incident) destroyed the round's first implementation; it was rebuilt
from scratch on the true lineage (origin/main b2df84d) and every
verification was re-run — nothing below cites a pre-recycle run.

## What shipped (engine, all internal seams — API surface unchanged)
1. **fdk__surface_self_blit** (render): overlap-safe same-surface
   region copy (memmove row walk, direction by shift sign). The
   DESTINATION is damage-tracked — deliberately and necessarily: the
   X11 present's slot sync brings the next back slot up to date by
   copying exactly the PRESENTED rects, so an untracked blit leaves
   the moved rows off the wire (stale arrangement on screen outside
   the strips) AND stale in the next back slot. SURFACE damage (wire
   bytes) is a different ledger from TREE damage (repaint-walk
   bounds): paint stays strip-limited, the wire honestly carries the
   moved band. [This was blit-check bug #1 — 36k differing subpixels.]
2. **scroll_apply_delta_damage** (widgets/scroll): pure offset
   changes blit the preserved region and set EXACT strip damage
   (fdk__widget_set_damage — replaces the root's box, unions the
   layout-entry snapshot + the visible bars' thumb-precise old∪new
   rects via bar_thumb_at/bar_thumb_damage). The flight is a
   SELF-DAMAGING animation (fdk__animation_set_self_damage: the pump
   skips its blanket post-tick invalidate); declined paths fall back
   to the whole-widget invalidate. [Blit-check bug #2: the blit/strip
   footprint must EXCLUDE a visible CLASSIC bar's strip — a
   full-width blit smeared shifted content over the opaque trough.]
3. **The HiDPI gate**: fdk__window_tree_paint_scale/_tree_surface
   (window.c, the one-way-opaque owner back-edge). At scale > 1 the
   tree paints into a cleared intermediate — preserved pixels do not
   exist there — so the strip path declines. Recorded follow-up: a
   physical-space blit under scaling (4K also honestly degrades to
   full damage today via the 75% coarsening).
4. **The hover repaint gap**: set_hovered flipped FDK_WF_HOVERED and
   delivered ENTER/LEAVE but nothing guaranteed an invalidation —
   and list rows handle neither event while row_paint renders hover
   fills from the flag. Sweeps left stale highlights; the hover
   storm measured 1 kHz zigzag → ZERO presents. The transition now
   damages both sides at the choke point. [Found by the rig AFTER
   fixing my own zigzag pattern bug — see rigs.]
5. **Instrumentation**: FDK_PRESENT_TRACE=1 (one line per present:
   monotonic ms, full|rects, damaged area, window area, rect count)
   and FDK_SCROLL_NOBLIT=1 (the A/B kill-switch).

## Measured (release build, 20k-row list, run_stressrig.sh)
| storm | strips OFF | strips ON |
|---|---|---|
| wheel 20/s | 3.0% core, 53/53 fulls | **1.8% core, 2/49 fulls, ratio 0.66** |
| wheel 250/s (freespin) | 15.7% core, 254/254 fulls | **8.8% core, 0/241 fulls, ratio 0.63** |
| hover 1 kHz | 0 presents (the gap) | **43/s presents, ratio 0.311, 4.7% core** |
| sbdrag 1 kHz | — | **ratio 0.141, 5.5% core** |
Blit pixel-identity (run_blitcheck.sh, 200/500/2000 files): PASS —
0 differing subpixels of 700,000, all three sizes.

## The rigs (in-tree; the round's own lessons are baked into each)
- `stress_rig.c` + `run_stressrig.sh` — real-rate input at last
  (RIG_RATE_HZ wheels, 1 kHz hover, scrollbar drags). THREE rig bugs
  found and fixed while building it, each of which first looked like
  an engine bug: (a) the /proc/<pid>/stat parser skipped one field
  too many — it read stime+cutime, so a pure-userspace worker
  measured "0% CPU" and the settle gate released a 20k-row UI bind
  straight into the storm (the "dead storm, burst at end" signature);
  the comment in proc_cpu_ticks guards the field count. (b) the hover
  zigzag reset its position per 16-event batch — the app's motion
  coalescing (by design) kept only each batch's LAST motion, i.e.
  the SAME row every batch: no hover change, a vacuous storm. Zigzag
  state must persist across batches. (c) the settle gate needs
  THREE consecutive quiet polls — the worker→bind pipeline has an
  idle gap a single poll can land in.
- `run_blitcheck.sh` + `scripts/xsnap.c` + `scripts/xexpose.c` —
  the pixel-identity gate (blit-scrolled frame vs forced full
  repaint must be byte-identical). XClearArea(exposures=True) is the
  repaint twin; the tools select the window by title substring or id
  (largest toplevel fallback under bare Xvfb).
- Wayland suite rig note (cost me an hour): the sway config MUST pin
  `org.fdk.wl-dnd-source` at `100 px 380 px` (the DnD legs' drag
  coordinates assume it) and export SWAYSOCK — without them the
  suite fails at wl_dnd_rx.enters / focused-window asserts that look
  exactly like engine regressions. Full recipe: WLR_BACKENDS=
  headless WLR_RENDERER=pixman, XDG_RUNTIME_DIR=/home/z/apt/xdg-run,
  LD_LIBRARY_PATH=/home/z/apt/prefix/usr/lib/x86_64-linux-gnu, sway
  -c with `xwayland disable` (no Xwayland binary on this image).

## Verification ladder (all post-recycle, on the final code)
- `make test` (ASan/UBSan): **762 [ok], all tests passed.**
- `make test-x11`: **228 [ok]** — after fixing a PRE-EXISTING
  round-10 leftover this round's runs tripped over: the test (and
  extend.c's comment, fdk_extend.h's doc, EXTENDING.md's table)
  still said X11 has no frame pacing, but round 10 shipped a real
  window_frame_ready (17 ms interval + SHM backpressure). All four
  flipped to the round-10 truth.
- `make test-wayland` (headless sway): **65 [ok], all passed** (GL-
  lane legs skip: no EGL driver on this image — documented).
- A/B + blit-check numbers above; FDK_SCROLL_NOBLIT control run
  confirms the OFF path still measures the old behavior exactly.

## Incident (the third recycle — same lesson, now with a twist)
Round 14 was completed once already when the sandbox recycled and
handed back a tree WITHOUT the round's working dir at all (work/
held an older clone at c1f27e9-era; FDK-r14 gone entirely; /tmp and
/home/z/apt wiped). Recovery followed the standing practice: fetch
origin (b2df84d was intact — the push-every-step rule would have
made this free, and the round's code commit had NOT yet been pushed,
so everything was rebuilt from the conversation's surviving
artifacts), reset main to it, rebuild toolchains via
scripts/setup-sandbox.sh (whose own cd bug — steps 2-4 stranded in
/tmp/x11dev-debs when x11dev was already prepared — is fixed in the
round's commit). The twist worth remembering: the recycled snapshot's
/home/z/my-project/scripts/ still held the OLD session's rig binaries
(fdk-wl-inject, wl_dnd_source — built Oct 7) — check there before
re-deriving tools.

## Next round candidates
- The physical-space blit under HiDPI scaling (4K full-damage today).
- fdk-files: the fs_stat_entry worker-pool threading (round-13
  carry-over — the 20k-row UI-thread bind this round's rig kept
  tripping over is exactly that candidate's business).
- (existing list: S7/S8/S10, PART-1..8, §30 backlog.)

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK;
ta_buf in apps; 8-experiment timebox; identity ONLY
Faded Dream Project <242450780+FemBoyGamerTechGuy@users.noreply.github.com>;
WIP push every step; webfetch specs, record URLs; NEVER force-push.

# §31 continuation — UPDATE (owner round 13: the core-of-FDK performance audit — the text path)

## The owner's report and what the audit found
"the filemanager feels slow — something is wrong in the core of
FDK about performance, look through the code really carefully and
find anywhere the performance can be increased." A deliberate
full pass over the ENGINE core (src/text, src/render, src/widget
paint paths, window paint walk), engine fixes only, measured with
a NEW input rig (wheel storms + hover sweeps under Xvfb, CPU
sampled from /proc) plus the round-12 nav rig.

Audit verdict table (what was checked and what it found):

| path | finding | verdict |
|---|---|---|
| paint walk (paint_rec) | damage-rect + bounds culling, clip stack | sound |
| blend_mask/blend_rgba | integer source-over, transparent-skip | sound |
| hb_font per font | created once, cached on the font | sound |
| **glyph cache lookup** | **linear scan over 2048 slots, per glyph PER PLACEMENT** | **FIXED — hash index** |
| **shaping (measure/draw/spans/caret)** | **FriBidi+HarfBuzz+3 allocs re-run per call per frame, freed, re-derived** | **FIXED — shaped-string cache** |
| fdk-files fs_stat_entry | stat() per file ON THE UI THREAD (invisible on tmpfs; THE real-disk feels-slow) | recorded — next fdk-files round: the fdk_thread.h worker pool exists, mirror thumbs.c |

## The two engine fixes
1. **Shaped-string cache** (src/text/shaping.c, text.c, entry.c):
   a shaped result is a pure function of (font, style, bytes) —
   the subpixel phase derives from the in-string pen-from-zero,
   consumers offset whole pixels — so measure/draw/spans/caret
   BORROW one LRU-cached result (256 entries, <=1KB). Runs!=NULL
   (styled spans) bypass. Invalidations: glyph eviction, style
   flush, font destroy, last-root teardown (beside the animation
   drain). THREE hard lessons baked in:
   - **Churn**: naive first-sighting admission REGRESSED the nav
     rig (14.9% -> 19.4% of a core): directory loads present N
     fresh strings, evicted before reuse. Fix: second-sighting
     admission (512-slot fingerprint remember-set + a park slot);
     bulk loads never churn, steady state always admits. Final
     nav: 13.6% (beats baseline).
   - **Ownership**: a bypass that returned caller-owned results
     into a borrow API leaked (LSan: the 1MB stress line, then
     every measure("")/draw("")). Fix: only NULL-returning cases
     bypass; empty strings flow through the cache; >1KB strings
     ride a single owned big-slot.
   - **Test layering**: two test_text assertions encoded the old
     "second draw touches the glyph table" assumption; now the
     shaped cache serves it pre-resolved. Invariants re-asserted:
     rasterizations flat (misses) + pixel-identical output.
2. **Glyph-cache hash index** (text.c): open addressing over the
   key (4096 buckets), backward-shift delete, insert-before-
   rasterize with key committed first (recursive fallback
   glyph_slot calls must see consistency). Init-to-EMPTY on font
   create (slot 0 is valid — memset(0) was the trap), reset on
   flush.

## Measurements (% of one core; release build; input rig + nav rig)
| rig | baseline | final |
|---|---|---|
| hover sweep 1000-file dir | 1.3% | **0.7%** |
| wheel scroll 1000-file dir | 6.2% | **5.5%** |
| nav storm 500 files | 14.9% | **13.7%** |
| (naive-cache nav detour) | 19.4% | the churn lesson above |
ASan: scroll 14.2 -> 10.5%; hover/nav flat (instrumentation
dominates). All rigs settle to 0.0 CPU after the storm.
Hover halved, scroll -12%, nav -8% — and the steady-state
re-shaping per frame (the actual "feels slow" mechanism for text
everywhere, not just fdk-files) is gone at the engine level.

## Verification this round
- make tools && make test (both backends, ASan/UBSan, LSan):
  GREEN — 762 [ok], zero leaks, zero warnings.
- Rigs: menu rig PASS (Properties runs Properties); click sweep
  over rows: header 160-175 -> sel 0 (none); rows 180-200 ->
  sel 1 alpha.txt (interactive selection intact).
- New rig survives in-tree: test-apps/input_rig.c +
  run_inputrig.sh (RIG_MODE=scroll|hover, RIG_FILES,
  RIG_SECONDS; build-rel for release numbers).

## Next round candidates (unchanged + this round's additions)
- fdk-files: thread the fs_stat_entry pass through the
  fdk_thread.h worker pool (mirror thumbs.c's pattern) — the
  real-disk directory-load latency, the remaining app-layer
  "feels slow".
- (existing list: S7/S8/S10, PART-1..8, §30 backlog.)

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK;
ta_buf in apps; 8-experiment timebox; identity ONLY
Faded Dream Project <242450780+FemBoyGamerTechGuy@users.noreply.github.com>;
WIP push every step; webfetch specs, record URLs.

# §31 continuation — UPDATE (owner round 10: the wrong-row offset, the pegged core, the GPU question)

# §31 continuation — UPDATE (owner round 11: the dead rubber band, the Properties-asks-delete, the pegged core, the unselecting right-click)

## The four reports, four root causes (all fixed this round)

1. **"hold and drag to highlight does not work / I want multi-file
   selection"** — the 1.5.0 drag veto was checked on the press's HIT
   TARGET, but a press inside a list hits a ROW, never the list: the
   list's own veto (list.c:2301) was dead code. The controller fired
   pane_begin_drag at 6px, the DnD grab broke the band. FIX: vetoes on
   every UNSELECTED row (flipping with selection), the rows box, the
   scrollview + dynamic scrollbars (new fdk__scrollview_veto_input,
   re-run at every scrollview_layout), the iconview grid. Click-select
   stays icon+text-only (round 9's gate is untouched).
2. **"Properties asks me if I want to delete"** — item_activate read
   g_menu_acts[idx-1] against a 1-based registration: every entry
   fired the action ABOVE it. Plus: context menus re-registered into
   the 256-slot shared registry per popup (silent corruption after
   ~25 right-clicks). FIX: 0-based like the toolbar/shortcut twins;
   bounded 64-slot ctx ring; "PHASE: act" telemetry.
3. **"one core pegged when I hold-click fast"** — every fast sweep
   "began" a spurious DnD (window-tree walk per motion on X11), and
   the controller re-walked the whole widget tree per motion to find
   its armed source; the iconview repainted the whole view per motion
   pixel. FIX: root-cached armed source (O(1), destroy-safe), veto
   kills the spurious gestures, iconview band damage is old∪new +
   per-flip. perf_drag: 4.8% → 4.0% of one core.
4. **"the row of selecting a folder is not right"** — the right-click
   selected nothing; the menu flavored from the stale live selection.
   FIX: new public fdk_list_row_at / fdk_iconview_item_at (the same
   geometry the press/band use); the right-click selects the clicked
   row (multi-selection kept), flavor from the CLICKED item
   (CTX_FOLDER now reachable). menu rig proves: right-click beta-dir
   → "sel 1 beta-dir"; last entry → "act Properties".

## Verification this round
- make tools && make test (BOTH backends, X11+Wayland lib, ASan/UBSan):
  GREEN, 764 [ok] checks, incl. 4 new (veto interplay, gutter/
  scrollbar never-drag, O(1) armed-lookup mid-press destroy,
  row_at/item_at geometry).
- Click sweep (Xvfb XTest, 600ms-spaced): header y 155-175 → nothing;
  row 0 (180-205) → alpha.txt; row 1 (210-235) → beta-dir; 240+ →
  nothing. Pixel-exact.
- run_menurig.sh end-to-end: VERDICT PASS (Properties runs
  Properties).
- perf_drag: 4.0% of one core steady-state.
- KNOWN OPEN: the §28 drag rig (run_drig.sh) D1-D4 fail IN THIS
  SANDBOX (the sink process / app boot timing under ASAN Xvfb; no
  dnd-g12 trace lines ever appear — the drag never starts here).
  Engine-level drag-fires-from-selected-row IS covered by the new
  test_veto_row_selection_interplay. NEXT ROUND: re-run run_drig.sh
  in a clean boot; if it still fails, bisect against e092f17.

## Remote proof
origin/main == this round's commit, normal fast-forward push (round 9's standing rule holds:
never force-push). `git ls-remote origin main` is the authority.

## Incident (the second one — read before trusting any sandbox snapshot)
Round 10 was COMPLETED once already (code, tests, rigs, docs, all verified, commit cbc26fc)
but the session ended before its push, and the next sandbox recycle handed back an ALIEN
snapshot again: HEAD at the pre-round-8 force-push-era commit c1f27e9, with c71a7b0/9fc4fb1
not even present as objects, plus one uncommitted 169-line wayland_egl.c edit that was never
part of the true lineage. Recovery this time: the uncommitted edit is preserved as a patch
(/home/z/my-project/download/alien-wayland-egl-uncommitted.patch — the owner decides whether
any of it is wanted), the alien head is tagged locally as branch alien-snapshot-c1f27e9,
origin was fetched and main reset --hard to c71a7b0 (the round-9 head, intact on the remote
all along), and round 10 was REDONE on the true lineage and re-verified from scratch.
The lesson both incidents teach, now standing practice: the REMOTE is the only lineage
authority; a recycled sandbox's tree is untrusted until `git fetch origin && git status`
says it matches, and any round whose push failed must be assumed LOST, not "pending".

## The owner's three reports, one root cause + one architecture finding

1. **"Selecting something in the context menu is not right"** and
2. **"clicking a folder [in fdk-files] highlights the item below"** — SAME bug. The context
   menu flavors itself from the pane's LIVE SELECTION (`view_event`: `sel > 1 → CTX_MULTI,
   sel == 1 → CTX_FILE, else CTX_EMPTY`), so a selection that lands one row low makes the menu
   act on the wrong file. The pointer coordinates were NEVER wrong — the platform layers
   (X11 core + XI2 + Wayland seat, fractional viewporter) were audited first and are
   scale-correct by design; the Xvfb event trace proved widget-local coordinates exact
   (`row-DOWN local=(60,7)` for a click 7px into row 1's rect).
   ROOT CAUSE, engine: the 1.5.0 columns round inset the rows 26px below the header strip but
   left the rubber-band math dividing RAW list-local y by the row height. Every list-level
   press mapped one row LOWER than the pointer: a header press "selected" row 0, a press right
   of a row's text (round 9's gate falling through to the band) selected the row BELOW — the
   owner's exact wording. FIX: `list_content_top()` (the header inset, 0 without columns) now
   routes every band↔content mapping — anchor capture, apply-band row math, band rect, paint
   anchor, both auto-scroll edge tests (which also mis-measured "near the top edge" against
   the widget's 0 instead of the viewport). Six sites, one invariant: content space starts
   below the strip.
   Round 9's OWN test had enshrined the bug (part 3 pressed visual row 0 and asserted row 1 —
   written against the offset's output). Corrected to assert the visual row; `test_columns`'
   sweep coordinates (press at local y=15 = the header!) recomputed through the new
   `fdk__list_test_content_top()` seam; NEW `test_round10_header_strip`: R1 header click
   selects NOTHING, R2 right-of-text chase lands the VISUAL row, R3 sweep edges exact, R4 the
   same sweep at a NONZERO scroll offset (offset 3*rh, rows 4..6 — the path where a sign
   error would hide until scrolled; one lived briefly in band_rect and diff review caught it).
3. **"One core pegged during drags; GPU idle"** — two findings:
   a) X11 had NO `window_frame_ready` op (Wayland-only), so `fdk_surface_frame_ready()`
      short-circuited to always-true and every canonical `pump; if (frame_ready && damage)
      paint` loop painted UNPACED — one full acquire/raster/present per loop pass while a
      drag kept damaging (every motion). X11 motion COALESCING already existed (1.1.0-rc1,
      the XPeekIfEvent collapse) — the pegging was paint RATE, not event rate. FIX: X11 now
      ships the op — 17ms minimum present interval stamped by both render lanes (GLX and
      SHM/CPU), the 1.1.6 Wayland capacity gate's reasoning (both SHM slots in flight →
      not-ready, so the next acquire doesn't block), and two exceptions (nothing presented
      yet; popups — first-present latency is their correctness). Measured with the new
      perf_drag rig (250 XTEST motions/s, 6s, /proc/<pid>/stat sampling): steady-state
      13.2% → 4.4% of one core on the original run; 4.8% on the redo (same ballpark —
      the sandbox's noise floor, not a regression). The click sweep re-verified
      pixel-perfect on the paced build (a click is not a frame-rate event; no latency
      regression is possible from a 17ms gate on a discrete gesture).
   b) The GPU question — honest answer below, in "The GPU assessment".

## The round's rigs (new, all survive recycles in-tree)
- `test-apps/fdk-files/click_rig.c` + `run_clickrig.sh` — REAL XTest click sweep down a pane
  (DRIG layout), reading the app's `PHASE: sel` telemetry. Two lessons baked in for whoever
  extends it: (1) the telemetry itself had to be ADDED — main.c's header has promised
  "PHASE: sel every selection change" since §26, but only the boot smoke script ever printed
  it; the gated print now lives in `list_selection_changed` (panes.c, FDK_FILES_DRIG-gated,
  prints the selected count + lowest row's text AND the empty selection as `sel 0 (none)` —
  the rig must distinguish "nothing fired" from "fired with zero"). (2) the sweep must space
  clicks ≥600ms: at ~330ms consecutive same-row clicks consume as DOUBLE-clicks
  (FDK_WINDOW_DBLCLICK_MS=400) and the second of each pair activates (open/navigate) instead
  of selecting — a rig artifact that perfectly impersonates an app bug. And a third, paid for
  twice now: the sweep's FROM/TO are WINDOW coordinates — read them from the DRIG-GEO lines,
  never from memory (pane0's list sits at y=152..377 in this layout).
- `test-apps/perf_drag.sh` — the pegged-core prover: DRIG app under Xvfb, a 6-second
  250-motion/s held-button drag, /proc/<pid>/stat sampled every 500ms, steady-state deltas
  computed. Gotcha already paid for: `pgrep -f` matches the timeout(1) wrapper first — use
  `pgrep -x fdk-files`.
- Makefile: both rigs link against /tmp/my-project/local-x11 (the drag-rig line's old
  /tmp/x11dev path died with a recycle; that prefix no longer exists).

## Verification ladder this round (the REDO, on the true lineage)
- `make tools && make test` (both backends, X11+Wayland lib): **826 ok / 0 fail** —
  identical to the pre-recycle original run of this same round.
- test_list round-10 regression green (R1–R4); round-9 test corrected-and-green.
- End-to-end click sweep (Xvfb, XTest, 600ms-spaced): header 152–177 → nothing; row 0 rect
  178–208 → alpha.txt; row 1 rect 208–238 → beta-dir; empty space 240+ → nothing.
- perf_drag: 4.8% of one core steady-state (pre-pacing baseline 13.2%).

## The GPU assessment (the owner's question, answered straight)
"Shouldn't the GPU fans ramp when I hold-click-drag? The GPU does nothing; FDK files is not
GPU accelerated; only one core is pegged." All three observations are correct and they all
follow from one architecture fact: **FDK is a CPU rasterizer with a GPU present-only lane.**
Every pixel — text glyphs, rounded rects, alpha blends, the rubber band — is computed by
scalar C into a software framebuffer (src/render/surface.c; blend_pixel per-pixel
source-over). The GPU's only job, when the lane engages at all, is the final composite: the
damaged rects are uploaded as a texture and drawn on a fullscreen quad (GLX on X11 / EGL+GLES2
on Wayland, `FDK_GL_MIN_PIXELS=131072` gate — panels and popups stay on the CPU memcpy path
because a private context + vsync swap costs more than a few hundred KB of SHM). So during a
drag: the CPU rasterizes band-sized rects at up to the loop rate (the pacing bug made that
"unbounded"), and the GPU does one small texture upload + one quad per frame — single-digit
watts, fans never move. That is not a misimplementation of acceleration; it is the design.
Is it the RIGHT design? For this project's stage, yes: the retained-mode damage tracker +
per-rect invalidation keeps most interactions touching a few thousand pixels, the whole
renderer is portable and testable headlessly (the entire test suite runs under Xvfb),
and no GPU state lives in the app's address space to leak or crash. What "GPU-accelerated
FDK" would actually mean, in honest effort order: (1) SIMD rasterization (SSE/AVX2 blend
and fill paths, or a skia-style scanline core) — the biggest win per week of work, no GPU
required, helps the pegged core directly; (2) render-thread + damage batching — move the
raster off the input thread so motion never queues behind paint; (3) GPU-side damage
(two texture slots + scissored glTexSubImage2D, which the lane already half-owns) — cuts
the upload but NOT the raster; (4) true GPU raster (glyph atlases, tessellated rects,
MSAA) — a rewrite-class project, months, and it forfeits the headless test story. The
recommendation recorded for the owner: (1)+(2) next; the GL lane as-is is fine for
presenting. And on "one core": FDK is deliberately single-threaded for input+paint (the
thread-safety story is documented in docs/architecture.md); the fix for a pegged core is
doing LESS per event (this round's pacing: 3× less) and eventually the render thread —
not more cores.

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK; ta_buf in apps;
8-experiment timebox; identity ONLY Faded Dream Project
<242450780+FemBoyGamerTechGuy@users.noreply.github.com> (git config global+local set to it;
.git/hooks/pre-commit enforces it and commit-msg strips any Co-Authored-By/Signed-off-by);
WIP push every step; webfetch specs, record URLs; NEVER force-push (round 9's standing rule).
# §31 continuation — UPDATE (owner round 9: the five reports, ported onto the restored lineage)

## Remote proof
origin/main == this round's code commit + docs commit. Tree clean at that commit.

## This round's first act: the lineage itself
Round 9 began with an incident, recorded here because it shaped the round. A mid-session sandbox
recycle handed the working tree a DIVERGED snapshot of the repo (a copy-fork whose HEAD had no
test-apps/), and the session committed and FORCE-PUSHED that fork's head over origin/main,
displacing round 8's head (9fc4fb1). The owner caught it. Recovery: the true head was re-fetched
from GitHub by full SHA (the object was still servable), main was reset and force-pushed BACK to
9fc4fb1, and the alien lineage's one commit (2263ee9 — the five fixes, already written against
the wrong tree) was preserved on the local branch backup/round-9-alien. This round's real work
was then PORTING those five fixes onto the true lineage. Standing rule from the owner, effective
permanently: **never force-push again** — normal fast-forward pushes only; a remote/local
divergence is a stop-and-reconcile event, never an overwrite.

## Round 9 delivered (the owner's five reports, engine-level)
1. **STUCK BANDS (gesture-cancel sweep)** — when a menu chain dies (outside-click CLOSE_REQUEST,
   popup_done), the popup swallowed the POINTER_UP, so any live rubber band's highlight stuck on
   screen. `fdk__list_cancel_bands_all_roots()` (list.c, seam in widgets_internal.h) now fires
   from menu.c's teardown paths (session_close_above level-0, menu_popup_destroyed): walks every
   root, ends every live band exactly as an UP would. PORT EXTRA beyond the alien version: it
   also clears the rows' pending_band flags (1.5.0's row-born gestures) — a stale armed row
   would have synthesized a phantom band from stale coordinates on the next motion.
2. **ROW HIT BANDS (presses right of the content are empty space)** — a row now accepts presses
   only over icon + name text + padding (`row_press_on_content`, columns-aware: the 1.5.x column
   cells ride the row's full-width PAINT rect, but presses there are the LIST's). A press right
   of the text falls through to the list and starts the rubber band there. The round-6
   press-time chase semantics are PRESERVED (a no-motion click at a row's y still selects that
   row — pinned by test_row_press_band and now by test_round9_ports).
3. **MENU STORMS (single-session context menus)** — `fdk__menu_popup_open_full` closes every
   other bar-less (context) session before opening; the 4-slot table can no longer be reached
   from popups. MenuBar chains are a different gesture class and keep their own rules.
4. **MENU MISFIRES (the opening-release guard)** — menu views now press-ARM: a POINTER_UP
   without a press this view saw is swallowed without activating. This kills the
   "Properties acts like Delete" class: the menu popped under the still-held button and the
   release chose whatever row sat there (GTK/Qt swallow this exact release too).
5. **REPAINT COST (damage-limited band)** — apply_band no longer invalidates the whole list per
   motion pixel: `fdk__widget_invalidate_rect` (new widget.c seam, widget-local rect clipped to
   bounds) carries the union of the old + new band rects; list_set_row already damages only the
   rows that FLIP. Large directories no longer peg a core during sweeps.
6. **POPUP TEARDOWN DAMAGE (the black box)** — popup windows now capture their anchor rect in
   parent-client coords at creation (popup_anchor_x/y/anchored, window_internal.h) and
   fdk_window_destroy damages that rect in the parent's tree: X11 popups unmap off the ROOT, so
   the parent never got the Expose for the region the menu covered.

What was NOT ported, deliberately: the alien commit's wayland_egl.c NVIDIA upload-path work
(BGRA/UNPACK_ROW_LENGTH probing, RGBA twin, pack scratch) — the TRUE lineage already carries it
(it originated here at 1.0.0 and evolved through the GLVND round-7 work; the alien fork's copy
was a re-derivation). Same for its examples/10_file_manager.c demo: fdk-files' own menus.c has
had the real context menu (Open / Open with… / Rename / trash / permanent delete, each wired)
since the §26 rounds.

## Tests
test_round9_ports (tests/test_list.c): presses right of the content start bands not row-clicks
(content presses still select), the cancel sweep ends swallowed gestures (redundant-safe,
stray-UP-safe), the gate holds under 1.5.x columns with the far-end anchor active. Full suite
green (`make test`, ASan/UBSan clean). fdk-files relinked and boots (app_id
org.fdk.testapps.files, PHASE: boot).

## Next round candidates
- His X11 boot-slowness report (round-6 item C) — STILL OPEN.
- The E2E XTest self-drop proof (round-6 leftover).

# §31 continuation — UPDATE (owner round 8: the columns' far-end anchor + the folder type)

## Remote proof
origin/main == this round's code commit + docs commit. Tree clean at that commit.

## Round 8 delivered (the owner's three reports)
1. **THE COLUMNS HUG THE FAR END + FOLLOW RESIZES (engine, 1.5.1)** — round 7 summed widths from
   the left, so the fixed columns clustered right after the shortest filename and never moved on
   resize. The layout is now RESOLVED, not summed: `list_columns_layout()` runs on every viewport
   change (arrange / the app's set_bounds / the paint self-heal — all funneled through one sync)
   and picks one of two modes. ANCHORED: the last fixed column's right edge sits at the viewport's
   end (clear of the v-scrollbar's reserved strip), the name column stretches across ALL the slack.
   CONTENT (window narrower than the natural spread): natural widths, one separator apart, with the
   h-scroll floored at the spread — narrow windows degrade into a scrollable pane instead of
   colliding with the names. The header gained vertical divider hairs between the fixed columns.
   ENGINE BUG this round exposed + fixed: the paint-tail self-heal had its own inline scroll
   set_bounds that BYPASSED the shared sync — the anchor never re-resolved on a direct set_bounds
   resize; it now routes through list_sync_scroll_bounds like every other path.
   Tested: right-edge anchor == viewport, widen-to-600 re-anchors, narrow-to-160 flips to content
   mode with the spread floor, re-widen re-anchors, selection under columns, clear-to-plain.
   Rig-verified on BOTH backends: name→Modified gap 571px (X11 1080px window) / 767px (Wayland
   1280px window) vs round 7's ~150px; right edges at the window end minus scrollbar+pad.
2. **THE FOLDER TYPE (app fix)** — mime_category had no inode branch: "inode/directory" fell
   through every category and returned "File". Directories now read "Folder" (and other inode/*
   entries "System"). Rig-verified: the folder row shows no Size (dirs) and a distinct Type cell.
3. **WAYLAND SUPPORT CONFIRMED** — fdk-files runs the Wayland backend natively (the owner's own
   logs have always been backend=wayland); this round adds the proof rig: headless sway + grim
   screenshot (attached to the report) showing the same far-end column geometry on Wayland, and
   the Wayland self-quit under LSan is STILL zero-leak exit-0. X11 self-quit: only the documented
   Mesa/GLVND ~108KB rig class (round 7) — unchanged, third-party, absent on the owner's NVIDIA X11.

## Rig index (additions)
grim lives at /home/z/apt/prefix/usr/bin/grim (restore_wayland_toolchain.sh); run it while the app
is ALIVE (a compositor screenshot is of live surfaces), with LD_LIBRARY_PATH to the prefix.
NOTE for the X11 rig: `env -u WAYLAND_DISPLAY` when a sway session is running in the same shell,
or the app will prefer the Wayland backend (the backend picker prefers WAYLAND_DISPLAY).

## Next round candidates
- His X11 boot-slowness report (round-6 item C) — STILL OPEN.
- The E2E XTest self-drop proof (round-6 leftover).

# §31 continuation — UPDATE (owner round 7: the list-view columns + the exit leaks)

## Remote proof
origin/main == this round's code commit + docs commit. Tree clean at that commit.

## Round 7 delivered
1. **LIST COLUMNS (engine, 1.5.0)** — the file-manager list view as a first-class fdk_list
   capability, NOT app code: `fdk_list_set_columns()` (fdk_list_column: title/min_width/
   align_left; column 0 = the primary text column, expands, keeps the v1 contract; columns
   1..N fixed-width right-aligned default), `fdk_list_row_set_cell()`, `fdk_list_get_column_count()`.
   Geometry is PAINT-TIME on the existing row widgets (no per-cell children — hit-testing,
   the implicit grab, the rubber band all untouched). Header strip pinned above the scrollview
   (inset via one list_sync_scroll_bounds rule), titles ride the horizontal scroll, cells render
   in text-disabled. Widths: declared → widened to the title → widened per cell (never clips a
   right-aligned tail); columns_width floors content_width AND the natural width. Exports
   1001 → 1004. make test green incl. new test_columns (API/geometry/selection-under-columns/
   clear-to-plain). ALSO FIXED a latent bug my code exposed: row_paint computed the owning list
   as list_of(SCROLLVIEW) — garbage since v1, harmless until something read it; now walks the
   chain to the real list class.
   The APP (fdk-files) only supplies data: Name | Modified | Size | Type (mime_category),
   human sizes, "Nov 12, 18:20" this-year dates — through the public per-row API beside the
   icon loop, on every rebuild path. Visual rig pixel-verified: header + rule + right-aligned
   cells, folder row shows no size, right edges align.
2. **EXIT LEAKS (his round-7 log)** — the dnd_uri leak (round 6) confirmed GONE on his log.
   Remaining classes were ALL third-party driver state:
   - libEGL_nvidia 4x256B + libdbus 184/646B (NVIDIA's EGL pulls libdbus transitively; FDK
     itself NEVER links/dlopens libdbus — its D-Bus is a self-implemented socket protocol):
     freed by the library's dlclose destructors, which the pinned-forever policy skipped.
     FIX: fdk_wayland_egl_libraries_release() — dlcloses libEGL AFTER wl_display_disconnect
     (the display must be dead first; the same rule the X11 lane taught us below).
     PROVEN: headless-sway self-quit LSan = ZERO leaks, exit 0.
   - A cell-leak in MY new columns code (row_destroy walked a parent chain that destroy
     ALREADY unlinked — the tree walker's rule): rows now free their own cells by cells_cap
     recorded at every alloc/grow site. Same rig: zero.
   - The X11 twin (libGLX_mesa ~108KB on the llvmpipe rig): dlclose(libGL) after
     XCloseDisplay runs clean BUT Mesa/GLVND holds the vendor lib with its own ref
     (dlclose rc=0, state survives) — GLVND internals, rig-specific: the owner's NVIDIA
     X11 runs showed NO such class (round-6 X11 log leaked only FDK's dnd payload).
     The hook (fdk_x11_gl_libraries_release) stays as a documented no-op. THE SEGV LESSON:
     dlclose BEFORE XCloseDisplay crashes (Xlib's GLX extension cache lives inside libGL) —
     recorded in the code so nobody repeats it.

## Rig index (this round)
headless sway: WLR_BACKENDS=headless + XDG_RUNTIME_DIR=/home/z/apt/xdg-run + LD_LIBRARY_PATH=
/home/z/apt/prefix/usr/lib/x86_64-linux-gnu (restore_wayland_toolchain.sh rebuilds the prefix;
fix the local-x11 xrandr.pc prefix after a reset — it ships prefix=/usr). Self-quit LSan rigs:
FDK_TESTAPP_FRAMES=120 + FDK_FILES_STARTUP_DIR on each backend. Column visual rig:
Xvfb :99 + x11_shot.py, pixel-assert via spans (bgs {(35,35,41),(41,41,49)}).

## Next round candidates
- His X11 boot-slowness report is STILL OPEN (round-6 item C, unstarted).
- The E2E XTest self-drop proof (round-6 leftover; rig was flaky).
- Column WIDTHS could later ride the theme metrics (FDK_TM) if a theme ever wants them.

# §31 continuation — UPDATE (owner round 6: DnD self-drop, drag leaks, multi-select freeze)

## Remote proof
origin/main == this round's code commit + docs commit (see git log). Working tree = clean at that commit.

## What round 6 fixed (ALL ENGINE-LEVEL, no app hardcoding)
1. **X11 self-drop deadlock**: the drop's selection read (wait_selection_notify_on) waited for
   SelectionNotify but never ANSWERED the SelectionRequest our own drag source received on the
   clip_helper — same-process drops timed out at 1000ms and delivered an empty payload (his two
   "owner did not answer" WARNs). FIX: the wait loop now peels SelectionRequests off the wire and
   serves them (fdk_x11_dnd_serve_selection → clipboard fallback), XPutBackEvent for everything
   else. THE E2E XTEST PROOF IS STILL PENDING (rig issue, see below).
2. **Drag payload leaks (both backends)**: x11 dnd_source_finish and wayland drag_source_report
   now free text/uri_payload at drag END (before: freed only at teardown; a second drag
   overwrote the pointers — his 256-byte LSan report). Code-verified; 2-drag LSan gate pending.
3. **Multi-select freeze/never-works**: THREE interacting causes, all engine:
   a. The rubber band only started on EMPTY-space presses; a full directory is wall-to-wall rows
      → unreachable. FIX: row_handle_event now holds a plain press on an UNSELECTED row as a
      "pending band"; motion past 6px slop converts it to the list's band (row keeps the tree
      grab and forwards motions/release/grab-broken, re-localized); release inside slop = the
      classic click-select. PROVEN: new headless test (test_row_press_band) + E2E rig: S1 sweep
      selects 5 rows (12336 selection px vs 2700 on HEAD).
   b. The widget drag-source controller armed at press BEFORE delivery, so a band press ALSO
      fired a drag at 6px → the X11 drag's grab ate the release → banding + 100Hz autoscroll
      timer stuck → next press dead = THE FREEZE. FIX: new public API fdk_widget_set_drag_veto()
      (a widget owning press gestures vetoes arm-at-press); List sets it automatically in
      MULTIPLE mode; drag-from-content now starts only from rows the app arms via the new
      fdk_list_row_set_drag_armed (fdk-files arms SELECTED rows on every selection change — the
      desktop-standard press-selected-row-and-move gesture).
   c. No notification when a drag stole the pointer: new public event FDK_WIDGET_GRAB_BROKEN,
      delivered by fdk_drag_begin_ex to the tree's grabbed widget (root->last_pointer_pos added);
      List ends the band + timer on it. HEAD E2E proof: after a drag attempt, S3's press did
      nothing (frozen, 2916→2916 px); FIXED: S3 selects fresh (12336→2789 px = responsive).
4. Exports: 998 → 1001 (+set_drag_veto +get_drag_veto +fdk_list_row_set_drag_armed). make test
   ALL GREEN including the new band test (note: the suite's lists need a paint pass for layout —
   the new test does fdk_widget_tree_paint first; the OLD band test worked because 0x0 scroll
   made presses hit the list directly — it never exercised row hit-testing).

## NOT FINISHED (next round's first jobs)
1. **E2E self-drop XTest proof**: the stepped-motion rig (scripts/repro_s4.sh) produced NO dnd
   trace at all (not even the release line) — the rig or window-finder is flaky, the fix itself
   is compiled and unit-sound. Debug the rig: check WID, check the app is pumping, one motion
   per driver call with w:40 spacing, then verify folder receives the file + no WARN lines.
2. **2-drag LSan gate** for the leak fix (run the s4 rig twice + exit, expect NO 256-byte
   dnd_uri leak).
3. **FIX C — X11 boot slowness**: NOT STARTED. His log: ~2s gap before "GPU present lane
   active" (=GLX/NVIDIA context creation) + ~1s to self-check pass. Plan: strace -ttt -e
   trace=openat,poll,recvfrom,sendto the boot, diff against wayland; suspects: dlopen of
   libGLX_nvidia + first glXMakeCurrent + any XSync roundtrips in x11_gl bring-up; the
   self-check itself is 9ms timer-paced (NOT the stall).
4. The icons-mode smoke + both-theme self-quits were NOT re-run after round-6 engine changes
   (make test covers the widget layer; re-run them next round before closing).

## Rig index: scripts/repro_r6.sh (band+freeze E2E), scripts/repro_s4.sh (self-drop, flaky),
scripts/xtest_driver (built, needs LD_LIBRARY_PATH=/tmp/x11dev2/root/usr/lib/x86_64-linux-gnu),
scripts/x11_find_window.py (ctypes XQueryTree finder).

# §31 continuation — UPDATE (owner round 5: the round-4 registry's use-after-frees)

## Remote proof
origin/main == `82435e2` ("the round-5 use-after-frees"). Working tree = that commit, clean.

## Round 5's two fixes (both bugs were MINE, introduced in round 4)
1. **ROW-ICON OWNERSHIP** — list/iconview rows BORROW images; my round-4 registry freed
   the clones per pane_build_view. A rebuild of ONE pane (pane_add / split view) freed
   what the OTHER pane still painted. Repro: `--smoke` on HEAD (the split round) → ASAN
   heap-use-after-free in row_paint. Fix: `icons_get_file()` in the app icon cache
   (resolves via the round-4 public engine API, caches by TYPE = directory/extension);
   cache owns surfaces for the process lifetime. Registry deleted.
2. **STALE THUMBNAIL DELIVERY** — the icons-mode smoke then died in thumb_ready_cb: a
   worker thumbnail delivered into a destroyed iconview (smoke navigates mid-flight; the
   request captured the widget pointer). Fix: `tab.view_gen` bumped by every
   pane_build_view; requests carry (tab, gen, iv) in a heap thumb_target; deliveries
   drop on stale generation BEFORE touching widgets. `thumbs_get` grew `out_submitted`
   so the caller knows if the worker owns its user pointer.

## Rig note (do NOT re-add): single-pane self-quit rigs CANNOT see either bug — the
smoke rig (`--smoke`) is the regression test that catches both (detail mode = bug 1,
icons mode = bug 2). The temp FDK_TEST_VIEW_ICONS override was reverted; to test icons
mode use the prefs quirk knowledge: view.mode is saved but never applied on boot (app
quirk, line ~641 discards the read) — the menu is the only way in the shipped app.

## Verification state: smoke (detail+icons) → PHASE:quit zero ASAN; make test GREEN;
exports 998; Papirus self-quit 22/22, Adwaita 19/21 (the 2 = names Debian's Adwaita
lacks); icons-mode screenshot 6830 saturated px (#5294e2 cluster); leaks = libGLX only.

## Owner re-test: ./build/fdk-files — boot, split view (F9 or the split toolbar button),
switch to icons view, navigate around while thumbnails load, close. No ASAN abort; both
panes keep themed icons through each other's rebuilds.

# §31 continuation — UPDATE (owner round 4: the Adwaita crash + invisible file-row icons + the per-entry API)

## Remote proof
origin/main == `b21d109` ("the round-4 trio"). Working tree = that commit, clean.

## Round 4's three fixes
1. **CRASH (ASAN stack-use-after-scope, icons.c:233)** — round-3's plain-before-symbolic
   lookup declared `sym` inside the fallback `if`; `which` borrowed it past the block and
   was strdup'd at the cache store. Fires ONLY when the plain name misses: Papirus ships
   plains (rig never fell in), the owner's Adwaita is symbolic-only (every boot). `sym`
   now lives at iteration scope.
2. **FILE-ROW ICONS** — the bound list carried text only; no icons in any view on any
   theme (his words: "the folder dont even have a folder icon at all"). The list branch
   decorates after bind_model on every rebuild path (navigate/back/fwd/up/inotify/filter
   all funnel through pane_build_view); iconview resolves the same way. Surfaces are
   caller-owned clones tracked in a registry, freed when the view rebuilds.
3. **PUBLIC API** — `fdk_icon_theme_file_surface(dir, name, is_dir, size)` in
   fdk_theme.h: the file dialog's per-entry resolution exported (extension table, folder
   chains, one-stat executable split). Answers whatever the INSTALLED theme ships.

## Theme-genericity (the "not just one theme" concern — it never was theme-specific,
   but now it is PROVEN on two): Adwaita via gtk-ini 18/20 system hits (2 misses are
   names Debian's Adwaita doesn't ship — verified against its file tree); Papirus via
   ~/.icons+default/index.theme 20/20. Both self-quit cleanly; leak report has zero
   engine frames (libGLX_mesa's 108124-byte globals only, unchanged since round 2).

## 16px Papirus note (do not "fix" this): Papirus's small-size SVGs are currentColor +
.ColorScheme-Text (#444444) BY DESIGN — the colored artwork is 32px+ (icon view shows
color; list rows show the theme's own monochrome small set, same as GTK managers).

## Rig map: /tmp/hisbox (Papirus shape), /tmp/adwaitabox (gtk-ini shape), both with
populated HOME (Documents/Downloads/…, .txt/.c/.png/.zip). X11 dev at /tmp/x11dev2
(.pc prefixes repointed); wayland via scripts/restore_wayland_toolchain.sh; make test
needs FDK_ENABLE_WAYLAND=1 + `make tools` FIRST (test_cli_tools asserts fdk-theme).

## Owner re-test: ./build/fdk-files →
- no ASAN abort; sidebar icons themed; FILE rows now carry icons (16px themed set);
  icon view (View menu) shows colored 32px themed artwork
- on quit: `icons: N system hits / M lookups` (Adwaita box: N≈18, M=20 is a PASS —
  view-compact/view-hidden are not in Debian's Adwaita)

# §31 continuation — UPDATE (owner round 3: "icons even in the side bar aren't using system themes")

## Remote proof
origin/main == `77be5c7` ("icons/sidebar: the app never asked the theme — now every icon surface does").

## The finding that mattered
His round-3 log carried our round-2 verdict line: `icons: theme
'Papirus-Nordic' via search-path default` — DETECTION WORKS. The rig
repro with the REAL Papirus package in his exact box shape resolved
12/12 toolbar names SYSTEM tier. **The engine was never broken — the
app never asked.** The places sidebar, devices, bookmarks, trash and
icon-view rows drew procedural `FDK_ROW_ICON_*` glyphs / plain
buttons; no `icons_get` existed for any of them. FDK's own file
dialog had the themed-raster pattern since 1.5.x, but the seam was
engine-internal so an app COULDN'T copy it.

## What changed
1. `fdk_list_row_set_icon_surface()` is now public API (wraps the
   internal seam). The app's places/mounts/bookmarks rows carry real
   freedesktop names and resolve 16px themed rasters, glyph floor
   kept. Icon-view file rows resolve themed type icons (folder,
   image-x-generic, ...). His-box rig: **20/20 system hits**.
2. Toolbar lookup is PLAIN-FIRST, symbolic only as fallback (the old
   symbolic-first order tinted everything flat monochrome — "looks
   like not my theme" while hitting it every time). NOTE: Papirus's
   16px ACTION icons are near-monochrome by design (currentColor +
   .ColorScheme-Text); the colored ones are the places/folder icons,
   which now render colored (pixel-verified on the rig).
3. Self-diagnosing logs: WARN when the detected theme has no
   index.theme on the search path (dangling Inherits — the case that
   silently served hicolor), and a shutdown one-liner
   `icons: N system hits / M lookups` via public `fdk_log`.

## Rig notes (post-sandbox-recycle rebuild)
- X11 dev prefix now /tmp/x11dev2 (debs + .pc prefixes repointed);
  scripts/hisbox_run.sh reproduces the owner's box shape; real
  Papirus from apt (22.6 MB) parked at /tmp/pkg/papirus-root.
- Wayland toolchain restored via scripts/restore_wayland_toolchain.sh
  (make test needs FDK_ENABLE_WAYLAND=1 + `make tools` first —
  test_cli_tools asserts the fdk-theme binary exists).
- A GLM-authored divergent line was found on the local tree
  (c1f27e9 + dirty wayland_egl.c): preserved on branch `glm-line`
  + stash; DO NOT git add -A on this tree — untracked fdk-shell/
  leftovers from that line will be swallowed.

## Owner re-test
`./build/fdk-files` and look for:
- `icons: theme 'Papirus-Nordic via ...` (detection verdict)
- the sidebar rows now carrying Papirus artwork (colored folder /
  house / drive icons), toolbar buttons the theme's own glyphs
- on quit: `icons: N system hits / M lookups` (N > 0 = the desktop
  theme is being served)
- if a WARN about a missing index.theme appears, the theme name is
  dangling — install it or fix the setting it came from.

# §31 continuation — UPDATE (owner round 2: "still not used" + unclosable Help window + the 2.4 MB ASAN leak)

## Remote proof
origin/main == `ea020af` ("icons+app: the unclosable Shortcuts window, the 2.4 MB icon-surface leak, and the one-line detection verdict").

## The three findings, each with its broken assumption
1. **The unclosable Shortcuts window** — fdk_event.h says FDK does NOT
   destroy on close request (the app decides); menus.c believed a
   comment claiming "auto-managed". Now sc_list_window_event destroys
   on CLOSE_REQUEST or Escape (the props_close pattern). This was also
   the "shutdown with 1 window(s) still open" WARN.
2. **The 2.4 MB icon leak** (owner ASAN: 925 surfaces via icons.c:190/193)
   — fdk_icon_theme_surface() returns a FRESH OWNED CLONE per call;
   the app treated it as borrowed. The app cache now holds system hits
   under "S|name|px" and frees them at teardown. Self-quit under
   detect_leaks=1: ZERO FDK leaks (only libGLX_mesa's 108 KB one-time
   globals — inside libGL, not ours).
3. **"Still not used"** — the resolver WAS working on his machine (925
   system surfaces allocated); the verdict was invisible because
   FDK_ICONS_TRACE was not set. The chain now logs ONE INFO line:
   `icons: theme '<name>' via <channel>` on first detection. ALSO:
   the symbolic tint bakes the color theme's ink at decode time and
   the surface cache never expired on a color-theme switch —
   install/uninstall now call fdk__icon_theme_retheme() (surface-cache-
   only reset; detection/lookup/name caches stay).

## Owner re-test (one command each)
- `./build/fdk-files` → the log must now contain
  `icons: theme '…' via …` (which theme, which channel).
- Help > Shortcuts → the WM close button / Alt+F4 / Escape all close it.
- Quit under ASAN → no LeakSanitizer block (or only the libGLX_mesa
  108 KB system-library globals).

## Rig notes
- /tmp/shotapp.sh now honors RIG_ASAN (default detect_leaks=0), RIG_FRAMES,
  RIG_WAIT, FDK_ICONS_TRACE, RIG_DATA. THE LEAK GATE IS detect_leaks=1 +
  A SELF-QUIT (small FDK_TESTAPP_FRAMES) — a rig-timeout kill hides
  nothing anymore but the self-quit is the proof.
- The remaining non-FDK leak (libGLX_mesa globals) is expected on this
  image; note it in reports so it is not chased as ours.

# §31 continuation — UPDATE (owner round: "icon theme not working at all" + sidebar.c:344 warning)

## Remote proof
origin/main == `238ccc4` ("icons: the THREE real reasons the toolbar was blank on a real desktop").

## What the owner's report exposed (all fixed in 238ccc4)
1. **-Werror had NOT actually landed** in test-apps/Makefile — the
   scripted edit silently missed; the owner's build line proved it.
   Really applied now; a canary (injected unused function) FAILS the
   build. The rig now re-greps the MAKE OUTPUT, not the file, for
   "warning:".
2. **sidebar.c:344 -Wformat-truncation**: newer gcc computes the
   array-SPANNING bound of g_bms[i].path. All fixed-buffer copies in
   sidebar.c go through ta_copy/ta_join now.
3. **The icon claim was FALSE, and here is why my verification lied**:
   - openbox was silently EXITING ("Unable to load a theme") in the
     rig — every "openbox" run was BARE Xvfb. It now gets a working
     Clearlooks theme in a fake HOME; EWMH verified up first.
   - The "13 clusters" in the old screenshots were menubar TEXT, not
     icons: FDK_ICONS_TRACE printed ZERO lines because
     icons_get was NEVER CALLED — the icon button's canvas child had
     EMPTY bounds (sized only in ta_icon_buttons_relayout, which
     runs on FDK_EVENT_WINDOW_CONFIGURE, an event that never arrives
     after the callback is registered) → paint_rec early-outs on
     rect_empty → THE BLANK TOOLBAR (S1's true root cause). Fixed:
     bounds set AT CREATION (the button's natural 32x28) + one boot
     relayout pass after menus_build.
   - The BUNDLED tier never rendered either: the embedded SVGs went
     to fdk_surface_create_from_image_bytes (stbi = raster only) →
     "image decode failed" WARNs. FDK gained the public API
     fdk_surface_create_from_svg_bytes() (nanosvg core factored from
     fdk__icon_svg_raster; test-first tests/test_svg_bytes.c;
     exports 995→996); icons.c routes SVG bytes to it.
   - Fallback-name chain added per Part 2.2 (view-hidden→visibility,
     view-compact→view-list-compact/view-list-details, view-grid→
     view-list-icons, system-search→edit-find, user-trash-full→
     user-trash, folder-*→folder): themes legitimately lack names
     (Adwaita has NO view-hidden/view-compact at any size; breeze has
     no view-list).

## The honest verification now (rerun these commands after any change)
- Rig = /tmp/shotapp.sh (recreate after /tmp wipe; it needs openbox
  debs in /tmp/iconsroot/root with a WORKING theme in fake HOME,
  netpbm+x11-apps in /tmp/pbroot, RIG_DATA for the icon dir,
  gtk-3.0/settings.ini in the fake HOME declares the theme — that is
  how a real desktop states it when no XSETTINGS daemon runs).
- Trace gate: `FDK_ICONS_TRACE=1` → expect 12 "icons: PAINT" and
  tier lines; Papirus = 12/12 SYSTEM; breeze = 11 SYSTEM + 1 BUNDLED
  (view-list); Adwaita-only = 10 + 2 (eye, view-compact). NO
  "decode failed" WARNs, no runtime errors.
- Pixel gate: toolbar band at window-top+30 must contain ≥8 runs of
  icon width (8..20 px); pre-fix shots have ZERO. Cross-theme cell
  hashes differ where the themes' glyph art differs.
- make test GREEN; make verify-exports = 996; apps -Werror clean;
  build/sidebar-test GREEN.

## Still open from §31 (unchanged except S10/S8 data point)
The rig logs show a "no D-Bus address resolves" INFO every ~2 s for
the app's lifetime (the notify client's try_connect re-arms; find
what ticks it every 2 s — that is ALSO the S8 idle-frame suspect).
Then: sidebar_view.c, PART-3 toolbar polish (24 px in 36×36 — the
buttons currently paint 16 px icons in 32×28), PART-4 artifacts,
S7 grab, S8 idle, S10 endpoint chain, --diag, PART-8 audit.

# §31 continuation state — FIX fdk-files (round cut; read this first)

## Remote proof
origin/main == `91e2b94` (the §31/PART-5 sidebar model). `git ls-remote origin main` is the authority.

## The real-conditions rig (PART V) — rebuild after a sandbox recycle
/tmp is wiped on recycle. Rebuild:
1. `bash scripts/setup-sandbox.sh` (x11dev, wayland prefix, rigs, hooks).
2. User-space desktop stack (apt-get download + dpkg -x <deb> root):
   openbox libobrender32v5 libobt2v5 libstartup-notification0 libimlib2t64
   libxcb-util1 adwaita-icon-theme papirus-icon-theme breeze-icon-theme
   hicolor-icon-theme → /tmp/iconsroot/root; netpbm libnetpbm11t64
   x11-apps → /tmp/pbroot.
3. Runners (in /home/z/my-project/scripts, survive recycles):
   realui-run.sh = Xvfb 1920x1080x24 + openbox (EWMH) + private
   HOME/XDG_DATA_DIRS with the chosen theme + no D-Bus + UBSAN
   halt_on_error + the log gate. /tmp/shotapp.sh (recreate on
   recycle): same stack, FDK_TESTAPP_FRAMES=10000, sleep 6, xwd -root
   | xwdtopnm | pnmtopng (netpbm from /tmp/pbroot, LD_LIBRARY_PATH
   /tmp/pbroot/usr/lib/x86_64-linux-gnu and
   /tmp/iconsroot/root/usr/lib/x86_64-linux-gnu for openbox).

## DONE and verified this round (commit → what)
- 0b9bed6 — S5 root cause: the first pane "duplicates the active
  tab's dir" but AT BOOT there is no tab → NULL deref; falls back to
  HOME now. Bonus: fsops.c:87 qsort(NULL,0) UB on an EMPTY $HOME
  (guarded, n==0). Log gate clean since (UBSAN halt + WARN scan).
- c028275 — S6: FDK's shortcut parser grew the xkb keysym names
  (question, semicolon, apostrophe, grave, backslash, bracketleft/
  right, equal), Prior/Next, F13–F24, KP_*, and the LITERAL-SYMBOL
  grammar ("Ctrl+?" = Ctrl+Shift+slash, the US shifted-glyph rule,
  documented in-code; trailing "Ctrl++" = the plus key). Test-first;
  272 format round-trips; make test + dbus GREEN.
- be56c5f — S9: fdk-edit's warnings fixed PROPERLY (unsigned-domain
  clamp files.c:259; (void) fixed-signature params; dead
  on_pref_toggle removed). test-apps Makefile: -Werror ON. Verified
  ZERO warnings gcc+clang19 × -O0/1/2 × 4 apps.
- e0b059d + e463248 — S3 / PART-2 core: icon priority INVERTED —
  system theme FIRST (bundled stems → freedesktop standard names,
  -symbolic preferred, comes back tinted), EMBEDDED bundled SVG
  second, glyph floor third. FDK gained fdk_icon_theme_name()/
  source()/set() (the --diag channel report; exports 992→995),
  test-first. Rig proof: 13 toolbar icon clusters under EVERY theme
  (Papirus/Adwaita/breeze/hicolor). Specs webfetched+recorded:
  http://specifications.freedesktop.org/icon-theme/latest/ and
  http://specifications.freedesktop.org/icon-naming/latest/
- 91e2b94 — S4 / PART-5 MODEL half: sidebar.c/.h (XDG places
  exist-checked, Trash IN Places w/ full-empty icon; bookmarks own +
  shared gtk-3.0 format merged; devices from /proc/self/mountinfo
  with the full exclusion table, /run/media + /run/mount exceptions,
  by-label→by-partlabel labels with lexical dot-dot normalization,
  statvfs usage, removable incl. nvme suffix strip, correct icons).
  `make build/sidebar-test` GREEN on the spec's fixture. make test
  GREEN; zero warnings.

## NEXT ACTIONS (exact order)
1. sidebar_view.c — the VIEW half (API in sidebar.h: sb_build/
   sb_refresh/sb_highlight_path/sb_bookmark_add): one scrollable
   column, 220px + splitter + persisted width; 11px dim bold headers
   (12px above except first, 4px below, nothing else); 32px rows,
   20px icon @ pad 8, 8px gap, ellipsized + full tooltip; accent +
   current-folder highlight; Up/Down/Enter; context menus; 3px usage
   bar (5s + on mount change); mountinfo POLL (POLLPRI|POLLERR) →
   sb_refresh. Then DELETE panes.c build_places, wire panes_build →
   sb_build. Geometry test (DRIG-GEO style): 32px pitch, margins.
2. PART-3 toolbar: 24px in 36x36, separators nav|view|fileops|search,
   disabled dimmed Back/Forward/Up, the Icons/Icons+text/Text pref.
3. PART-4 artifacts (S2): search entry's magnifier + conditional
   clear icon — canvas transparent-clear, damage covers the icon,
   pixel test outside-glyph == button background.
4. S7 modal grab bounded retry (MapNotify, ~100ms, WARN only on
   final failure); S8 idle ≤2 frames/2s (8381-frames bug; damage-
   limit caret blink; find the always-invalidate source) + time-to-
   first-frame (~2s pre-GPU-lane); S10 D-Bus endpoint chain (env →
   $XDG_RUNTIME_DIR/bus → X11 root property → ~/.dbus/session-bus,
   log candidates tried, no-bus stays INFO).
5. PART-1 --diag in all 4 apps (uses icons_last_tier +
   fdk_icon_theme_source; idle-frame counter; VIOLATION lines) +
   RUN-ON-YOUR-DESKTOP.md + the log gate (log-allowlist.txt, per-line
   reasons) in the smoke targets.
6. PART-8 widget-tree audit (a11y, tooltips, NONE-tier, contrast
   ≥3:1 per FDK theme × state incl. Solar Flare, clipped labels,
   spacing, 24px minimums, Tab reachability) in smoke + --diag.
7. Only then the §30 backlog (fdk-monitor, fdk-notes, PASS B,
   PRE-1.0 audit, chases).

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK;
ta_buf in apps; 8-experiment timebox; identity ONLY
Faded Dream Project <242450780+FemBoyGamerTechGuy@users.noreply.github.com>;
WIP push every step; webfetch specs, record URLs.
