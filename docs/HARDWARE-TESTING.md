# Hardware testing — the kit for a real GPU machine

FDK's CI proves the software story on stock runners (headless sway,
Xvfb, software GL); what CI can NEVER prove is the driver story: a
real render node, a real GL driver, the dmabuf round-trip against a
real allocator. This kit collects exactly that, in one paste-back
report.

## What the kit is

- `tests/run-hardware.sh` — one POSIX-sh script (`--asan` opts into sanitizer builds — they are off by default), no
  network, no tokens. Run it from the repo root on the GPU machine:

      sh tests/run-hardware.sh

  It writes `fdk-hardware-report.txt` next to itself and prints the
  same content. Paste the whole file back into the session.

## What it runs

1. **The build** — plain `make`. The default config is fine (add `--asan` for a sanitizer build — sanitizers are off by default); the report is about the driver, not the build config.
2. **The GL facts** — `glxinfo -B` (renderer, vendor, version) and
   `eglinfo` (the extension list) when present. Without them the
   report says so and the tests' own verdicts carry the section —
   install `mesa-utils` / `mesa-utils-extra` for the full strings.
3. **The GPU paint smoke** (`test_gpu_paint`) — the CPU-vs-GPU
   interior/edge split: interior pixels must be near-exact, edges
   only need AA-consistency. The interior/edge statistics table in
   the report is the number to eyeball: interior mean should be ~0 on
   any healthy driver; edge mean is the driver's AA quality signal.
4. **The GL canvas example** (`examples/gl-canvas.c`) — the real GL
   lane painting through the compositor.
5. **The dmabuf round-trip** (`test_dmabuf`) — ONLY when
   `/dev/dri/renderD128` exists and is readable; otherwise the report
   records a loud SKIP. A SKIP is an honest answer, never a fake
   PASS; it is exactly what checklist step 4 of
   `docs/PRE-1.0-AUDIT.md` needs (the round-trip verdict from a real
   allocator).

## Prerequisites

- a C toolchain, make, pkg-config with the usual dev packages
  (the same ones CI installs),
- an X display for the GL steps (a desktop session, or
  `xvfb-run -a sh tests/run-hardware.sh` — but note: on Xvfb the GL
  driver is software, so the report only counts on REAL hardware),
- optional: `mesa-utils` (glxinfo), `mesa-utils-extra` (eglinfo),
- for step 5: a readable `/dev/dri/renderD128`
  (`sudo usermod -aG render <user>`, then re-login).

## What to do with the report

Paste it back. The interesting lines: the `OpenGL renderer` string
(the driver identity), the interior/edge split table (the GPU paint
quality), `test_dmabuf: rc=0` with the round-trip `[ok]` lines (the
zero-copy verdict), and the EGL extension list (what the driver can
actually promise).
