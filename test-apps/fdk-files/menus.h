/* menus.h — the menu bar, context menus, and shortcuts. */
#ifndef FILES_MENUS_H
#define FILES_MENUS_H

#include "files.h"

void menus_build(files_app *a);
void menus_popup_context(struct pane *p, int for_what, fdk_i32 x,
                         fdk_i32 y);
void menus_open_shortcut_list(files_app *a);
/* Pop + track an app-built context menu (the model outlives the
 * popup; freed when the chain closes). */
void menus_ctx_popup(fdk_menu *m, fdk_widget *anchor, fdk_i32 x,
                     fdk_i32 y);
void menus_ctx_shutdown(void);

#endif /* FILES_MENUS_H */
