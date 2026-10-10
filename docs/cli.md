# The FDK Command-Line Tools

FDK's settings face for shells and scripts: `fdk-theme`,
`fdk-prefs` (1.4.13), and `fdk-set` (1.4.14). Every desktop toolkit
ends up needing this — GTK has `gsettings`, Qt has its
platform-theme configuration — for one reason: **most applications
never ship a settings UI**, and a desktop where changing the theme
requires each application to have built a theme picker is a desktop
that cannot change its theme. FDK's answer is three small tools over
one global settings file plus the per-application stores the prefs
system already owns.

All three are ordinary applications on the public API — built by
`make` (or `make tools`), installed by `make install` into
`$(PREFIX)/bin`, and doubles as the reference consumer of the
prefs and theme-discovery surfaces.

## The global settings store

The toolkit keeps ONE preference of its own: the theme name. It
lives in the reserved application id `fdk` of the prefs system
(`fdk_prefs.h`), resolved exactly like any application's store:

```
$FDK_PREFS_FILE              (explicit absolute override)
  else $XDG_CONFIG_HOME/fdk.prefs     (default ~/.config/)
  else $HOME/.config/fdk.prefs
```

The file is the documented, human-editable prefs format
(`docs/fdk-prefs-format.md`) — you can watch it change under the
tools, and edit it by hand if you want:

```
# FDK preferences v1 — fdk
[theme]
name = matrix
```

Key consumers can rely on today: `theme.name`. Applications are
expected to keep their OWN settings under their own app id (that is
the 1.3.7 prefs contract); the `fdk` store is the toolkit's, and
`fdk-prefs -a fdk` is how you touch anything in it deliberately.

### How applications pick it up

The theme module applies the setting **once per process, lazily, at
the first theme resolution** (usually the first paint):

```
$FDK_THEME                   (per-process override, like GTK_THEME)
  else theme.name in the application's own store (1.4.14)
  else theme.name in the fdk store
  else the built-in "Faded Dream"
```

An application that calls `fdk_theme_set_default()` before its
first paint **owns the process's theme** — no settings source ever
overrides an explicit choice. That is the same contract GTK gives:
`GTK_THEME` beats the setting, an explicit gtk-theme-name set in
code beats everything. The precedence rules live in
`include/fdk/fdk_theme.h`; the engine is `src/theme/settings.c`.

Failure is soft at every step, on purpose (the prefs resilience
rule): a missing or corrupt settings file means "no preference",
and a name that does not resolve on the search path logs one
warning and stays on the current theme. **A themed launch must
never be a failed launch.**

### Live re-theming (1.4.14)

Running applications **follow the settings files live**. While a
display context exists, FDK watches the settings files' directories
(inotify) and re-resolves whenever one changes: `fdk-theme set` and
`fdk-set theme set` re-theme every running FDK application the
moment they land — including applications driven by plain
`fdk_run()` (the pump repaints their damaged windows itself).
Applications that installed their own theme opted out and are never
overridden; a settings directory that did not exist at application
start cannot be watched (the setting applies at the next launch
instead — the honest limitation).

The developer's side of the contract is
`fdk_theme_set_allowed_themes()`: an application whose brand is one
theme (or a curated set) clamps every settings source above to the
list — see `fdk_theme.h` for the exact semantics.

### The per-application store (1.4.14)

The second and third resolution legs live in the application's OWN
prefs store — the `<app_id>.prefs` file under the same XDG
resolution, where `app_id` is the identity passed to `fdk_init()`.
That is the file `fdk-set theme set NAME --app APP` writes, and the
reason one application can keep its own theme while the desktop's
default changes. Note that under `$FDK_PREFS_FILE` (the explicit
single-file override) every store resolves to that one file, so the
per-app leg is a no-op there — the XDG layout is where per-app
separation exists.

## Theme discovery — where themes live

`fdk-theme` never takes a file path; it takes a NAME, resolved
against the search path (XDG basedir order):

