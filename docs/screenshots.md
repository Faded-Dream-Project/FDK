# FDK — Captured Proof Gallery

Every image here is a real captured frame from the test rigs — not a
mockup. Captures come from Xvfb + `x11grab` on the X11 side, and from
compositor screenshot paths (weston/sway headless, wlr-screencopy) on
the Wayland side. Since the Wayland deferred-first-frame fix
(milestone 1.1.1) the whole example suite runs on both backends under
the test rigs: every example is launched in the same
show -> paint -> pump order a real application uses,
screenshot-verified, then closed through the real close path. Some
early proofs show example names from before the suite was
consolidated; the feature proofs are unchanged.

## The consolidated example suite on Wayland

Real compositor output from sway (headless, wlr-screencopy):

![02_rendering on Wayland](screenshots/suite_wayland_02_rendering.png)

![03_text on Wayland](screenshots/suite_wayland_03_text.png)

![04_widgets on Wayland](screenshots/suite_wayland_04_widgets.png)

![07_advanced on Wayland](screenshots/suite_wayland_07_advanced.png)

## Rendering proofs (X11)

The demo window at 640x480, and again after a live resize to 800x600
— the framebuffer follows the resize and the content repositions into
the new bounds. The dark area around each window is the bare Xvfb
root window, kept in frame on purpose: it shows the window boundary
is real.

![X11 demo window at 640x480](screenshots/x11_frame_640x480.png)

![X11 demo window after a live resize to 800x600](screenshots/x11_frame_800x600.png)

## Rendering proofs (Wayland)

The same example under Wayland (weston 14, headless backend,
compositor screenshot), captured 3.5 seconds apart. The gradient
occupies the whole frame because the kiosk shell configures the
surface fullscreen; the shifted colors between the two frames show
the animation genuinely advancing frame by frame on the Wayland
present path.

![Wayland demo, first capture](screenshots/wayland_frame_1.png)

![Wayland demo, 3.5 s later — gradient advanced](screenshots/wayland_frame_2.png)

A 10-second screencast of the animated X11 session — including the
live resize and a clean window close mid-render — is committed
alongside the stills:
[`screenshots/fdk_render_x11.mp4`](screenshots/fdk_render_x11.mp4).

## Widget foundation

Two frames of the Phase 4 foundation demo (since folded into the
widget catalog, `examples/04_widgets.c`) under Xvfb, driven by real
injected input (XSendEvent through the X server): the initial state,
and after two button clicks plus a Tab. Between the frames the meter
recolored AND grew (a `set_bounds` layout change repainting only the
affected region), and the "hue" button's color shifted because
keyboard focus moved to it. The quit button ends the demo by
destroying the window from inside its own pointer-release handler —
and the process exits cleanly (exit 0, ~390 frames).

![Widget foundation, initial state](screenshots/widgets_frame_initial.png)

![Widget foundation after clicks + Tab](screenshots/widgets_frame_interacted.png)

## Layout engine

The layout demo (its grid now lives in the "Layout — grid" frame of
`examples/04_widgets.c`) held at two different window sizes during
its steady phases: the header dots stay anchored top-left, the
expanding panel owns whatever space the window gives it, the footer
stays pinned to the bottom. Nothing in the example computes geometry
by hand.

![Layout at 660x480](screenshots/layout_hold_660x480.png)

![Layout at 500x380 — same code, reflowed](screenshots/layout_hold_500x380.png)

## Text stack

The raw-rendering half of `03_text` in its deterministic hold frame
(real shaped glyphs, PIL-verified down to the colored runs and the
stats footer):

![Text hold frame: wordmark, size ladder, colored runs, wave, stats](screenshots/text_frame_640x480.png)

Text layout's proof — the label-mode half of `03_text` at full width
and after the rig narrows the window: the paragraph re-wraps from
five lines to seven and the truncated line's ellipsis moves left with
the edge.

