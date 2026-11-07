/* wide.h — character cell widths for the terminal grid.
 *
 * A from-scratch wcwidth implementation (the Markus Kuhn shape: the
 * Unicode width tables distilled to range checks — zero for combining
 * marks, two for the East Asian Wide/Fullwidth blocks, one otherwise,
 * and -1 for the C0/C1 controls the VT layer never places anyway).
 * Ligatures are OFF by design (a terminal grid advances per cell;
 * what a font does inside a cell is its business — 'liga' never
 * merges cells here), which is also what the brief demands.
 *
 * Emoji: the extended-pictographic set reports width 2 (the terminal
 * convention every modern emulator follows).
 *
 * No locale dependency, no wchar_t games: the terminal decodes its
 * own UTF-8, so the input is a codepoint. */
#ifndef FDKTERM_WIDE_H
#define FDKTERM_WIDE_H

#include <stdint.h>
#include <stdbool.h>

/* Width in terminal cells of a codepoint: 0 (combining), 1, or 2.
 * Control characters (< 0x20, 0x7F..0x9F) return 0 — the VT layer
 * consumes them before they ever reach the grid. */
int tw_width(uint32_t cp);

/* Is this a combining mark (width 0, attaches to the cell before)? */
bool tw_is_combining(uint32_t cp);

/* Is this codepoint a wide (2-cell) character? */
bool tw_is_wide(uint32_t cp);

#endif /* FDKTERM_WIDE_H */
