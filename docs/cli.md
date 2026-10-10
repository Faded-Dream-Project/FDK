# The FDK Command-Line Tools

FDK's settings face for shells and scripts: `fdk-theme` and
`fdk-prefs` (1.4.13). Every desktop toolkit ends up needing this —
GTK has `gsettings`, Qt has its platform-theme configuration — for
one reason: **most applications never ship a settings UI**, and a
desktop where changing the theme requires each application to have
built a theme picker is a desktop that cannot change its theme.
FDK's answer is two small tools over one global settings file.

Both tools are ordinary applications on the public API — built by
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
  else theme.name in the fdk store
  else the built-in "FDK Modern"
```

An application that calls `fdk_theme_set_default()` before its
first paint **owns the process's theme** — the global setting never
overrides an explicit choice. That is the same contract GTK gives:
`GTK_THEME` beats the setting, an explicit gtk-theme-name set in
code beats everything. The precedence rules live in
`include/fdk/fdk_theme.h`; the boot code is `src/theme/theme.c`.

Failure is soft at every step, on purpose (the prefs resilience
rule): a missing or corrupt settings file means "no preference",
and a name that does not resolve on the search path logs one
warning and stays on the built-in theme. **A themed launch must
never be a failed launch.**

Running applications do not live-reload the setting — new launches
pick it up. (Live re-theming is what `fdk_theme_set_default()` is
for, in an app that offers a picker.)

## Theme discovery — where themes live

`fdk-theme` never takes a file path; it takes a NAME, resolved
against the search path (XDG basedir order):

```
$FDK_THEME_DIR                        (one explicit directory)
  $XDG_DATA_HOME/fdk/themes           (default ~/.local/share/fdk/themes)
  $XDG_DATA_DIRS/.../fdk/themes       (default /usr/local/share:/usr/share)
```

`make install` puts FDK's shipped themes into
`$(PREFIX)/share/fdk/themes` — the default `XDG_DATA_DIRS` already
covers `/usr/local/share` and `/usr/share`, so an installed FDK
finds them with zero configuration.

A theme file is `<stem>.fdk`; the **stem is the handle**
(`matrix` → `matrix.fdk`, exact and case-sensitive). Stems should
be 1–64 characters of `[A-Za-z0-9_-]` — the same character class as
a prefs key half. A theme whose file name differs from its internal
`name` key is reachable by that internal name too (`fdk-theme set
"Daylight"` works).

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
fdk-theme reset              forget it (back to FDK Modern)
fdk-theme path [<name>]      the file a theme lives in
```

`set` **validates before writing** — a name that cannot load is
rejected with exit 2 and never stored, because a stored-but-broken
name would silently no-op every future launch (the library's
soft-fail boot is right for applications, wrong for the tool whose
whole job is to say "this works"). `set "FDK Modern"` is accepted
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
FDK Modern    (built-in default)
daylight      /usr/local/share/fdk/themes/daylight.fdk
matrix        /usr/local/share/fdk/themes/matrix.fdk

$ fdk-theme set matrix
theme set: Matrix (/usr/local/share/fdk/themes/matrix.fdk)
new FDK applications will use it; running ones keep their current theme

$ fdk-theme get
matrix

$ FDK_THEME=daylight some-fdk-app &      # one launch, one theme
```

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

Both tools create the config directory when it does not exist
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
  `$(PREFIX)/bin/fdk-theme`, `$(PREFIX)/bin/fdk-prefs`, and
  `$(PREFIX)/share/fdk/themes/*.fdk` (the shipped themes: the
  complete light `daylight` and the partial `matrix`).
- All knobs (`PREFIX`, `BINDIR`, `DATADIR`, `LIBDIR`, `INCDIR`)
  are documented in `docs/build.md`.
- No runtime dependency beyond the library itself. The tools read
  only the XDG environment and the files described above; they
  never touch a display, a compositor, or a daemon — they are safe
  in build scripts and package installers alike.
