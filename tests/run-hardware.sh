#!/bin/sh
# run-hardware.sh — the FDK hardware test kit (the owner runs this on
# a real GPU machine; the output is ONE report file to paste back).
#
# It never needs any token, network access, or push: everything runs
# locally against the checkout it lives in.
#
# What it does, in order:
#   1. builds FDK (plain make; the debug/sanitizer default is fine —
#      the report should be about the DRIVER, not the config),
#   2. prints the GL facts: renderer string, driver, EGL extension
#      list (glxinfo/eglinfo when present, the tests' own verdicts
#      otherwise),
#   3. runs the GPU paint smoke and captures the CPU-vs-GPU
#      interior/edge split statistics,
#   4. builds and runs the GL canvas example,
#   5. runs the dmabuf round-trip when /dev/dri exists — SKIPs loudly
#      otherwise (the report must never fake a PASS),
#   6. writes fdk-hardware-report.txt next to itself and prints it.
#
# Prerequisites: a C toolchain, make, an X display (or Xvfb), and for
# the GL facts optionally mesa-utils (glxinfo) + mesa-utils-extra
# (eglinfo). Run from the repo root:  sh tests/run-hardware.sh

set -u

REPORT="fdk-hardware-report.txt"
: > "$REPORT"
r() { printf '%s\n' "$*" >> "$REPORT"; printf '%s\n' "$*"; }

r "==== FDK hardware report — $(date -u '+%Y-%m-%d %H:%M:%SZ') ===="
r "host: $(uname -srm)"
r ""

r "---- 1. build ----"
WL_CFLAGS=$(pkg-config --cflags wayland-client xkbcommon 2>/dev/null || true)
WL_LIBS=$(pkg-config --libs wayland-client xkbcommon 2>/dev/null || true)
if make build/tests/test_gpu_paint build/tests/test_dmabuf > /dev/null 2>&1; then
    r "make (lib + test_gpu_paint + test_dmabuf): OK"
else
    r "make: FAILED — the report below is not trustworthy; fix the build first"
    r "==== end ===="
    exit 1
fi
r ""

r "---- 2. GL facts ----"
if command -v glxinfo > /dev/null 2>&1; then
    glxinfo -B >> "$REPORT" 2>&1
    grep -E 'OpenGL vendor|OpenGL renderer|OpenGL version' "$REPORT" | sed 's/^/  /'
else
    r "glxinfo: ABSENT (install mesa-utils for the renderer/driver strings)"
fi
if command -v eglinfo > /dev/null 2>&1; then
    eglinfo -B >> "$REPORT" 2>&1 || eglinfo >> "$REPORT" 2>&1
    r "egl extension list: appended above"
else
    r "eglinfo: ABSENT (install mesa-utils-extra for the EGL extension list)"
fi
if [ -d /dev/dri ]; then
    r "DRM nodes: $(ls /dev/dri | tr '\n' ' ')"
else
    r "DRM nodes: NONE (/dev/dri missing — the dmabuf round-trip will SKIP)"
fi
r ""

r "---- 3. GPU paint smoke (the CPU-vs-GPU interior/edge split) ----"
# Failure reporting: the full-scene run's own output pinpoints WHICH
# gate died (per-region interior/edge stats, forensics pixels, the
# GL-up renderer string, the fbconfig sRGB fact). An early death has
# no summary line at all, so on any nonzero rc the log TAIL is
# printed verbatim; rc>=128 means a signal (crash). When the full
# scene fails, GPU_MINIMAL (bg + solid rect + rounded fill only — no
# blits/rotation/texture/atlas) and GPU_EMPTY (untouched surface)
# isolation runs separate core-raster problems from the dynamic
# paths and from present/readback alone.
# ra — report-append with echo: appends a line to the report FILE
# AND prints it. The first hardware paste proved why this matters:
# r() dual-prints but a plain >> append went to the file only, so the
# terminal (and the owner's paste) showed bare rc codes while the
# evidence sat in fdk-hardware-report.txt. Everything appended in
# section 3 must go through ra, never a bare >>.
ra() { while IFS= read -r _line; do printf '%s\n' "$_line" >> "$REPORT"; printf '%s\n' "$_line"; done; }

