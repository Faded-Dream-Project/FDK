/* menus.h — the menu bar, context menus, and shortcuts. */
#ifndef FILES_MENUS_H
#define FILES_MENUS_H

#include "files.h"

void menus_build(files_app *a);
void menus_popup_context(struct pane *p, int for_what, fdk_i32 x,
                         fdk_i32 y);
void menus_open_shortcut_list(files_app *a);
void menus_ctx_shutdown(void);

#endif /* FILES_MENUS_H */
