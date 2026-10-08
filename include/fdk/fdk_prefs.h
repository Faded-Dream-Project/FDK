/*
 * fdk_prefs.h — application preferences (settings persistence)
 *
 * The settings half of a serious application: remember the window
 * size, the last browsed directory, the toggles the user set —
 * across restarts, in a human-editable file, under the user's own
 * config directory. The API is deliberately tiny: open, typed
 * get/set with defaults, save. Everything else is policy the
 * application owns.
 *
 * STORAGE. One file per application, resolved as:
 *
 *   1. $FDK_PREFS_FILE          — an explicit override (the test
 *                                 suite and sandboxed rigs; absolute)
 *   2. $XDG_CONFIG_HOME/<app_id>.prefs   (default ~/.config/)
 *   3. absent XDG and HOME      — the store is memory-only; save()
 *                                 returns FDK_ERR_UNSUPPORTED
 *                                 (documented, not faked)
 *
 * The file format is FDK's third strict line-based dialect (the
 * theme and message-catalog grammars came first); the normative
 * reference is docs/fdk-prefs-format.md. Summary: `[section]`
 * headers, `name = value` lines, `#`/`//` comments, LF or CRLF,
 * bounded to 1 MiB / 1024-byte lines / 8192 keys / 256-byte values,
 * strict UTF-8, duplicate keys are load errors.
 *
 * FAILURE POSTURE — the one deliberate difference from the theme
 * and catalog parsers, and it is the resilience rule for settings:
 * a corrupt or unreadable prefs file NEVER takes the application
 * down. fdk_prefs_open logs the parse error with its line number
 * and hands back an EMPTY store — every getter then serves its
 * default, the app runs like a first launch, and the next save()
 * rewrites the file cleanly. (A half-themed UI is worse than no
 * theme, so themes fail loud; a dead settings file must not kill a
 * working app, so prefs fail soft. Both rules are the right rule
 * for their own kind of file.)
 *
 * LIFECYCLE. get/set operate on the in-memory store; NOTHING
 * touches the disk until fdk_prefs_save(). save() writes a
 * temporary file in the same directory and renames it over the
 * target — atomic on POSIX: a crash mid-save can leave a .tmp
 * residue but never a truncated prefs file. destroy() WITHOUT an
 * intervening save() discards changes (the application saves at
 * quit and at meaningful checkpoints; autosave-every-set is a
 * policy the application can trivially implement on top).
 *
 * KEYS. A key is `section.name` — both halves 1..64 chars of
 * [A-Za-z0-9_-], the dot separating exactly one section level
 * (the whole FDK family stops at one level of nesting; a settings
 * file that needs deeper structure needs a real document format,
 * not a prefs file). Keys are CASE-SENSITIVE; ordering in the file
 * is section-then-key insertion order, and save() rewrites the
 * file wholesale in that order (comments in the source file are
 * not preserved across a save — a generated header comment is
 * always written instead; hand-edits to values survive, hand-added
 * comments do not. Documented, deliberate).
 */

#ifndef FDK_PREFS_H
#define FDK_PREFS_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fdk_prefs fdk_prefs;

/* ---- lifecycle ------------------------------------------------------- */

/* Opens (or creates empty) the application's preference store.
 * `app_id` is the application's identity: the file name stem under
 * the config directory (see the header comment for resolution) and
 * the generated header comment in the saved file. NULL/empty
 * app_id is FDK_ERR_INVALID_ARGUMENT.
 *
 * The file is READ here (a missing file is a clean first run — an
 * empty store, no warning); a parse failure yields an empty store
 * and ONE warning carrying the offending line (the resilience
 * rule above). The caller never distinguishes first-run from
 * corrupt-file via the return value — FDK_OK in both cases — but
 * can via fdk_prefs_source() if it cares. */
fdk_result fdk_prefs_open(const char *app_id, fdk_prefs **out_prefs);
void fdk_prefs_destroy(fdk_prefs *prefs);

/* Why the store was empty at open (diagnostics/telemetry):
 * FDK_PREFS_SOURCE_FILE (loaded a real file),
 * FDK_PREFS_SOURCE_FRESH (no file existed — first run),
 * FDK_PREFS_SOURCE_RECOVERED (a file existed but was rejected;
 * getters serve defaults until the next save rewrites it). */
typedef enum {
    FDK_PREFS_SOURCE_FILE = 0,
    FDK_PREFS_SOURCE_FRESH = 1,
    FDK_PREFS_SOURCE_RECOVERED = 2,
} fdk_prefs_origin;
fdk_prefs_origin fdk_prefs_source(const fdk_prefs *prefs);

/* ---- typed access ----------------------------------------------------
 *
 * Getters return the DEFAULT when the key is absent, when its value
 * does not parse as the requested type, or when the store is
 * NULL-received — the "never an error, always an answer" contract
 * (a settings read must not be a failure path in an application).
 * A present-but-unparsable value is worth knowing about though, so
 * it logs one DEBUG line the first time that key is read (not per
 * call — the log is for diagnosing a hand-edited file, not for
 * spamming a render loop).
 *
 * Setters validate the KEY grammar strictly (the family rule:
 * typos fail loud, not silent) and copy the value; they touch no
 * disk. NULL prefs or a malformed key is FDK_ERR_INVALID_ARGUMENT.
 * Values are stored as canonical strings; set_int/bool/double
 * format exactly as their getters parse (round-trip stability is
 * pinned by tests: save, reopen, compare bit-for-bit). */

const char *fdk_prefs_get(const fdk_prefs *prefs, const char *key,
                          const char *def);
long fdk_prefs_get_int(const fdk_prefs *prefs, const char *key,
                       long def);
bool fdk_prefs_get_bool(const fdk_prefs *prefs, const char *key,
                        bool def);
double fdk_prefs_get_double(const fdk_prefs *prefs, const char *key,
                            double def);

fdk_result fdk_prefs_set(fdk_prefs *prefs, const char *key,
                         const char *value);
fdk_result fdk_prefs_set_int(fdk_prefs *prefs, const char *key,
                             long value);
fdk_result fdk_prefs_set_bool(fdk_prefs *prefs, const char *key,
                              bool value);
fdk_result fdk_prefs_set_double(fdk_prefs *prefs, const char *key,
                                double value);

/* Removes a key (absent key: quiet no-op, like the undo stack's
 * blind-pop). The removal is in-memory until save(). */
fdk_result fdk_prefs_remove(fdk_prefs *prefs, const char *key);

/* ---- persistence ----------------------------------------------------- */

/* Writes the store to the resolved path via temp-file + rename
 * (atomic). The generated header names the format and app_id.
 * A memory-only store (no resolvable directory) returns
 * FDK_ERR_UNSUPPORTED; I/O failures return FDK_ERR_IO with
 * the cause logged. save() on an UNCHANGED store is a no-op that
 * returns FDK_OK without touching the disk (checksummed dirty
 * flag — repeated save-on-timer costs nothing). */
fdk_result fdk_prefs_save(fdk_prefs *prefs);

/* The file path this store saves to (diagnostics; the string is
 * owned by the store, valid until destroy; NULL for a memory-only
 * store). */
const char *fdk_prefs_path(const fdk_prefs *prefs);

/* ---- iteration (diagnostics/export) ---------------------------------- */

size_t fdk_prefs_count(const fdk_prefs *prefs);
/* NULL (and not the empty string) past the end. Keys arrive in
 * section-then-insertion order — exactly the save() layout. */
const char *fdk_prefs_key_at(const fdk_prefs *prefs, size_t index);
const char *fdk_prefs_value_at(const fdk_prefs *prefs, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* FDK_PREFS_H */
