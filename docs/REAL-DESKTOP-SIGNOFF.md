# REAL-DESKTOP-SIGNOFF.md — M-9: the owner's real-desktop sign-off

**sign-off — FDK 1.0.0, the first release.** Every claim below is
the owner's own run on their hardware, quoted or paraphrased from
their session logs, gathered across the verification sessions of
2026-11-09 (rounds 53-56). This document is the G4 artifact: the
release gate checks for its existence and this signed state.

## The machine

Owner: **FemBoyGamerTechGuy**. X11: Openbox + tint2, screen
1920x1080 `HDMI-0`, NVIDIA GeForce GTX 1050 Ti (proprietary driver).
Wayland sessions: **labwc**. A single output — this fact becomes the
one hardware-blocked item below. Session locale: Romanian (`ro`),
which is what surfaced the missing-catalog boot noise (fixed in
round 53).

## The walked evidence

| # | item | verdict | the owner's evidence |
|---|------|---------|----------------------|
| 1 | the UI font size chain (TASK C, §8) | PASS | the default runs; the `FDK_UI_FONT_SIZE` 12→16→24→32 sweep ("the text just got bigger and bigger"); the desktop statement read at boot |
| 2 | the tray against the real bar | PASS | fdk-files docked in tint2, popup menus at the click point, the live-model row |
| 3 | notifications against the real daemon | PASS | delivered via dunst; the daemon-less sessions degrade correctly (round 51's About dialog needs no daemon) |
| 4 | multi-monitor hotplug (§12) | NOT TESTABLE | one HDMI-0 output; "i cant test it to expensive for another monitor" — recorded in the release notes as untested |
| 5 | the Wayland compositor walk | PASS | the owner's Wayland sessions are labwc (not sway — the docs were re-scoped); the round-56-era logs show the Wayland backend, explicit-sync advertisement, the GPU lane, text-input-v3, clean exits |
| 6 | drag icons (§14) | PASS | "i can drag icons on fdk-files and it follows the cursor" |
| 7 | portals (§15/§8) | PASS, with a recorded limitation | the Settings portal read confirmed live: `gsettings` Sans 28 → "the portal's font statement is 28 pt (38 px)"; the no-portal private-bus run fell back to the 14 px compiled default with no hang. The owner's locked spec (rounds 55-56): FDK does not listen to GTK's font configuration and gains no GTK dependency — the channel is the desktop statement, portal optional. In 1.0.0 the statement is recorded for fresh instances; live resize of running UI is the first post-1.0.0 follow-up (the log line says so honestly) |
| 8 | Orca / AT-SPI (§16) | PASS | "Tested orca with fdk and it works and its loud my ears"; a refused a11y bus (startup race) degrades gracefully and retries |
| 9 | touch (§26) | N/A by hardware | no touch device; the probe behaves correctly ("touch events available", "no stylus evidence among 5 pointer device(s)") |
| 10 | the GL/GPU lane (§27) | PASS | "GPU present lane active (NVIDIA GeForce GTX 1050 Ti)" on X11 and Wayland; the small-surface CPU path observed; the owner confirmed enable/disable |

The remaining chain checks, all owner-run (2026-11-09):

- **No-GTK linkage**: `ldd ./build/examples/text_demo | grep -iE
  'gtk|gdk|gobject|pango'` — empty. The coupling to the desktop is
  D-Bus only.
- **Compiled default**: the private-bus run rendered 14 px with no
  ui-metrics line — the portal ReadOne failed and FDK silently kept
  its default. No hang, no crash, clean exit.
- **Startup is non-blocking**: fonts initialize before the portal
  reply lands; the statement applies to the metrics when it arrives.

## The owner's dispositions (the release calls)

1. **Multi-monitor** — not testable on this hardware; shipped as a
   documented unknown, not a claim.
2. **Portal live-apply** — the owner's decision: do not touch the
   font-size implementation in 1.0.0. The log line states the
   recorded-not-applied behavior; the release notes carry it; the
   live-resize follow-up (reload + re-shape through the same entry
   point `FDK_UI_FONT_SIZE` uses at init) is filed for post-1.0.0.
3. **The ≤1 KB libdbus teardown residual** — accepted: release
   builds do not ship the sanitizer; the detector stays alive in
   dev builds; the `shutdown with N D-Bus connection(s) still live`
   line is the honest marker.

## The final call

The owner, 2026-11-09, after the last disposition round: everything
not walked on this hardware **"should work perfectly fine"** — the
release proceeds on this evidence.

## sign-off

Signed: **FemBoyGamerTechGuy** (owner), 2026-11-09 — v1.0.0 is
authorized to tag.

Register note: M-9 stays IN PROGRESS in the ROADMAP register by
design — the real-desktop walkthrough is the standing verification
surface for every future release, not a one-time hurdle.
