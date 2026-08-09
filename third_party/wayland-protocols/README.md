# Wayland protocol XML

`xdg-shell.xml` is copied verbatim from `wayland-protocols` (the
upstream freedesktop.org project), MIT-licensed — see the
`<copyright>` block inside the file itself for the full notice and
list of copyright holders.

It is not FDK's own work; it's the standard protocol description used
to generate `src/platform/wayland/generated/xdg-shell-client-protocol.h`
and `xdg-shell-protocol.c` via `wayland-scanner` (part of the
`libwayland-dev` package, itself MIT-licensed) as part of the FDK
build. See `docs/dependencies.md` for the full justification and
`docs/licensing-policy.md` for why this is compatible with FDK's
proprietary license despite being third-party code physically present
in the repository.

Regenerate the bindings after updating this file with:

    wayland-scanner client-header third_party/wayland-protocols/xdg-shell.xml \
        src/platform/wayland/generated/xdg-shell-client-protocol.h
    wayland-scanner private-code third_party/wayland-protocols/xdg-shell.xml \
        src/platform/wayland/generated/xdg-shell-protocol.c
