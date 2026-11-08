# FDK build environment (source it).
# The X11 dev debs (xrandr/xtst/xi/x11 + deps) are extracted to
# /tmp/x11dev by scripts/setup-sandbox.sh; the Wayland toolchain
# lives under /home/z/apt/prefix (scripts/restore_wayland_toolchain.sh).
#
# Resilience rule (the 2026-11-08 gate lesson): a source'd env file
# must NEVER clobber an environment the caller already prepared, and
# a missing prefix must be skipped, not exported as a dead search
# path. On a recycled sandbox both canonical prefixes can be gone
# while the caller's CFLAGS/PKG_CONFIG_PATH point at a fresh
# extraction — the release gate's clean build died exactly there
# (pkg-config answered empty, -lXrandr went missing, make failed
# with its output discarded). A dev machine with system -dev
# packages needs neither prefix at all.
export X11_PREFIX="${X11_PREFIX:-/tmp/x11dev/usr}"
export WL_PREFIX="${WL_PREFIX:-/home/z/apt/prefix/usr}"
for fdk_env_prefix in "$X11_PREFIX" "$WL_PREFIX"; do
    if [ -d "$fdk_env_prefix/lib/x86_64-linux-gnu/pkgconfig" ]; then
        export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$fdk_env_prefix/lib/x86_64-linux-gnu/pkgconfig"
        export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$fdk_env_prefix/lib/x86_64-linux-gnu"
    fi
done
unset fdk_env_prefix
