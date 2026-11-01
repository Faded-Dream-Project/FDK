/* teardown.h — the fdk-files allocation registry (§25/4a).
 *
 * FDK widgets die with their window, but every payload the app
 * callocs for callbacks (crumbs, view contexts, dialog contexts)
 * and every fdk_menu MODEL outliving its popup has no FDK-side
 * owner — the Canvas class has no destroy hook, the menu bar keeps
 * its models until fdk_menu_destroy. This registry is the one
 * owner: allocations register here at creation, ta_close drains
 * them at app teardown (after the window died), LSan stays clean.
 *
 * NOT a framework: an array of (ptr, free_how) and three calls.
 */
#ifndef FILES_TEARDOWN_H
#define FILES_TEARDOWN_H

#include <stddef.h>

/* What the pointer is, so the sweep frees it the right way. */
typedef enum td_kind {
    TD_RAW = 0,     /* free()                          */
    TD_MENU_MODEL,  /* fdk_menu_destroy()              */
    TD_CTX_MENUS,   /* the menus.c context-menu array  */
    TD_INFO_ARRAY,  /* model.c's infos batch           */
} td_kind;

/* Registers `ptr` (ignored when NULL or the table is full — the
 * honest cap; a full table logs once). */
void td_track(void *ptr, td_kind kind);

/* Drops `ptr` from the registry WITHOUT freeing (the owner freed
 * it early — a closed dialog's window, a replaced model). */
void td_untrack(void *ptr);

/* Frees everything tracked. Call once at app teardown, AFTER the
 * window/tree died and after each app module freed what it knows. */
void td_sweep(void);

/* The count currently tracked (diagnostics for the smoke rig). */
size_t td_count(void);

#endif /* FILES_TEARDOWN_H */
