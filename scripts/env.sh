# FDK build environment (source it).
# The X11 dev debs (xrandr/xtst/xi/x11 + deps) are extracted to
# /tmp/x11dev by scripts/setup-sandbox.sh; the Wayland toolchain
# lives under /home/z/apt/prefix (scripts/restore_wayland_toolchain.sh).
export X11_PREFIX="${X11_PREFIX:-/tmp/x11dev/usr}"
export WL_PREFIX="${WL_PREFIX:-/home/z/apt/prefix/usr}"
export PKG_CONFIG_PATH="$X11_PREFIX/lib/x86_64-linux-gnu/pkgconfig:$WL_PREFIX/lib/x86_64-linux-gnu/pkgconfig"
export LD_LIBRARY_PATH="$X11_PREFIX/lib/x86_64-linux-gnu:$WL_PREFIX/lib/x86_64-linux-gnu"
