#!/usr/bin/env bash
# Restore the user-space Wayland toolchain after an environment reset.
# The /home/z/apt prefix is NOT part of the mounted project dir and gets
# wiped whenever the sandbox resets; this script rebuilds it from
# Debian trixie packages (download + dpkg -x, no root needed).
set -u
DEBS=/home/z/apt/debs
PREFIX=/home/z/apt/prefix
mkdir -p "$DEBS" "$PREFIX" /home/z/apt/xdg-run
chmod 700 /home/z/apt/xdg-run 2>/dev/null || true

cd /tmp
# Build deps + sway (with its full runtime closure) + weston.
apt-get download \
    libwayland-dev libwayland-bin libwayland-client0 libwayland-cursor0 libwayland-server0 \
    libxkbcommon-dev libxkbcommon0 libxkbregistry0 \
    sway weston grim labwc wtype xwayland libxcvt0 libsfdo0 \
    libevdev2 libinput10 libudev1 libseat1 libjson-c5 libpcre2-8-0 \
    libpng16-16t64 libfreetype6 libfontconfig1 libcairo2 libpixman-1-0 \
    libgbm1 libdrm2 libwlroots-0.18 libgdk-pixbuf-2.0-0 libglib2.0-0t64 \
    libpango-1.0-0 libpangocairo-1.0-0 libsystemd0 \
    libegl1 libgles2 libdisplay-info2 libliftoff0 libwacom9 \
    libwayland-egl1 libegl-mesa0 libglvnd0 libgl1-mesa-dri libllvm19 \
    libxcb-composite0 libxcb-errors0 libxcb-ewmh2 libxcb-icccm4 \
    libxcb-render-util0 libxcb-res0 libxcb-xinput0 \
    libgudev-1.0-0 libmtdev1t64 2>&1 | tail -1
mv /tmp/*.deb "$DEBS/" 2>/dev/null || true
for d in "$DEBS"/*.deb; do dpkg -x "$d" "$PREFIX/"; done

# Debian's .pc files hardcode prefix=/usr; rewrite to the real prefix so
# pkg-config --cflags/--libs point at the extracted files.
for f in "$PREFIX"/usr/lib/x86_64-linux-gnu/pkgconfig/*.pc \
         "$PREFIX"/usr/share/pkgconfig/*.pc; do
    [ -f "$f" ] && sed -i 's|^prefix=/usr$|prefix=/home/z/apt/prefix/usr|' "$f"
done
echo "toolchain restored: $(ls "$DEBS" | wc -l) debs extracted to $PREFIX"
