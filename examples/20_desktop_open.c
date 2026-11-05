/* 20_desktop_open.c — fdk_desktop_open_uri: the detached XDG opener.
 *
 * A window with one entry + button; typing a URL and pressing the
 * button (or Enter) hands the URI to the desktop's configured
 * handler via xdg-open — detached (setsid, /dev/null stdio), never
 * blocking the loop. The honest outcomes print on the status label:
 * OK (launched), UNSUPPORTED (no xdg-open on PATH), or the refusal
 * code for a string that failed validation. */
#include "example_window.h"

#include "fdk/fdk_desktop.h"

static fdk_example ex;
static fdk_widget *g_status;

static void open_clicked(fdk_widget *button, void *user) {
    (void)button;
    fdk_widget *entry = user;
    const char *uri = fdk_entry_get_text(entry);
    fdk_result r = fdk_desktop_open_uri(uri);
    switch (r) {
    case FDK_OK:
        fdk_label_set_text(g_status, "launched (the desktop owns it now)");
        break;
    case FDK_ERR_UNSUPPORTED:
        fdk_label_set_text(g_status,
                           "no xdg-open on PATH — nothing to hand it to");
        break;
    default:
        fdk_label_set_text(g_status, "refused: URI failed validation");
        break;
    }
}

static void entry_activated(fdk_widget *entry, void *user) {
    (void)entry;
    open_clicked(NULL, user);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "20")) {
        return 1;
    }
    if (!fdk_example_open(&ex, ctx, "20",
                          "desktop open (the detached XDG opener)", 520,
                          170)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_font *font = ex.ui;

    fdk_widget *row = NULL;
    fdk_box_create(ex.content, FDK_HORIZONTAL, &row);
    fdk_box_set_padding(row, 12);
    fdk_box_set_spacing(row, 8);

    fdk_widget *entry = NULL;
    fdk_entry_create(row, font, "https://example.org/", &entry);
    fdk_widget_set_expand(entry, true, false);
    fdk_entry_set_on_activate(entry, entry_activated, entry);

    fdk_widget *btn = NULL;
    fdk_button_create(row, font, "Open", &btn);
    fdk_button_set_role(btn, FDK_BUTTON_ROLE_SUGGESTED);
    fdk_button_set_on_activate(btn, open_clicked, entry);

    fdk_label_create(ex.content, font,
                     "Enter a URI (scheme:...) or an absolute path",
                     &g_status);
    fdk_widget_set_natural_size(g_status, 480, 24);

    fdk_example_run(&ex);
    fdk_shutdown(ctx);
    return 0;
}
