# FDK test-apps — the validation suite

Four real desktop applications written ONLY against FDK's public
API (`include/fdk/`). Their purpose is to find what FDK is missing
or gets wrong **before any 1.0.0** — every rough edge an application
developer would hit, hit here first, on purpose.

They are NOT toy demos. Every app has: icon toolbars and menus (no
"Back" *word* — a left-arrow icon with a tooltip and a shortcut),
a menu bar, a status bar, context menus, keyboard shortcuts plus a
shortcut list, drag-and-drop where it makes sense, undo/redo,
preferences persisted through `fdk_prefs`, theme-aware colors that
follow a live light/dark switch, i18n-ready strings (English +
Romanian catalogs), HiDPI/scale-correct rendering, accessibility
names/roles on every interactive widget, tray/notification
integration where sensible, and a clean LSan shutdown.

| App | What it is | The FDK surface it pushes |
| --- | --- | --- |
| `fdk-files/` | a real file manager | model/view + sort/filter proxies, virtualized lists at 50k rows, clipboard uri-lists, DnD both sides, MIME detection, thumbnails, inotify, trash spec, undo |
| `fdk-edit/` | a code/text editor | the textview/entry stack at 50 MB, syntax highlighting over range attributes, complex scripts + IME, find/replace, session restore, print |
| `fdk-monitor/` | a system monitor | /proc + /sys parsing, canvas graphs with animation, gestures (pinch/pan), tray pixmaps, notifications with actions, 250 ms timers |
| `fdk-notes/` | a rich notes app | rich text spans, model/view trees, PDF export via the print API, reminders + tray badge, a11y narration, focus timer animation |

## Build and run

```sh
make -C .. static        # the in-tree libfdk.a (ASan debug build)
make -C .                # builds all four into test-apps/build/
./build/fdk-files        # run one (needs an X display; Wayland too)

make test-apps-smoke     # every app under a throwaway Xvfb:
                         # opens, paints 40 frames, exits cleanly
                         # under the repo's LSan suppressions
```

The apps link the in-tree static library with the same
ASan+UBSan flags the FDK test suite uses, so any memory error in
FDK that an app can reach, the smoke run reaches too.

`FDK_TESTAPP_FRAMES=N` exits any app after N painted frames (the
smoke target sets 40). `FDK_TESTAPP_SCREENSHOT=path.png` asks the
app to save a screenshot at frame ~30 through FDK's surface PNG
encoder (see `screenshots/`).

## How results are reported

- **`FINDINGS.md`** — the running ledger: every FDK bug or gap an
  app found, its root cause, the fix commit, and the regression
  test that pins it. Protocol: a failing test lands in FDK's own
  suite FIRST, then the fix, then the app continues. Nothing is
  worked around silently inside an app.
- **Each app's `README.md`** — the feature checklist with an honest
  done / partial / not-done verdict per line, the FDK APIs it
  exercises, performance numbers (measured, with hardware noted),
  what is verified in this sandbox vs. marked "should work" (needs
  a real desktop session), and the spec sources it implements.
- **`PRE-1.0-CHECKLIST.md`** — after all four apps: every FDK API
  area with a verdict, the gaps that still block a trustworthy
  1.0.0, and a ranked list of what to do next.

## Sandbox honesty

These apps were developed and verified under **bare Xvfb (no window
manager, no D-Bus session, no desktop daemons)**. That means:
window-manager interactions, real tray hosts, real notification
daemons, IME, and Wayland compositors are exercised by code path
but not by live observation — the READMEs say "should work" for
those, never "it works". Everything else (rendering, widgets, model
view, clipboard, DnD within one app, file operations, /proc
parsing, print-to-PDF) is verified here for real.
