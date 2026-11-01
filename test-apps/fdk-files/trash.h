/* trash.h — the freedesktop trash implementation for fdk-files.
 *
 * Spec: the Trash Specification (specifications.freedesktop.org/
 * trash). Home trash at $XDG_DATA_HOME/Trash (default ~/.local/
 * share/Trash) with files/ and info/ subdirs; a .trashinfo per
 * entry ([Trash Info] / Path= / DeletionDate=); same-filesystem
 * trash = rename() into files/ with a unique name when a clash
 * occurs; cross-device falls back to copy+unlink. Restoring reads
 * Path from the info file.
 */
#ifndef FILES_TRASH_H
#define FILES_TRASH_H

#include "files.h"

#include <stddef.h>

/* The home trash dir ($XDG_DATA_HOME/Trash), created on demand
 * (files/ + info/). Returns false when it cannot exist. */
bool trash_dir(char *out, size_t cap);

/* Moves `path` into the trash. Writes the .trashinfo FIRST and
 * atomically (tmp + rename, O_EXCL chosen name), then moves the
 * payload. Returns false with a reason in `err` (cap bytes). */
bool trash_file(const char *path, char *err, size_t cap);

/* Enumerates the trash; returns entries as (name, orig_path,
 * deletion_date). Free with trash_list_free. */
typedef struct trash_entry {
    char name[FI_NAME_MAX];       /* inside files/                */
    char orig_path[FI_PATH_MAX];  /* from Path=                   */
    char deleted[32];             /* DeletionDate= raw            */
} trash_entry;

size_t trash_list(trash_entry **out_entries);

/* Restores a trash entry by its files/ name to orig_path. */
bool trash_restore(const char *trash_name, char *err, size_t cap);

/* Deletes one entry permanently (payload + info). */
bool trash_delete(const char *trash_name, char *err, size_t cap);

/* Empties the whole trash. */
bool trash_empty(char *err, size_t cap);

/* The trash's total size, walking files/ (no cache file — we keep
 * the directorysizes cache OUT: it is an optimization, and the
 * honest README says so). */
off_t trash_size(void);

void trash_list_free(trash_entry *entries);

#endif /* FILES_TRASH_H */
