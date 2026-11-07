# test-apps — FDK gaps found while building the real applications

The contract (the owner's test-apps brief): anything an app needs
that FDK lacks is a BUG IN FDK, not a limitation to work around in
the app. Each gap found is logged here, then fixed in FDK proper
(`src/`, `include/`) as a general-purpose API, with an example under
`examples/` and a test (headless or X11 suite, whichever fits),
before the app uses it.

| # | App that hit it | What was needed | Why | Status |
|---|-----------------|-----------------|-----|--------|
| 1 | fdk-term | Whole-window **opacity** for the terminal's background-opacity profile. | Every terminal since the compositing era offers it; EWMH `_NET_WM_WINDOW_OPACITY` is the X11 convention. | **FIXED in FDK** — `fdk_window_set_opacity`/`fdk_window_get_opacity` (X11 CARDINAL 32; Wayland reports `FDK_ERR_UNSUPPORTED` honestly — xdg-shell has no per-toplevel opacity protocol). Test: X11 suite, window-identity leg (property round-trip + clamps). |
| 2 | fdk-term | **Audible bell** for BEL (0x07). | The terminal protocol's bell; X11 has XBell natively. | **FIXED in FDK** — `fdk_window_bell(window, percent)` (X11 XBell, percent clamped [-100,100]; Wayland `FDK_ERR_UNSUPPORTED` — no bell protocol; visual bell/notify is the caller's answer, documented). Test: X11 suite, window-identity leg. |
| 3 | fdk-term | **fd readability in the event loop** (PTY output must wake the pump). | A terminal is an fd-driven app; without loop integration the UI stalls until the next X event. | **Already existed** — `fdk_ext_loop_add_fd` / `fdk_ext_loop_update_fd` / `fdk_ext_loop_remove_fd` (`fdk_extend.h`, the extension loop API; the pump's poll set grows with watched fds). Not a gap after all; recorded here because the search was real. |
| 4 | fdk-term (also fdk-chat, fdk-panel) | **Open a URI with the user's configured handler** (Ctrl+click a URL in the terminal). | Every real desktop app opens links; the toolkit should own the xdg-open dance (PATH probe, detach, validation) so apps never shell out. | **FIXED in FDK** — `fdk_desktop_open_uri` / `fdk_desktop_open_path` (`fdk_desktop.h`): validated URIs, xdg-open probed first (`FDK_ERR_UNSUPPORTED` without it), detached setsid launch, no blocking. Test: test_desktop_open.c (validation ladder + honest split). Example: 20_desktop_open.c. |
