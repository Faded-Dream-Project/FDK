/* bookmarks.h — the sidebar's bookmarks (§26/3a).
 *
 * A persistent, drag-reorderable list of folders in the sidebar:
 *   - add:      Ctrl+D on the active tab's folder (or DnD a folder
 *               onto the bookmark list)
 *   - remove:   the context menu's "Remove bookmark" on a bookmark
 *               row (the standard gesture)
 *   - reorder:  drag a bookmark row onto another position of the
 *               list (a URI drag of the bookmarked folder; a drop
 *               whose URI equals a bookmark's own path is a MOVE,
 *               not an add)
 *   - persist:  XDG_CONFIG_HOME/fdk-files/bookmarks (one path per
 *               line, order-preserving; comments with #)
 *   - activate: navigate the active pane
 *
 * Written only against FDK's public API: the list widget's insert/
 * remove + row icons, the widget drag callback with window-local
 * coordinates mapped to rows via the public row height and bounds.
 */
#ifndef FILES_BOOKMARKS_H
#define FILES_BOOKMARKS_H

#include "files.h"

#define FI_BOOKMARKS_MAX 32
#define FI_BOOKMARK_LABEL 64

typedef struct fi_bookmark {
    char path[FI_PATH_MAX];
    char label[FI_BOOKMARK_LABEL];
} fi_bookmark;

/* Loads from XDG config (missing file = the empty list). */
void bookmarks_load(void);

/* Saves to XDG config (best effort; a read-only HOME just loses
 * persistence, never crashes). */
void bookmarks_save(void);

/* The in-memory list (order = display order). */
size_t bookmarks_count(void);
const fi_bookmark *bookmarks_at(size_t i);

/* Adds a folder if not already present (no-op duplicate). Returns
 * the new index or -1 (full / not a folder path). */
int bookmarks_add(const char *path);

/* Removes by path. True when something was removed. */
bool bookmarks_remove(const char *path);

/* Moves the bookmark at `from` to `to` (clamped). True on success. */
bool bookmarks_move(size_t from, size_t to);

/* Builds the sidebar section (the label + the list) into `parent`
 * and wires activation + the context menu + the drop target. */
void bookmarks_build(fdk_widget *parent);

/* Rebuilds the list rows from the in-memory state. */
void bookmarks_refresh(void);

/* The list widget (for teardown sweeps / tests). */
fdk_widget *bookmarks_widget(void);

#endif /* FILES_BOOKMARKS_H */
