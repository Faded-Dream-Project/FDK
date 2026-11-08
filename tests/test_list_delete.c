/* test_list_delete.c — round 15 regression: interaction after row
 * removal. The engine bug (fixed this round): fdk_list_remove
 * clamped `anchor` but NOT last_click_row/key_cursor — after removing
 * rows, Enter-activate silently died on the past-end key_cursor and
 * click/double-click bookkeeping misfired. Behavior-level, public API
 * only (test_list.c's idioms). Fails on the pre-fix engine at the
 * Enter-after-removals legs.
 */
#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static fdk_font *g_font = NULL;

static fdk_event_data ev_key_evt(fdk_scancode sc) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    return e;
}

static int s_activated = -1;
static int s_activate_count = 0;

static void on_activate(fdk_widget *w, size_t row, void *user) {
    (void)w; (void)user;
    s_activated = (int)row;
    s_activate_count++;
}

int main(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    for (int i = 0; candidates[i] != NULL; i++) {
        /* Existence gate first: a candidate for ANOTHER distro's
         * layout is a normal miss, not an error — the loader's
         * ERROR log stays loud for candidates that exist but fail. */
        if (access(candidates[i], R_OK) != 0) {
            continue;
        }
        g_font = fdk_font_load(candidates[i], 16);
        if (g_font != NULL) {
            break;
        }
    }
    if (g_font == NULL) {
        printf("[skip] no system TrueType font found\n");
        return 0;
    }

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 400},
                                    &root)));
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    fdk_list_set_on_row_activate(list, on_activate, NULL);
    assert(fdk_widget_focus(list));

    for (int i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof name, "row%d", i);
        size_t idx = 0;
        assert(fdk_ok(fdk_list_append(list, name, &idx)));
    }
    assert(fdk_list_row_count(list) == 10);

    /* six notches from cold start: the keyboard cursor sits at
     * row 5 */
    fdk_event_data down = ev_key_evt(FDK_KEY_DOWN);
    for (int i = 0; i < 6; i++) {
        assert(fdk_widget_tree_handle_event(root, &down));
    }
    assert(fdk_list_get_selected(list) == 5);

    /* remove every row from index 2 up: the count shrinks to 2
     * UNDER the live cursor (5) — the delete-a-file-then-use shape
     * that leaves a stale past-end key_cursor */
    for (int i = 0; i < 8; i++) {
        assert(fdk_ok(fdk_list_remove(list, 2)));
    }
    assert(fdk_list_row_count(list) == 2);

    s_activate_count = 0;
    fdk_event_data enter = ev_key_evt(FDK_KEY_ENTER);
    assert(fdk_widget_tree_handle_event(root, &enter));
    assert(s_activate_count == 1);
    assert(s_activated >= 0 && s_activated <= 1);

    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_list_get_selected(list) >= 0 &&
           fdk_list_get_selected(list) <= 1);

    assert(fdk_ok(fdk_list_select(list, 0)));
    assert(fdk_ok(fdk_list_remove(list, 0)));
    assert(fdk_list_row_count(list) == 1);
    s_activate_count = 0;
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_widget_tree_handle_event(root, &enter));
    assert(s_activate_count == 1);

    printf("[ok] list delete: interaction alive after removals "
           "(Enter-activate, arrows)\n");
    fdk_widget_destroy(root);
    return 0;
}