gpu_run() { # $1 = env prefix, $2 = label
    env $1 ASAN_OPTIONS=detect_leaks=0 build/tests/test_gpu_paint \
        > /tmp/fdk-hw-gpu.log 2>&1
    RC=$?
    r "$2: rc=$RC"
    r "  (log: $(wc -c < /tmp/fdk-hw-gpu.log 2>/dev/null || echo 0) bytes"
    r "   — 0 bytes = the process produced no output at all; check the"
    r "   ASAN/LSAN/FDK env vars on this box)"
    if [ "$RC" -ge 128 ]; then
        r "  (died by signal $((RC - 128)) — a crash, not a verdict)"
    fi
    grep -E 'fdk-gpaint: GL up|fdk-gl: fbconfig' /tmp/fdk-hw-gpu.log \
        | sed 's/^/  /' | ra
    if [ "$RC" -eq 0 ]; then
        sed -n '/CPU-vs-GPU interior\/edge split/,$p' /tmp/fdk-hw-gpu.log \
            | sed 's/^/  /' | ra
    else
        tail -40 /tmp/fdk-hw-gpu.log | sed 's/^/  | /' | ra
        # the crash tail ends at death; also surface the GL identity
        grep -E 'fdk-gpaint: GL up|fdk-gl: fbconfig' /tmp/fdk-hw-gpu.log \
            | sed 's/^/  /' | ra
        grep -E '\[FAIL\]|\[skip\]|GPU PAINT SMOKE' /tmp/fdk-hw-gpu.log \
            | sed 's/^/  /' | ra
    fi
    return $RC
}
if [ -x build/tests/test_gpu_paint ]; then
    gpu_run "" "test_gpu_paint (full scene)"
    FULL=$?
    if [ "$FULL" -ne 0 ] && [ "$FULL" -lt 128 ]; then
        gpu_run "GPU_MINIMAL=1" "test_gpu_paint MINIMAL (core raster only)"
        MIN=$?
        gpu_run "GPU_EMPTY=1" "test_gpu_paint EMPTY (present+readback only)"
        r "isolation: full=$FULL minimal=$MIN empty=$RC (full!=0 + minimal=0"
        r "  => a dynamic path (blit/rotation/texture/atlas); minimal!=0"
        r "  => the core raster (config/rounding/sRGB); empty!=0 => the"
        r "  present/readback seam itself)"
    elif [ "$FULL" -ne 0 ]; then
        r "(crash — no isolation runs; fix the crash first)"
    fi
else
    r "build/tests/test_gpu_paint: MISSING (the build step should have made it)"
fi
r ""

r "---- 4. the GL canvas example ----"
P="$PWD/build/fdk-install"
if make PREFIX="$P" install > /dev/null 2>&1 \
   && cc -Iinclude $WL_CFLAGS -fsanitize=address,undefined \
        examples/gl-canvas.c "$P/lib/libfdk.a" \
        -L"$P/lib" -lX11 -lXext -lXrandr -lXrender -ldl \
        $WL_LIBS -lm -lz -pthread -o build/gl-canvas-example; then
    if [ -n "${DISPLAY:-}" ]; then
        ASAN_OPTIONS=detect_leaks=0 timeout 60 build/gl-canvas-example > /tmp/fdk-hw-canvas.log 2>&1
        RC=$?
        r "gl-canvas: rc=$RC"
        grep -E '\[canvas\]|\[gl\]' /tmp/fdk-hw-canvas.log | head -8 | sed 's/^/  /' >> "$REPORT"
    else
        r "gl-canvas: SKIPPED — no DISPLAY (set one, or run under Xvfb)"
    fi
else
    r "gl-canvas: BUILD FAILED — capture the cc output manually (pkg-config wayland-client xkbcommon)"
fi
r ""

r "---- 5. the dmabuf round-trip (the DRM-box step) ----"
if [ -w /dev/dri/renderD128 ] || [ -r /dev/dri/renderD128 ]; then
    make build/tests/test_dmabuf > /dev/null 2>&1
    ASAN_OPTIONS=detect_leaks=0 build/tests/test_dmabuf > /tmp/fdk-hw-dmabuf.log 2>&1
    RC=$?
    r "test_dmabuf: rc=$RC"
    grep -E 'round-trip|modifier|DMABUF|\[ok\]|\[skip\]|\[FAIL\]' /tmp/fdk-hw-dmabuf.log | sed 's/^/  /' >> "$REPORT"
else
    r "test_dmabuf: SKIP — no readable /dev/dri/renderD128 (record the SKIP in the release notes; never fake a PASS)"
fi
r ""

r "==== end — paste everything above back to the session ===="
exit 0
