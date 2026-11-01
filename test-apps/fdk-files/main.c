/* main.c — placeholder during the skeleton bring-up; the app's real
 * multi-file implementation replaces this. */
#include "../common/app.h"

int main(void) {
    ta_app app;
    if (!ta_init(&app, "placeholder")) {
        return 1;
    }
    if (!ta_open(&app, "FDK test app", 900, 600)) {
        ta_close(&app);
        return 1;
    }
    ta_status(&app, "skeleton");
    ta_run(&app);
    ta_close(&app);
    return 0;
}