```
$FDK_THEME_DIR                        (one explicit directory)
  $XDG_DATA_HOME/fdk/themes           (default ~/.local/share/fdk/themes)
  $XDG_DATA_DIRS/.../fdk/themes       (default /usr/local/share:/usr/share)
```

`make install` puts FDK's shipped themes into
`$(PREFIX)/share/fdk/.FDKThemes` (1.4.16: the custom folder — the
source tree's `.FDKThemes/`, five faces: `faded-dream` the packaged
copy of the default, `daylight`, `matrix`, `mono-chromatic`,
`pink-rave`) — the default `XDG_DATA_DIRS` already covers
`/usr/local/share` and `/usr/share`, so an installed FDK finds them
with zero configuration.

The full search path, first match wins:

```
$FDK_THEME_DIR                     one explicit directory (absolute)
$HOME/.FDKThemes                   the per-user custom folder (1.4.16)
$XDG_DATA_HOME/fdk/.FDKThemes      (default ~/.local/share/fdk/.FDKThemes)
$XDG_DATA_HOME/fdk/themes          the 1.4.13 location, still scanned
each $XDG_DATA_DIRS entry's fdk/.FDKThemes, then its fdk/themes
```

Drop a `.fdk` file in `~/.FDKThemes` and it outranks every system
theme — the `~/.fonts` precedent applied to themes.

A theme file is `<stem>.fdk`; the **stem is the handle**
(`matrix` → `matrix.fdk`, exact and case-sensitive). Stems should
be 1–64 characters of `[A-Za-z0-9_-]` — the same character class as
a prefs key half. A theme whose file name differs from its internal
`name` key is reachable by that internal name too (`fdk-theme set
"Pink Rave"` works).

The library surface under all of this is
`fdk_theme_find()` / `fdk_theme_available_count()` /
`fdk_theme_available_name()` / `fdk_theme_available_path()` —
see `fdk_theme.h`. `fdk-theme list` is a print of exactly what
those return, plus the built-in theme.

## fdk-theme

```
fdk-theme list [--paths]      themes on the search path
fdk-theme get [--verbose]    the effective theme, and (with -v) why
fdk-theme set <name>         remember <name> in the global store
fdk-theme reset              forget it (back to Faded Dream)
fdk-theme path [<name>]      the file a theme lives in
```

`set` **validates before writing** — a name that cannot load is
rejected with exit 2 and never stored, because a stored-but-broken
name would silently no-op every future launch (the library's
soft-fail boot is right for applications, wrong for the tool whose
whole job is to say "this works"). `set "Faded Dream"` is accepted
and means reset.

Exit codes (stable, for scripts):

| code | meaning                                             |
|------|-----------------------------------------------------|
| 0    | success                                            |
| 1    | usage error, or a theme that exists but is unusable |
| 2    | theme not found                                    |
| 3    | the settings could not be saved                    |

A typical session:

```
$ fdk-theme list
Faded Dream   (built-in default)
daylight      /usr/local/share/fdk/.FDKThemes/daylight.fdk
faded-dream   /usr/local/share/fdk/.FDKThemes/faded-dream.fdk
matrix        /usr/local/share/fdk/.FDKThemes/matrix.fdk
mono-chromatic /usr/local/share/fdk/.FDKThemes/mono-chromatic.fdk
pink-rave     /usr/local/share/fdk/.FDKThemes/pink-rave.fdk

$ fdk-theme set pink-rave
theme set: Pink Rave (/usr/local/share/fdk/.FDKThemes/pink-rave.fdk)
running FDK applications re-theme the moment this lands (they watch the settings file)

$ fdk-theme set mono-chromatic
theme set: Mono Chromatic (/usr/local/share/fdk/.FDKThemes/mono-chromatic.fdk)
the whole desktop goes black-and-white — circle buttons included

$ fdk-theme get
matrix

$ FDK_THEME=daylight some-fdk-app &      # one launch, one theme
```

## fdk-set (1.4.14)