![Text layout hold frame](screenshots/text_layout_frame_480x620.png)

![Text layout after the resize](screenshots/text_layout_reflow_340x620.png)

## Widget catalog

The widget catalog in its deterministic hold frame: real title bands,
a toggle, checkboxes, a checked radio (the accent dot), buttons, a
full progress bar, and the status label — every element PIL-verified
by the test rig:

![Widget catalog hold frame](screenshots/widgets_catalog_frame_520x430.png)

## Theme engine

`05_theme` cycling its faces (the built-in Faded Dream, Daylight from
a complete `.fdk` file, Matrix from a deliberately partial one, and
the source tree's `.FDKThemes/` folder): same widgets, same layout,
same code; only the default theme differs between captures. The rig
also verifies the round trip back to the default face is pixel-exact.

![Three themes, same code](screenshots/theme_three_themes_1380x330.png)

## Decorations and window management

Decorated (note the FDK band: title, the three management buttons,
themed rule — at the post-drag position) vs. toggled back to WM
decorations (band gone, content reflows to the full window):

![Decorations on/off](screenshots/decorations_on_off_1050x360.png)

The Phase 8 completion, driven by real input in the test rig:
decorated and dragged; then MAXIMIZED via the band's own maximize
button (fills the screen); then restored by double-clicking the band
and GROWN 460x300 -> 500x330 by dragging the bottom-right resize
corner FDK draws:

![Window management through the decoration band](screenshots/decorations_management_1636x340.png)

## Advanced widgets

The Phase 9 completion, same rig, same discipline — `07_advanced`
driven by real clicks through the X server: (1) the File menu open
below its bar title — a toolkit-owned popup the demo never paints
itself; (2) a ComboBox dropdown; (3) the modal message dialog (its
own toplevel, grabbing input server-side); (4) a nested submenu
chain, each level its own popup. Escape peels the chain one level per
press; the rig's XQueryTree assertion proves every popup window left
the X server when the chain closed:

![Advanced widgets: popups through real input](screenshots/advanced_widgets_2600x578.png)

## The narrator

The embedded narrator demo (`08_narrator`): a scripted tour walks
focus through a form while the narrator speaks each move through a
subtitle bar (also referenced from `docs/roadmap.md`):

![Narrator demo](screenshots/narrator_demo_460x400.png)

## Applications

The capabilities example mid-typing on X11 (1024x768):

![Capabilities example, typing](screenshots/capabilities_typing_x11_1024x768.png)

The file dialog's new-folder prompt on X11:

![File dialog new-folder prompt](screenshots/prompt_dialog_newfolder_x11_1024x768.png)

The file picker wearing the DESKTOP's own icons — every file type
distinct: folders, archives, executables, images, text, audio. Both
views were captured with NO theme override at all: the engine found
the system default channel on its own (/usr/share/icons/default ->
Adwaita here). The list view resolves the theme's 16-px PNGs; the
grid's 48-px cells are RASTERIZED FROM SVG (Adwaita ships no 48-px
PNGs — the vendored nanosvg renders the scalable assets at the
requested size, verified pixel-level against an independent cairo
reference). Since 1.5.3 the icons composite with true per-pixel
alpha — the transparent surround around every artwork shows the row
or cell background (the earlier builds painted an opaque black box
there), and the detection chain reads the LIVE desktop through
XSETTINGS on X11 in addition to the file channels. Captured on X11:

![File picker, themed icons, list view](screenshots/file_picker_themed_list_x11_1024x768.png)

![File picker, themed icons, grid view](screenshots/file_picker_themed_grid_x11_1024x768.png)

FDK Files (`examples/10_file_manager.c`) — column view on X11, and
the home folder on Wayland:

![FDK Files column view on X11](screenshots/file_manager_columns_x11_964x600.png)

![FDK Files home folder on Wayland](screenshots/file_manager_home_wayland_880x540.png)
