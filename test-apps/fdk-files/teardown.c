/* teardown.c — see teardown.h. */
#include "teardown.h"

#include "fdk/fdk_widgets.h"

#include <stdio.h>
#include <stdlib.h>

#define TD_CAP 4096

typedef struct td_entry {
    void *ptr;
    td_kind kind;
} td_entry;

static td_entry g_entries[TD_CAP];
static size_t g_count;
static int g_full_warned;

void td_track(void *ptr, td_kind kind) {
    if (ptr == NULL) {
        return;
    }
    if (g_count == TD_CAP) {
        if (!g_full_warned) {
            fprintf(stderr,
                    "teardown registry full (%d) — leaking further "
                    "allocations\n",
                    TD_CAP);
            g_full_warned = 1;
        }
        return;
    }
    g_entries[g_count].ptr = ptr;
    g_entries[g_count].kind = kind;
    g_count++;
}

void td_untrack(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    for (size_t i = 0; i < g_count; i++) {
        if (g_entries[i].ptr == ptr) {
            g_entries[i] = g_entries[g_count - 1];
            g_count--;
            return;
        }
    }
}

void td_sweep(void) {
    for (size_t i = 0; i < g_count; i++) {
        switch (g_entries[i].kind) {
        case TD_MENU_MODEL:
            fdk_menu_destroy((fdk_menu *)g_entries[i].ptr);
            break;
        case TD_CTX_MENUS: {
            /* the menus.c array of up to MAX_CTX_MENUS models */
            fdk_menu **arr = g_entries[i].ptr;
            for (int k = 0; k < 8; k++) {
                if (arr[k] != NULL) {
                    fdk_menu_destroy(arr[k]);
                }
            }
            free(arr);
            break;
        }
        case TD_INFO_ARRAY:
        case TD_RAW:
        default:
            free(g_entries[i].ptr);
            break;
        }
    }
    g_count = 0;
}

size_t td_count(void) {
    return g_count;
}