The per-application face: the same theme setting, scoped to one
application instead of every FDK app on the desktop. Without
`--app` it is `fdk-theme set` spelled differently (same file, same
validation, same exit codes — the tools interoperate); with
`--app <app_id>` it writes the application's OWN `<app_id>.prefs`
store, which outranks the global setting for that application
only:

```
fdk-set theme set "matrix"                    the global default
fdk-set theme set "matrix" --app my.editor   one app's own theme
fdk-set theme get [--app <app_id>] [--verbose]
fdk-set theme reset [--app <app_id>]
fdk-set theme list [--paths]
```

`--app` takes the application id passed to `fdk_init()` — the same
identity that names its prefs file and its taskbar entry (the
examples use `org.fdk.exampleNN`; an application's documentation is
the place its id is stated). `get --app` walks the full precedence
(env, the app's store, the global store) and `--verbose` names the
leg that answered. `reset --app` removes only the override — the
application falls back to the global setting, live in both
directions.

A desktop-shaped session:

```
$ fdk-theme set pink-rave               # the desktop goes all-pink
$ fdk-set theme set daylight --app org.fdk.mywriter
theme set: Daylight (/usr/local/share/fdk/.FDKThemes/daylight.fdk)
org.fdk.mywriter re-themes the moment this lands (it watches its own settings file)

$ fdk-set theme get --app org.fdk.mywriter --verbose
daylight
source: the application's settings (~/.config/org.fdk.mywriter.prefs) [app org.fdk.mywriter]

$ fdk-set theme reset --app org.fdk.mywriter   # back to the desktop default
```

Exit codes match `fdk-theme`'s exactly (0 ok; 1 usage or unusable;
2 not found; 3 could not save).

## fdk-prefs

The generic editor — the same store machinery, any application's
file. Without `-a` it operates on the toolkit's own store; with
`-a <app_id>`, that application's:

```
fdk-prefs list                     every key with its value
fdk-prefs get theme.name           one value (exit 2 when absent)
fdk-prefs set window.width 940     write one key
fdk-prefs remove window.width      drop one key (absent: quiet)
fdk-prefs path                     where the store lives
```

Options: `-a, --app <id>` (default `fdk`), `--default <str>` (`get`:
print this instead of failing when the key is absent).

`set` types its argument the way the format means it: `true` /
`false` → bool, integers → int, decimals → double, anything else →
string. The store canonicalizes, so values round-trip exactly (the
prefs test suite pins that bit-for-bit).

Exit codes: 0 ok; 1 usage or invalid key; 2 no such key (get); 3
cannot save.

## First run, permissions, and honest failures

All three tools create the config directory when it does not exist
(`~/.config` on a minimal system) — that is TOOL policy, not
library policy: `fdk_prefs_save()` deliberately never grows
directory trees, and the tools deliberately never fail on first
run. The split is documented in `tools/toolutil.h`.

The tools silence the library's INFO/WARN chatter (their own
messages are the precise ones) but keep genuine library errors —
a theme file that fails to parse when you asked for it BY NAME
still gets the full per-line diagnostic. A theme file that is
merely discovered broken mid-scan is skipped with one warning and
never hides its healthy neighbors; the same file, asked for by
name, fails loudly. Both postures are deliberate — see
`fdk_theme.h`'s discovery section for the reasoning.

## For packagers

- `make install` lays out: headers + libs as before,
  `$(PREFIX)/bin/fdk-theme`, `$(PREFIX)/bin/fdk-prefs`,
  `$(PREFIX)/bin/fdk-set`, and
  `$(PREFIX)/share/fdk/.FDKThemes/*.fdk` (the five shipped
  faces: `faded-dream` the packaged default, the complete light
  `daylight`, the partial `matrix`, the 1.4.16 `mono-chromatic`
  and `pink-rave`).
- All knobs (`PREFIX`, `BINDIR`, `DATADIR`, `LIBDIR`, `INCDIR`)
  are documented in `docs/build.md`.
- No runtime dependency beyond the library itself. The tools read
  only the XDG environment and the files described above; they
  never touch a display, a compositor, or a daemon — they are safe
  in build scripts and package installers alike.
