#!/usr/bin/env bash
# setup-sandbox.sh — ONE COMMAND to rebuild the whole test environment
# after a sandbox recycle (which wipes /tmp and /home/z/apt, and can
# reset the clone to a stale snapshot). Idempotent: every step checks
# whether its outputs already exist and skips.
#
# What it builds (paths the suites and rigs expect):
#   /tmp/x11dev                 — the X11 dev debs (headers + .so),
#                                 pc prefixes rewritten, .a archives
#                                 removed (they break the .so link)
#   /home/z/apt/prefix          — the user-space Wayland toolchain
#                                 (scripts/restore_wayland_toolchain.sh)
#   /tmp/xtest_driver           — the XTEST input rig (RPATH'd)
#   /home/z/my-project/scripts/xdnd_sink — the XDND sink rig (RPATH'd;
#                                 the x11 integration suite spawns it
#                                 at this absolute path)
#   git identity hooks          — pre-commit (identity check) and
#                                 commit-msg (trailer stripper)
#
# Usage:  bash scripts/setup-sandbox.sh
set -u
# Pin the repo root ABSOLUTELY at script start, before ANY cd below:
# step 1's else-branch cd's into /tmp/x11dev-debs, and a later
# dirname-based re-pin resolves RELATIVE to that stranded cwd (it
# failed exactly that way after the fourth recycle: steps 2-4 ran
# from /tmp/x11dev-debs, the Wayland toolchain never installed, and
# the link failed on the wayland_egl symbols).
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

X11DEV=/tmp/x11dev
X11LIB=$X11DEV/usr/lib/x86_64-linux-gnu
OUTSIDE=/home/z/my-project/scripts

echo "== 1/4: the X11 dev debs =="
if [ -f "$X11LIB/pkgconfig/xrandr.pc" ] && \
   grep -q "prefix=$X11DEV/usr" "$X11LIB/pkgconfig/xrandr.pc" 2>/dev/null; then
    echo "   x11dev already prepared"
else
    mkdir -p "$X11DEV" /tmp/x11dev-debs
    cd /tmp/x11dev-debs
    # x11/xext/xrender/xfixes/xrandr/xtst/xi/xcb/xkbcommon dev+runtime
    # debs: headers for the build, .so for the link, .so.N for runtime.
    apt-get download \
        libx11-dev libx11-6 libxext-dev libxext6 libxrender-dev \
        libxrender1 libxfixes-dev libxfixes3 libxrandr-dev libxrandr2 \
        libxtst-dev libxtst6 libxi-dev libxi6 libxcb1-dev libxcb1 \
        libxkbcommon-dev libxkbcommon0 >/dev/null 2>&1 || true
    for f in *.deb; do
        [ -f "$f" ] && dpkg -x "$f" "$X11DEV"
    done
    # The deb .pc files hardcode prefix=/usr; rewrite them to the
    # extraction prefix so -I/-L point at our copies.
    for pc in "$X11LIB"/pkgconfig/*.pc; do
        sed -i "s|^prefix=/usr\$|prefix=$X11DEV/usr|" "$pc"
    done
    # x11.pc's libdir stays prefix-relative (all fine once prefix is
    # rewritten). The STATIC archives go: without them absent the
    # linker takes the shared objects, which we need for -fPIC links
    # (the .a files are non-PIC and break libfdk.so with
    # "relocation R_X86_64_PC32 ... recompile with -fPIC").
    rm -f "$X11LIB"/*.a
    cd "$(dirname "$0")" && cd ..
    echo "   x11dev prepared"
fi
# The x11dev branch may have been skipped (already prepared) — the
# earlier cd into /tmp/x11dev-debs is NOT undone in that path. Re-pin
# the working directory to the repo root by absolute path; every later
# step (restore_wayland_toolchain.sh, rig compiles) runs from here.
cd "$ROOT_DIR" # re-pin (the x11dev branch may have cd'd to /tmp)

echo "== 2/4: the Wayland toolchain =="
if [ -f /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/pkgconfig/wayland-client.pc ]; then
    echo "   wayland prefix already present"
else
    bash scripts/restore_wayland_toolchain.sh
fi
# The restore can re-extract X11 .a archives into the apt prefix
# (its deb list has some overlap); purge those too.
rm -f /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/libX11.a \
      /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/libXext.a \
      /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/libXrender.a \
      /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/libXrandr.a \
      /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/libXi.a \
      /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/libXtst.a \
      /home/z/apt/prefix/usr/lib/x86_64-linux-gnu/libXfixes.a 2>/dev/null

echo "== 3/4: the rig binaries =="
cc -O1 -I"$X11DEV/usr/include" scripts/xtest_driver.c \
   -o /tmp/xtest_driver \
   -L"$X11LIB" -lXtst -lX11 \
   -Wl,-rpath,"$X11LIB" && echo "   xtest_driver built" \
   || echo "   xtest_driver BUILD FAILED"
mkdir -p "$OUTSIDE"
cc -O1 -I"$X11DEV/usr/include" scripts/xdnd_sink.c \
   -o "$OUTSIDE/xdnd_sink" \
   -L"$X11LIB" -lX11 \
   -Wl,-rpath,"$X11LIB" && echo "   xdnd_sink built" \
   || echo "   xdnd_sink BUILD FAILED"

echo "== 4/4: the git identity hooks =="
git config user.name "Faded Dream Project"
git config user.email "242450780+FemBoyGamerTechGuy@users.noreply.github.com"
git config --global user.name "Faded Dream Project" 2>/dev/null || true
git config --global user.email \
    "242450780+FemBoyGamerTechGuy@users.noreply.github.com" 2>/dev/null || true
mkdir -p .git/hooks
cat > .git/hooks/pre-commit <<'HOOK'
#!/bin/sh
name=$(git config user.name)
email=$(git config user.email)
if [ "$name" != "Faded Dream Project" ] || \
   [ "$email" != "242450780+FemBoyGamerTechGuy@users.noreply.github.com" ]; then
    echo "PRE-COMMIT ABORT: identity must be exactly"
    echo "  Faded Dream Project <242450780+FemBoyGamerTechGuy@users.noreply.github.com>"
    echo "  (got: $name <$email>)"
    exit 1
fi
exit 0
HOOK
cat > .git/hooks/commit-msg <<'HOOK'
#!/bin/sh
python3 - "$1" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split('\n')
out = []
for ln in lines:
    low = ln.strip().lower()
    if (low.startswith('co-authored-by:') or
            low.startswith('signed-off-by:') or
            low.startswith('z.t.x:') or low == 'z.t.x'):
        continue
    out.append(ln)
open(p, 'w').write('\n'.join(out))
PY
exit 0
HOOK
chmod +x .git/hooks/pre-commit .git/hooks/commit-msg
echo "   hooks installed"

echo
echo "setup complete. Now: source scripts/env.sh && make -j4 && make test"
