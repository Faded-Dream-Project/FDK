#define FDK_LOG_TAG "atspi"

/*
 * atspi.c — the AT-SPI2 accessibility bridge, level 1 (1.1.0, S-8 / B-10).
 *
 * Exposes FDK applications to OS screen readers (Orca, NVDA+atspi,
 * Dragon accessibility stacks) over the native D-Bus client — the
 * roadmap's "roles/names/states/focus/activate over M-7's D-Bus; text
 * interfaces later" scope. The internal a11y tree (fdk_a11y.h — rich,
 * tested, virtual-children-aware since 1.4.x) is the data source; this
 * module is only the WIRE.
 *
 * The two-bus handshake (the at-spi2 architecture):
 *
 *   1. the SESSION bus carries org.a11y.Bus.GetAddress -> the
 *      accessibility bus's own address (a second daemon, started by
 *      at-spi2, that only carries accessibility traffic);
 *   2. the bridge connects to THAT address (fdk_dbus_connect_address)
 *      and serves the object tree there.
 *
 * Every exported object is a D-Bus path /org/freedesktop/atspi/
 * accessible/<id> (id 0 is the application root, "/.../root") with
 * three interfaces:
 *
 *   org.freedesktop.atspi.Accessible — the tree: Role/RoleName/
 *     State/ChildCount/GetChildren/GetChildAtIndex/GetIndexInParent/
 *     GetApplication/GetInterfaces/GetAttributes (+ the Name/
 *     Description/Parent properties through org.freedesktop.DBus.
 *     Properties.Get);
 *   org.freedesktop.atspi.Component — GetExtents/GetPosition/
 *     GetSize in desktop coordinates (window position + the a11y
 *     snapshot's root-absolute bounds);
 *   org.freedesktop.atspi.Action — DoAction(0) == the widget's
 *     ACTIVATE (the same path a screen reader's "click" takes), with
 *     GetNActions/GetName describing it.
 *
 * VIRTUAL CHILDREN (painted-row containers — lists, trees, icon
 * views) get their own object paths keyed (container, index); a
 * screen reader sees the rows, not an empty box.
 *
 * Events: the bridge subscribes to the internal a11y notifications
 * (scope NULL — the documented bridge pattern) and emits the AT-SPI
 * signals screen readers listen for: Event.Object.Focus (detail
 * "focus") on focus gains, StateChanged for the state flags that
 * changed, PropertyChange for name/description edits, ChildrenChanged
 * for tree edits.
 *
 * Engagement: the pump's attach tick (window.c's scheme tick calls
 * fdk__atspi_tick) — throttled to ONE attempt per context, degraded
 * silently and permanently when the desktop has no a11y bus (the
 * honest no-daemon posture the audit row asked to close).
 * $FDK_ATSPI=0 disables; =1 forces (a missing bus then logs the
 * failure at WARN — the testing posture).
 *
 * Liveness: the registry is pointer-keyed; CHILDREN_CHANGED events
 * re-diff each exported parent's children and kill gone entries, and
 * every method call verifies reachability from a live root (address
 * comparison only — a dead pointer is never dereferenced). The
 * pointer-reuse alias case is documented: the diff makes it rare, the
 * backstop makes it harmless in the common paths.
 */

#include "fdk/fdk_a11y.h"
#include "fdk/fdk_dbus.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
/* fdk_text_utf8_next: text.c's internal UTF-8 stepper (the cp<->byte
 * conversion below rides it). Declared here instead of including
 * text_internal.h, whose stb_truetype include needs the text TU's
 * own -I path. */
int fdk_text_utf8_next(const char *s, size_t len, size_t i,
                       fdk_u32 *out_cp);
#include "widget/widget_internal.h"
#include "widget/widgets_internal.h" /* the C-2 text-extents seam     */
#include "window/window_internal.h" /* the owner back-edge + position op */
#include "desktop/atspi_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- constants ---------------------------------------------------------- */

#define ATSPI_PREFIX "/org/freedesktop/atspi/accessible/"
#define IFACE_ACCESSIBLE "org.freedesktop.atspi.Accessible"
#define IFACE_ACTION "org.freedesktop.atspi.Action"
#define IFACE_COMPONENT "org.freedesktop.atspi.Component"
#define IFACE_TEXT "org.freedesktop.atspi.Text"
#define IFACE_EVENT_OBJECT "org.freedesktop.atspi.Event.Object"
#define IFACE_DBUS_PROPS "org.freedesktop.DBus.Properties"
/* The retry interval: a screen reader toggled on mid-session must
 * still find running apps, so failures RETRY on this throttle instead
 * of degrading permanently (the color-scheme portal's shape). The
 * INFO line fires once per attempt; the successful attach line once
 * ever. */
#define ATSPI_RETRY_MS 2000u

#define A11Y_BUS_NAME "org.a11y.Bus"
#define A11Y_BUS_PATH "/org/a11y/bus"
#define A11Y_BUS_IFACE "org.a11y.Bus"

/* ---- the registry ------------------------------------------------------- */

/* What one exported object stands for. */
typedef enum {
    ATK_OBJ_APP = 0,     /* the application root (id 0)              */
    ATK_OBJ_WIDGET,      /* a real widget                            */
    ATK_OBJ_VIRTUAL,     /* (container, index) — a painted row       */
} atk_obj_kind;

#define ATSPI_MAX_OBJECTS 2048

typedef struct {
    int used;
    atk_obj_kind kind;
    fdk_widget *w;   /* the widget (or the row's container)          */
    int vindex;      /* ATK_OBJ_VIRTUAL only                          */
} atspi_obj;

static struct {
    int engaged;      /* a session attempt is in flight              */
    int degraded;     /* explicitly disabled: never again             */
    uint64_t last_try_ms; /* the retry throttle (late-starting        *
                           * screen readers are a REAL case: the      *
                           * user toggles one on mid-session)         */
    fdk_context *ctx;
    fdk_dbus_connection *session; /* for the GetAddress handshake    */
    fdk_dbus_connection *bus;     /* the a11y bus itself             */
    int sub_active;   /* the internal a11y subscription is in        */
    atspi_obj objs[ATSPI_MAX_OBJECTS];
    int next_free;    /* round-robin free-slot hint                  */
} g_atspi;

/* The object path for a registry index. */
static void obj_path(char *out, size_t cap, int index) {
    if (index == 0) {
        snprintf(out, cap, "%sroot", ATSPI_PREFIX);
    } else {
        snprintf(out, cap, "%s%d", ATSPI_PREFIX, index);
    }
}

/* Allocates a registry slot for the widget/virtual pair. Returns the
 * index or -1 (full). */
static int obj_register(atk_obj_kind kind, fdk_widget *w, int vindex) {
    for (int tries = 0; tries < ATSPI_MAX_OBJECTS; tries++) {
        int i = g_atspi.next_free;
        g_atspi.next_free = (g_atspi.next_free + 1) % ATSPI_MAX_OBJECTS;
        if (!g_atspi.objs[i].used) {
            g_atspi.objs[i].used = 1;
            g_atspi.objs[i].kind = kind;
            g_atspi.objs[i].w = w;
            g_atspi.objs[i].vindex = vindex;
            return i;
        }
    }
    FDK_WARN("atspi: object registry full (%d) — the tree beyond is "
             "invisible to screen readers", ATSPI_MAX_OBJECTS);
    return -1;
}

/* The slot holding (widget, ANY virtual index) or the widget itself;
 * -1 when absent. */
static int obj_find_widget(const fdk_widget *w) {
    for (int i = 0; i < ATSPI_MAX_OBJECTS; i++) {
        if (g_atspi.objs[i].used &&
            g_atspi.objs[i].w == w) {
            return i;
        }
    }
    return -1;
}

static int obj_find_virtual(const fdk_widget *container, int vindex) {
    for (int i = 0; i < ATSPI_MAX_OBJECTS; i++) {
        if (g_atspi.objs[i].used &&
            g_atspi.objs[i].kind == ATK_OBJ_VIRTUAL &&
            g_atspi.objs[i].w == container &&
            g_atspi.objs[i].vindex == vindex) {
            return i;
        }
    }
    return -1;
}

/* Kills every entry whose widget is no longer reachable from a live
 * root (address comparison ONLY — the pointers of dead widgets are
 * never dereferenced). Called after tree edits and at every method
 * call's head (cheap enough: bounded by exported breadth, and the
 * walk breaks at the first root hit). */
/* Depth-first address-only walk (the candidate pointer is never
 * dereferenced — dead widgets are compared, not touched). */
static bool reachable_rec(fdk_widget *node, const fdk_widget *w) {
    if (node == w) {
        return true;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (reachable_rec(node->children[i], w)) {
            return true;
        }
    }
    return false;
}

static bool widget_reachable(const fdk_widget *w) {
    for (fdk_widget *root = fdk__widget_roots_head(); root != NULL;
         root = root->root_next) {
        if (reachable_rec(root, w)) {
            return true;
        }
    }
    return false;
}

static void registry_gc(void) {
    for (int i = 1; i < ATSPI_MAX_OBJECTS; i++) {
        if (g_atspi.objs[i].used && g_atspi.objs[i].w != NULL &&
            !widget_reachable(g_atspi.objs[i].w)) {
            g_atspi.objs[i].used = 0;
        }
    }
}

/* ---- role / state mapping ----------------------------------------------- */

/* FDK_A11Y_ROLE -> the AtspiRole enum (at-spi2-core's numbering —
 * the values every screen reader keys on). */
static unsigned role_to_atspi(fdk_a11y_role role) {
    switch (role) {
    case FDK_A11Y_ROLE_WINDOW: return 68u;        /* WINDOW          */
    case FDK_A11Y_ROLE_DIALOG: return 16u;        /* DIALOG          */
    case FDK_A11Y_ROLE_APPLICATION: return 74u;   /* APPLICATION     */
    case FDK_A11Y_ROLE_PANEL: return 39u;         /* PANEL           */
    case FDK_A11Y_ROLE_SCROLL_AREA: return 49u;   /* SCROLL_PANE     */
    case FDK_A11Y_ROLE_SCROLL_BAR: return 48u;    /* SCROLL_BAR      */
    case FDK_A11Y_ROLE_LIST: return 31u;          /* LIST            */
    case FDK_A11Y_ROLE_TREE: return 64u;          /* TREE            */
    case FDK_A11Y_ROLE_TOOLBAR: return 62u;       /* TOOL_BAR        */
    case FDK_A11Y_ROLE_MENU_BAR: return 34u;      /* MENU_BAR        */
    case FDK_A11Y_ROLE_MENU: return 33u;          /* MENU            */
    case FDK_A11Y_ROLE_TAB_LIST: return 38u;      /* PAGE_TAB_LIST   */
    case FDK_A11Y_ROLE_GROUP: return 39u;         /* PANEL           */
    case FDK_A11Y_ROLE_LIST_ITEM: return 32u;     /* LIST_ITEM       */
    case FDK_A11Y_ROLE_TREE_ITEM: return 32u;     /* LIST_ITEM (the
                                                    * honest level-1
                                                    * stand-in)      */
    case FDK_A11Y_ROLE_MENU_ITEM: return 35u;     /* MENU_ITEM       */
    case FDK_A11Y_ROLE_CHECK_MENU_ITEM: return 8u;/* CHECK_MENU_ITEM */
    case FDK_A11Y_ROLE_RADIO_MENU_ITEM: return 45u;
    case FDK_A11Y_ROLE_TAB: return 37u;           /* PAGE_TAB        */
    case FDK_A11Y_ROLE_OPTION: return 32u;        /* LIST_ITEM       */
    case FDK_A11Y_ROLE_BUTTON: return 43u;        /* PUSH_BUTTON     */
    case FDK_A11Y_ROLE_TOGGLE_BUTTON: return 61u; /* TOGGLE_BUTTON   */
    case FDK_A11Y_ROLE_CHECK_BOX: return 7u;      /* CHECK_BOX       */
    case FDK_A11Y_ROLE_RADIO_BUTTON: return 44u;  /* RADIO_BUTTON    */
    case FDK_A11Y_ROLE_LABEL: return 29u;         /* LABEL           */
    case FDK_A11Y_ROLE_ENTRY: return 78u;         /* ENTRY           */
    case FDK_A11Y_ROLE_SLIDER: return 51u;        /* SLIDER          */
    case FDK_A11Y_ROLE_SPIN_BUTTON: return 52u;   /* SPIN_BUTTON     */
    case FDK_A11Y_ROLE_COMBO_BOX: return 11u;     /* COMBO_BOX       */
    case FDK_A11Y_ROLE_PROGRESS_BAR: return 42u;  /* PROGRESS_BAR    */
    case FDK_A11Y_ROLE_SEPARATOR: return 50u;     /* SEPARATOR       */
    case FDK_A11Y_ROLE_CANVAS: return 6u;         /* CANVAS          */
    case FDK_A11Y_ROLE_STATUS_BAR: return 54u;    /* STATUS_BAR      */
    case FDK_A11Y_ROLE_SPINNER: return 43u;       /* PUSH_BUTTON (no
                                                    * spinner role)  */
    case FDK_A11Y_ROLE_SPLIT_PANE: return 53u;    /* SPLIT_PANE      */
    case FDK_A11Y_ROLE_EXPANDER: return 61u;      /* TOGGLE_BUTTON   */
    case FDK_A11Y_ROLE_LEVEL_BAR: return 42u;     /* PROGRESS_BAR    */
    case FDK_A11Y_ROLE_MENU_BUTTON: return 43u;   /* PUSH_BUTTON     */
    case FDK_A11Y_ROLE_COLOR_CHOOSER: return 9u;  /* COLOR_CHOOSER   */
    case FDK_A11Y_ROLE_FONT_CHOOSER: return 21u;  /* FONT_CHOOSER    */
    case FDK_A11Y_ROLE_TEXT_VIEW: return 60u;     /* TEXT            */
    case FDK_A11Y_ROLE_IMAGE: return 27u;         /* IMAGE           */
    case FDK_A11Y_ROLE_CALENDAR: return 5u;       /* CALENDAR        */
    case FDK_A11Y_ROLE_DATE_EDITOR: return 12u;   /* DATE_EDITOR     */
    default: return 66u;                          /* UNKNOWN         */
    }
}

/* FDK state flags -> AtspiStateType bit positions (the 64-bit set). */
static void state_to_atspi(fdk_a11y_state_set states, uint32_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    struct { fdk_a11y_state_flag f; unsigned bit; } map[] = {
        {FDK_A11Y_FOCUSABLE, 10},  {FDK_A11Y_FOCUSED, 11},
        {FDK_A11Y_ENABLED, 7},     {FDK_A11Y_VISIBLE, 29},
        {FDK_A11Y_SHOWING, 24},    {FDK_A11Y_CHECKED, 4},
        {FDK_A11Y_PRESSED, 19},    {FDK_A11Y_SELECTED, 22},
        {FDK_A11Y_EXPANDED, 9},    {FDK_A11Y_EDITABLE, 6},
        {FDK_A11Y_MULTI_SELECTABLE, 17}, {FDK_A11Y_MODAL, 15},
        {FDK_A11Y_BUSY, 3},        {FDK_A11Y_REQUIRED, 32},
        {FDK_A11Y_INVALID, 34},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (states & map[i].f) {
            if (map[i].bit < 32) {
                out[0] |= 1u << map[i].bit;
            } else {
                out[1] |= 1u << (map[i].bit - 32);
            }
        }
    }
    /* Every object is SENSITIVE(23)/OPAQUE-ish when enabled — the
     * flags screen readers treat as "interactable at all". */
    if (states & FDK_A11Y_ENABLED) {
        out[0] |= 1u << 23;
    }
}

/* ---- serving ------------------------------------------------------------ */

/* Resolves a message path to a registry index; -1 when unknown. */
static int path_index(const char *path) {
    if (path == NULL) {
        return -1;
    }
    if (strcmp(path, ATSPI_PREFIX "root") == 0) {
        return 0;
    }
    if (strncmp(path, ATSPI_PREFIX, strlen(ATSPI_PREFIX)) != 0) {
        return -1;
    }
    const char *num = path + strlen(ATSPI_PREFIX);
    char *end = NULL;
    long v = strtol(num, &end, 10);
    if (end == NULL || *end != '\0' || v <= 0 || v >= ATSPI_MAX_OBJECTS) {
        return -1;
    }
    return (int)v;
}

/* The widget for a slot, with the virtual describe for rows. */
static bool slot_widget(int index, fdk_widget **out_w) {
    if (index < 0 || index >= ATSPI_MAX_OBJECTS || !g_atspi.objs[index].used) {
        return false;
    }
    if (index == 0) {
        return false; /* the app root is not a widget */
    }
    if (!widget_reachable(g_atspi.objs[index].w)) {
        g_atspi.objs[index].used = 0; /* lazy GC on access */
        return false;
    }
    *out_w = g_atspi.objs[index].w;
    return true;
}

/* Registers a widget (idempotent) and returns its index. */
static int ensure_widget_object(fdk_widget *w) {
    int i = obj_find_widget(w);
    if (i >= 0) {
        return i;
    }
    return obj_register(ATK_OBJ_WIDGET, w, 0);
}

/* The children of a slot, as registry indexes (registering on
 * demand): real children first, then the virtual rows. Returns the
 * count (and fills ids[] when non-NULL, up to cap). */
static int slot_children(int index, int *ids, size_t cap) {
    if (index == 0) {
        /* the application root: the live window roots */
        int n = 0;
        for (fdk_widget *root = fdk__widget_roots_head(); root != NULL;
             root = root->root_next) {
            int id = ensure_widget_object(root);
            if (id > 0) {
                if (ids != NULL && (size_t)n < cap) {
                    ids[n] = id;
                }
                n++;
            }
        }
        return n;
    }
    fdk_widget *w = NULL;
    if (!slot_widget(index, &w)) {
        return 0;
    }
    int n = 0;
    for (size_t c = 0; c < w->child_count; c++) {
        int id = ensure_widget_object(w->children[c]);
        if (id > 0) {
            if (ids != NULL && (size_t)n < cap) {
                ids[n] = id;
            }
            n++;
        }
    }
    /* virtual rows (painted-row containers) */
    size_t vc = fdk_a11y_virtual_count(w);
    for (size_t v = 0; v < vc; v++) {
        int id = obj_find_virtual(w, (int)v);
        if (id < 0) {
            id = obj_register(ATK_OBJ_VIRTUAL, w, (int)v);
        }
        if (id > 0) {
            if (ids != NULL && (size_t)n < cap) {
                ids[n] = id;
            }
            n++;
        }
    }
    return n;
}

/* The parent of a slot as an index (-1 = the bus root / unknown). */
static int slot_parent(int index) {
    if (index == 0) {
        return -1; /* the app root has no parent */
    }
    fdk_widget *w = NULL;
    if (!slot_widget(index, &w)) {
        return -1;
    }
    if (g_atspi.objs[index].kind == ATK_OBJ_VIRTUAL) {
        return ensure_widget_object(w); /* the container */
    }
    if (w->parent == NULL) {
        return 0; /* a root widget: the application */
    }
    return ensure_widget_object(w->parent);
}

/* Builds the reply for a path-string reply. */
static void reply_object_path(fdk_dbus_connection *c, fdk_dbus_message *call,
                              int index) {
    fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
    if (reply == NULL) {
        return;
    }
    char path[96];
    if (index >= 0) {
        obj_path(path, sizeof path, index);
    } else {
        snprintf(path, sizeof path, ATSPI_PREFIX "root");
    }
    (void)fdk_dbus_message_append_object_path(reply, path);
    (void)fdk_dbus_connection_send(c, reply);
}

static void reply_error(fdk_dbus_connection *c, fdk_dbus_message *call,
                        const char *name) {
    fdk_dbus_message *err =
        fdk_dbus_message_new_error(call, name, NULL);
    if (err != NULL) {
        (void)fdk_dbus_connection_send(c, err);
    }
}

/* ---- the Accessible interface ------------------------------------------ */

/* C-2: defined after the Accessible/Component sections (they share
 * the helpers below); declared here because serve_accessible routes
 * GetRelationSet and the dispatcher routes IFACE_TEXT. */
static void serve_text(fdk_dbus_connection *c, fdk_dbus_message *call,
                       int index, const char *member);
static void serve_relation_set(fdk_dbus_connection *c,
                               fdk_dbus_message *call, int index);

static void serve_accessible(fdk_dbus_connection *c, fdk_dbus_message *call,
                             int index, const char *member) {
    /* The app root: role/name/state with no widget behind it. */
    if (index == 0) {
        if (strcmp(member, "GetRole") == 0) {
            fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
            if (r != NULL) {
                (void)fdk_dbus_message_append_uint32(r, 74u); /* APP */
                (void)fdk_dbus_connection_send(c, r);
            }
        } else if (strcmp(member, "GetRoleName") == 0 ||
                   strcmp(member, "GetLocalizedRoleName") == 0) {
            fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
            if (r != NULL) {
                (void)fdk_dbus_message_append_string(r, "application");
                (void)fdk_dbus_connection_send(c, r);
            }
        } else if (strcmp(member, "GetState") == 0) {
            fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
            if (r != NULL) {
                fdk_dbus_value *words[2] = {
                    fdk_dbus_value_new_uint32(0),
                    fdk_dbus_value_new_uint32(0),
                };
                fdk_dbus_value *arr =
                    fdk_dbus_value_new_array("u", words, 2);
                if (arr != NULL) {
                    (void)fdk_dbus_message_append(r, arr);
                    fdk_dbus_value_free(arr);
                }
                (void)fdk_dbus_connection_send(c, r);
            }
        } else if (strcmp(member, "GetChildCount") == 0) {
            fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
            if (r != NULL) {
                (void)fdk_dbus_message_append_int32(
                    r, (int32_t)slot_children(0, NULL, 0));
                (void)fdk_dbus_connection_send(c, r);
            }
        } else if (strcmp(member, "GetChildren") == 0) {
            int ids[128];
            int n = slot_children(0, ids, 128);
            fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
            if (r != NULL) {
                fdk_dbus_value *items[128];
                size_t m = 0;
                for (int i = 0; i < n && i < 128; i++) {
                    char path[96];
                    obj_path(path, sizeof path, ids[i]);
                    items[m++] = fdk_dbus_value_new_object_path(path);
                }
                fdk_dbus_value *arr =
                    fdk_dbus_value_new_array("o", items, m);
                if (arr != NULL) {
                    (void)fdk_dbus_message_append(r, arr);
                    fdk_dbus_value_free(arr);
                }
                (void)fdk_dbus_connection_send(c, r);
            }
        } else if (strcmp(member, "GetApplication") == 0 ||
                   strcmp(member, "GetIndexInParent") == 0) {
            if (strcmp(member, "GetApplication") == 0) {
                reply_object_path(c, call, 0);
            } else {
                fdk_dbus_message *r =
                    fdk_dbus_message_new_method_return(call);
                if (r != NULL) {
                    (void)fdk_dbus_message_append_int32(r, 0);
                    (void)fdk_dbus_connection_send(c, r);
                }
            }
        } else if (strcmp(member, "GetInterfaces") == 0) {
            fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
            if (r != NULL) {
                fdk_dbus_value *items[1] = {
                    fdk_dbus_value_new_string(IFACE_ACCESSIBLE),
                };
                fdk_dbus_value *arr =
                    fdk_dbus_value_new_array("s", items, 1);
                if (arr != NULL) {
                    (void)fdk_dbus_message_append(r, arr);
                    fdk_dbus_value_free(arr);
                }
                (void)fdk_dbus_connection_send(c, r);
            }
        } else if (strcmp(member, "GetAttributes") == 0) {
            fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
            if (r != NULL) {
                fdk_dbus_value *arr =
                    fdk_dbus_value_new_array("{ss}", NULL, 0);
                if (arr != NULL) {
                    (void)fdk_dbus_message_append(r, arr);
                    fdk_dbus_value_free(arr);
                }
                (void)fdk_dbus_connection_send(c, r);
            }
        }
        return;
    }

    fdk_widget *w = NULL;
    if (!slot_widget(index, &w)) {
        reply_error(c, call,
                    "org.freedesktop.atspi.Error.Defunct");
        return;
    }

    if (strcmp(member, "GetRole") == 0) {
        fdk_a11y_info info;
        fdk_a11y_describe(w, &info);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_uint32(
                r, role_to_atspi(info.role));
            (void)fdk_dbus_connection_send(c, r);
        }
        fdk_a11y_info_free(&info);
    } else if (strcmp(member, "GetRoleName") == 0 ||
               strcmp(member, "GetLocalizedRoleName") == 0) {
        fdk_a11y_info info;
        fdk_a11y_describe(w, &info);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_string(
                r, fdk_a11y_role_name(info.role));
            (void)fdk_dbus_connection_send(c, r);
        }
        fdk_a11y_info_free(&info);
    } else if (strcmp(member, "GetState") == 0) {
        fdk_a11y_info info;
        fdk_a11y_describe(w, &info);
        uint32_t words[2];
        state_to_atspi(info.states, words);
        fdk_a11y_info_free(&info);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            fdk_dbus_value *vals[2] = {
                fdk_dbus_value_new_uint32(words[0]),
                fdk_dbus_value_new_uint32(words[1]),
            };
            fdk_dbus_value *arr =
                fdk_dbus_value_new_array("u", vals, 2);
            if (arr != NULL) {
                (void)fdk_dbus_message_append(r, arr);
                fdk_dbus_value_free(arr);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetChildCount") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_int32(
                r, (int32_t)slot_children(index, NULL, 0));
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetChildren") == 0) {
        int ids[128];
        int n = slot_children(index, ids, 128);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            fdk_dbus_value *items[128];
            size_t m = 0;
            for (int i = 0; i < n && i < 128; i++) {
                char path[96];
                obj_path(path, sizeof path, ids[i]);
                items[m++] = fdk_dbus_value_new_object_path(path);
            }
            fdk_dbus_value *arr = fdk_dbus_value_new_array("o", items, m);
            if (arr != NULL) {
                (void)fdk_dbus_message_append(r, arr);
                fdk_dbus_value_free(arr);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetChildAtIndex") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        int32_t want = -1;
        if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
            n >= 1 && args != NULL) {
            want = fdk_dbus_value_int32(args[0]);
        }
        fdk_dbus_values_free_array(args, n);
        int ids[128];
        int count = slot_children(index, ids, 128);
        if (want < 0 || want >= count || want >= 128) {
            reply_error(c, call, "org.freedesktop.atspi.Error.InvalidIndex");
            return;
        }
        reply_object_path(c, call, ids[want]);
    } else if (strcmp(member, "GetIndexInParent") == 0) {
        int parent = slot_parent(index);
        int ids[128];
        int count = (parent >= 0) ? slot_children(parent, ids, 128) : 0;
        int32_t pos = 0;
        for (int i = 0; i < count && i < 128; i++) {
            if (ids[i] == index) {
                pos = (int32_t)i;
                break;
            }
        }
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_int32(r, pos);
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetApplication") == 0) {
        reply_object_path(c, call, 0);
    } else if (strcmp(member, "GetInterfaces") == 0) {
        fdk_a11y_action_set acts = fdk_a11y_actions_of(w);
        bool has_text = fdk_a11y_has_text_interface(w);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            fdk_dbus_value *items[4];
            size_t m = 0;
            items[m++] = fdk_dbus_value_new_string(IFACE_ACCESSIBLE);
            items[m++] = fdk_dbus_value_new_string(IFACE_COMPONENT);
            if (acts & FDK_A11Y_ACTION_ACTIVATE) {
                items[m++] = fdk_dbus_value_new_string(IFACE_ACTION);
            }
            if (has_text) {
                /* C-2: the Text interface rides text-bearing widgets
                 * (Entry, TextView, Label, Spin) — the screen reader
                 * enumerates it before issuing Text calls. */
                items[m++] = fdk_dbus_value_new_string(IFACE_TEXT);
            }
            fdk_dbus_value *arr = fdk_dbus_value_new_array("s", items, m);
            if (arr != NULL) {
                (void)fdk_dbus_message_append(r, arr);
                fdk_dbus_value_free(arr);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetRelationSet") == 0) {
        serve_relation_set(c, call, index);
    } else if (strcmp(member, "GetAttributes") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            fdk_dbus_value *arr = fdk_dbus_value_new_array("{ss}", NULL, 0);
            if (arr != NULL) {
                (void)fdk_dbus_message_append(r, arr);
                fdk_dbus_value_free(arr);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    }
}

/* ---- the Component interface ------------------------------------------- */

static void serve_component(fdk_dbus_connection *c, fdk_dbus_message *call,
                            int index, const char *member) {
    fdk_widget *w = NULL;
    if (index == 0 || !slot_widget(index, &w)) {
        reply_error(c, call, "org.freedesktop.atspi.Error.Defunct");
        return;
    }
    fdk_a11y_info info;
    if (g_atspi.objs[index].kind == ATK_OBJ_VIRTUAL) {
        fdk_a11y_virtual_describe(w, (size_t)g_atspi.objs[index].vindex,
                                  &info);
    } else {
        fdk_a11y_describe(w, &info);
    }

    /* Desktop coordinates: the window's own origin (root position
     * where the backend can tell, else zero — the documented
     * get_position posture) + the snapshot's window-absolute bounds. */
    int win_x = 0, win_y = 0;
    fdk_window *owner = fdk__window_of_owner(fdk__widget_window_owner(w));
    if (owner != NULL && owner->ops != NULL &&
        owner->ops->window_get_position != NULL) {
        fdk_i32 ox = 0, oy = 0;
        if (fdk_ok(owner->ops->window_get_position(owner->pwindow, &ox,
                                                   &oy))) {
            win_x = (int)ox;
            win_y = (int)oy;
        }
    }
    int x = win_x + (int)info.bounds.x;
    int y = win_y + (int)info.bounds.y;

    fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
    if (r == NULL) {
        fdk_a11y_info_free(&info);
        return;
    }
    if (strcmp(member, "GetExtents") == 0) {
        fdk_dbus_value *items[4] = {
            fdk_dbus_value_new_int32((int32_t)x),
            fdk_dbus_value_new_int32((int32_t)y),
            fdk_dbus_value_new_int32((int32_t)info.bounds.width),
            fdk_dbus_value_new_int32((int32_t)info.bounds.height),
        };
        fdk_dbus_value *st = fdk_dbus_value_new_struct(items, 4);
        if (st != NULL) {
            (void)fdk_dbus_message_append(r, st);
            fdk_dbus_value_free(st);
        }
    } else if (strcmp(member, "GetPosition") == 0) {
        (void)fdk_dbus_message_append_int32(r, (int32_t)x);
        (void)fdk_dbus_message_append_int32(r, (int32_t)y);
    } else if (strcmp(member, "GetSize") == 0) {
        (void)fdk_dbus_message_append_int32(
            r, (int32_t)info.bounds.width);
        (void)fdk_dbus_message_append_int32(
            r, (int32_t)info.bounds.height);
    } else {
        fdk_a11y_info_free(&info);
        fdk_dbus_message_free(r);
        reply_error(c, call, "org.freedesktop.DBus.Error.UnknownMethod");
        return;
    }
    fdk_a11y_info_free(&info);
    (void)fdk_dbus_connection_send(c, r);
}

/* ---- the Action interface ---------------------------------------------- */

static void serve_action(fdk_dbus_connection *c, fdk_dbus_message *call,
                         int index, const char *member) {
    fdk_widget *w = NULL;
    if (index == 0 || !slot_widget(index, &w)) {
        reply_error(c, call, "org.freedesktop.atspi.Error.Defunct");
        return;
    }
    fdk_a11y_action_set acts =
        (g_atspi.objs[index].kind == ATK_OBJ_VIRTUAL)
            ? fdk_a11y_virtual_actions(
                  w, (size_t)g_atspi.objs[index].vindex)
            : fdk_a11y_actions_of(w);
    int can = (acts & FDK_A11Y_ACTION_ACTIVATE) != 0;

    if (strcmp(member, "GetNActions") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_int32(r, can ? 1 : 0);
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetName") == 0 ||
               strcmp(member, "GetDescription") == 0 ||
               strcmp(member, "GetKeyBinding") == 0 ||
               strcmp(member, "GetLocalizedName") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        (void)fdk_dbus_message_read_args(call, &args, &n);
        fdk_dbus_values_free_array(args, n);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            const char *v = "";
            if (strcmp(member, "GetName") == 0) {
                v = can ? "activate" : "";
            } else if (strcmp(member, "GetLocalizedName") == 0) {
                v = can ? "activate" : "";
            }
            (void)fdk_dbus_message_append_string(r, v);
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetActions") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            if (can) {
                fdk_dbus_value *items[3];
                items[0] = fdk_dbus_value_new_string("activate");
                items[1] = fdk_dbus_value_new_string("");
                items[2] = fdk_dbus_value_new_string("");
                fdk_dbus_value *st = fdk_dbus_value_new_struct(items, 3);
                fdk_dbus_value *arr =
                    st != NULL
                        ? fdk_dbus_value_new_array("(sss)",
                                                   (fdk_dbus_value *[]){st},
                                                   1)
                        : NULL;
                if (arr != NULL) {
                    (void)fdk_dbus_message_append(r, arr);
                    fdk_dbus_value_free(arr);
                }
            } else {
                fdk_dbus_value *arr =
                    fdk_dbus_value_new_array("(sss)", NULL, 0);
                if (arr != NULL) {
                    (void)fdk_dbus_message_append(r, arr);
                    fdk_dbus_value_free(arr);
                }
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "DoAction") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        int32_t want = 0;
        if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
            n >= 1 && args != NULL) {
            want = fdk_dbus_value_int32(args[0]);
        }
        fdk_dbus_values_free_array(args, n);
        bool ok = false;
        if (can && want == 0) {
            /* The screen reader's "click": the same programmatic path
             * a keyboard user's activation takes. */
            if (g_atspi.objs[index].kind == ATK_OBJ_VIRTUAL) {
                ok = fdk_ok(fdk_a11y_virtual_perform(
                    w, (size_t)g_atspi.objs[index].vindex,
                    FDK_A11Y_ACTION_ACTIVATE, 0.0));
            } else {
                ok = fdk_ok(fdk_a11y_perform(w, FDK_A11Y_ACTION_ACTIVATE,
                                             0.0));
            }
        }
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_boolean(r, ok ? 1 : 0);
            (void)fdk_dbus_connection_send(c, r);
        }
    }
}

/* ---- the properties (Name/Description/Parent) --------------------------- */

static void serve_properties(fdk_dbus_connection *c, fdk_dbus_message *call,
                             int index, const char *member) {
    if (strcmp(member, "Get") != 0 && strcmp(member, "GetAll") != 0) {
        reply_error(c, call, "org.freedesktop.DBus.Error.UnknownMethod");
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    const char *prop = NULL;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK && n >= 2 &&
        args != NULL) {
        prop = fdk_dbus_value_string(args[1]);
    }

    if (strcmp(member, "Get") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r == NULL) {
            fdk_dbus_values_free_array(args, n);
            return;
        }
        if (prop == NULL) {
            fdk_dbus_values_free_array(args, n);
            reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs");
            fdk_dbus_message_free(r);
            return;
        }
        if (strcmp(prop, "Name") == 0) {
            const char *v = "";
            char *owned = NULL;
            if (index == 0) {
                v = "fdk-application";
            } else {
                fdk_widget *w = NULL;
                if (slot_widget(index, &w)) {
                    fdk_a11y_info info;
                    if (g_atspi.objs[index].kind == ATK_OBJ_VIRTUAL) {
                        fdk_a11y_virtual_describe(
                            w, (size_t)g_atspi.objs[index].vindex, &info);
                    } else {
                        fdk_a11y_describe(w, &info);
                    }
                    if (info.name != NULL) {
                        owned = info.name; /* steal for the append */
                        info.name = NULL;
                        v = owned;
                    }
                    fdk_a11y_info_free(&info);
                }
            }
            (void)fdk_dbus_message_append_variant_string(r, v);
            fdk_free(owned);
        } else if (strcmp(prop, "Description") == 0) {
            const char *v = "";
            char *owned = NULL;
            if (index != 0) {
                fdk_widget *w = NULL;
                if (slot_widget(index, &w)) {
                    fdk_a11y_info info;
                    if (g_atspi.objs[index].kind == ATK_OBJ_VIRTUAL) {
                        fdk_a11y_virtual_describe(
                            w, (size_t)g_atspi.objs[index].vindex, &info);
                    } else {
                        fdk_a11y_describe(w, &info);
                    }
                    if (info.description != NULL) {
                        owned = info.description;
                        info.description = NULL;
                        v = owned;
                    }
                    fdk_a11y_info_free(&info);
                }
            }
            (void)fdk_dbus_message_append_variant_string(r, v);
            fdk_free(owned);
        } else if (strcmp(prop, "Parent") == 0) {
            char path[96];
            int parent = slot_parent(index);
            obj_path(path, sizeof path, parent >= 0 ? parent : 0);
            fdk_dbus_value *inner = fdk_dbus_value_new_object_path(path);
            fdk_dbus_value *var = inner != NULL
                                      ? fdk_dbus_value_new_variant(inner)
                                      : NULL;
            if (var != NULL) {
                (void)fdk_dbus_message_append(r, var);
                fdk_dbus_value_free(var);
            }
            if (inner != NULL) {
                fdk_dbus_value_free(inner);
            }
        } else if (strcmp(prop, "ChildCount") == 0) {
            fdk_dbus_value *inner = fdk_dbus_value_new_int32(
                (int32_t)slot_children(index, NULL, 0));
            if (inner != NULL) {
                fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
                fdk_dbus_value_free(inner);
                if (var != NULL) {
                    (void)fdk_dbus_message_append(r, var);
                    fdk_dbus_value_free(var);
                }
            }
        } else if (strcmp(prop, "Locale") == 0) {
            (void)fdk_dbus_message_append_variant_string(r, "C");
        } else {
            fdk_dbus_message_free(r);
            fdk_dbus_values_free_array(args, n);
            reply_error(c, call, "org.freedesktop.DBus.Error.InvalidArgs");
            return;
        }
        fdk_dbus_values_free_array(args, n);
        (void)fdk_dbus_connection_send(c, r);
        return;
    }

    /* GetAll: the a{sv} of everything we serve. */
    fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
    if (r == NULL) {
        fdk_dbus_values_free_array(args, n);
        return;
    }
    fdk_dbus_value *entries[5];
    size_t m = 0;
    /* Name */
    {
        const char *v = "";
        char *owned = NULL;
        if (index == 0) {
            v = "fdk-application";
        } else {
            fdk_widget *w = NULL;
            if (slot_widget(index, &w)) {
                fdk_a11y_info info;
                if (g_atspi.objs[index].kind == ATK_OBJ_VIRTUAL) {
                    fdk_a11y_virtual_describe(
                        w, (size_t)g_atspi.objs[index].vindex, &info);
                } else {
                    fdk_a11y_describe(w, &info);
                }
                if (info.name != NULL) {
                    owned = info.name;
                    info.name = NULL;
                    v = owned;
                }
                fdk_a11y_info_free(&info);
            }
        }
        fdk_dbus_value *inner = fdk_dbus_value_new_string(v);
        fdk_dbus_value *var =
            inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
        if (inner != NULL) {
            fdk_dbus_value_free(inner);
        }
        if (var != NULL) {
            fdk_dbus_value *k = fdk_dbus_value_new_string("Name");
            entries[m++] = fdk_dbus_value_new_dict_entry(k, var);
        }
        fdk_free(owned);
    }
    /* Description (empty for the app root) */
    {
        fdk_dbus_value *inner = fdk_dbus_value_new_string("");
        fdk_dbus_value *var =
            inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
        if (inner != NULL) {
            fdk_dbus_value_free(inner);
        }
        if (var != NULL) {
            fdk_dbus_value *k = fdk_dbus_value_new_string("Description");
            entries[m++] = fdk_dbus_value_new_dict_entry(k, var);
        }
    }
    /* Parent */
    {
        char path[96];
        int parent = slot_parent(index);
        obj_path(path, sizeof path, parent >= 0 ? parent : 0);
        fdk_dbus_value *inner = fdk_dbus_value_new_object_path(path);
        fdk_dbus_value *var =
            inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
        if (inner != NULL) {
            fdk_dbus_value_free(inner);
        }
        if (var != NULL) {
            fdk_dbus_value *k = fdk_dbus_value_new_string("Parent");
            entries[m++] = fdk_dbus_value_new_dict_entry(k, var);
        }
    }
    /* ChildCount */
    {
        fdk_dbus_value *inner =
            fdk_dbus_value_new_int32((int32_t)slot_children(index, NULL, 0));
        fdk_dbus_value *var =
            inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
        if (inner != NULL) {
            fdk_dbus_value_free(inner);
        }
        if (var != NULL) {
            fdk_dbus_value *k = fdk_dbus_value_new_string("ChildCount");
            entries[m++] = fdk_dbus_value_new_dict_entry(k, var);
        }
    }
    fdk_dbus_value *arr = fdk_dbus_value_new_array("{sv}", entries, m);
    if (arr != NULL) {
        (void)fdk_dbus_message_append(r, arr);
        fdk_dbus_value_free(arr);
    }
    fdk_dbus_values_free_array(args, n);
    (void)fdk_dbus_connection_send(c, r);
}

/* ---- the method dispatcher ---------------------------------------------- */

static void atspi_method(fdk_dbus_connection *c, fdk_dbus_message *call,
                         void *user) {
    (void)user;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    const char *path = fdk_dbus_message_path(call);
    if (iface == NULL || member == NULL || path == NULL) {
        return;
    }
    int index = path_index(path);
    if (index < 0) {
        reply_error(c, call, "org.freedesktop.DBus.Error.UnknownObject");
        return;
    }
    if (strcmp(iface, IFACE_ACCESSIBLE) == 0) {
        serve_accessible(c, call, index, member);
    } else if (strcmp(iface, IFACE_COMPONENT) == 0) {
        serve_component(c, call, index, member);
    } else if (strcmp(iface, IFACE_ACTION) == 0) {
        serve_action(c, call, index, member);
    } else if (strcmp(iface, IFACE_TEXT) == 0) {
        serve_text(c, call, index, member);
    } else if (strcmp(iface, IFACE_DBUS_PROPS) == 0) {
        serve_properties(c, call, index, member);
    }
    /* Unknown interface: the client's own default error answer (the
     * documented no-handler posture) never applies — a handler IS
     * installed — so answer UnknownMethod for honest callers. */
    else {
        reply_error(c, call, "org.freedesktop.DBus.Error.UnknownMethod");
    }
}

/* ---- events -------------------------------------------------------------- */

/* Emits one Event.Object signal: (s detail, i detail1, i detail2,
 * v any) on the object's own path. */
static void emit_event(int index, const char *member, const char *detail,
                       int32_t d1, int32_t d2,
                       const fdk_dbus_value *any) {
    if (g_atspi.bus == NULL || index < 0) {
        return;
    }
    char path[96];
    obj_path(path, sizeof path, index);
    fdk_dbus_message *sig =
        fdk_dbus_message_new_signal(path, IFACE_EVENT_OBJECT, member);
    if (sig == NULL) {
        return;
    }
    (void)fdk_dbus_message_append_string(sig, detail);
    (void)fdk_dbus_message_append_int32(sig, d1);
    (void)fdk_dbus_message_append_int32(sig, d2);
    if (any != NULL) {
        (void)fdk_dbus_message_append(sig, any);
    } else {
        fdk_dbus_value *inner = fdk_dbus_value_new_int32(0);
        if (inner != NULL) {
            fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
            fdk_dbus_value_free(inner);
            if (var != NULL) {
                (void)fdk_dbus_message_append(sig, var);
                fdk_dbus_value_free(var);
            }
        }
    }
    (void)fdk_dbus_connection_send(g_atspi.bus, sig);
}

/* The internal a11y notification -> the AT-SPI events (the bridge
 * pattern the subscription documents). */
static void atspi_a11y_notify(const fdk_a11y_event *ev, void *user) {
    (void)user;
    if (g_atspi.bus == NULL) {
        return;
    }
    int index = -1;
    if (ev->widget != NULL) {
        index = obj_find_widget(ev->widget);
    }
    switch (ev->kind) {
    case FDK_A11Y_STATE_CHANGED:
        if (index < 0) {
            break;
        }
        if (ev->state_flag == FDK_A11Y_FOCUSED) {
            /* the focus announcement: the state change AND the
             * screen-reader cue */
            fdk_widget *w = NULL;
            bool focused = false;
            if (slot_widget(index, &w)) {
                fdk_a11y_info info;
                fdk_a11y_describe(w, &info);
                focused = (info.states & FDK_A11Y_FOCUSED) != 0;
                fdk_a11y_info_free(&info);
            }
            emit_event(index, "StateChanged", "focused",
                        focused ? 1 : 0, 0, NULL);
            if (focused) {
                emit_event(index, "Focus", "focus", 0, 0, NULL);
            }
        } else {
            /* the states that map: announce by the FDK flag's own
             * name (the honest level-1 posture: name parity with the
             * internal tree) */
            const char *names[] = {
                NULL, NULL, "enabled", "visible", "showing",
                "checked", "pressed", "selected", "expanded",
                "editable", "read-only", "multiselectable", NULL,
                "modal", "busy", "required", "invalid",
            };
            if (ev->state_flag > 0 &&
                (size_t)ev->state_flag <=
                    sizeof names / sizeof names[0]) {
                const char *nm = names[ev->state_flag - 1];
                if (nm != NULL) {
                    emit_event(index, "StateChanged", nm, 1, 0, NULL);
                }
            }
        }
        break;
    case FDK_A11Y_NAME_CHANGED:
        if (index >= 0) {
            emit_event(index, "PropertyChange", "accessible-name", 0, 0,
                       NULL);
        }
        break;
    case FDK_A11Y_DESCRIPTION_CHANGED:
        if (index >= 0) {
            emit_event(index, "PropertyChange", "accessible-description",
                       0, 0, NULL);
        }
        break;
    case FDK_A11Y_CHILDREN_CHANGED: {
        /* the subject is the PARENT: re-diff its children (kills gone
         * entries) and announce the add/remove as best the event
         * allows (the count's new value). */
        registry_gc();
        if (ev->widget != NULL) {
            int pindex = obj_find_widget(ev->widget);
            if (pindex >= 0) {
                int n = slot_children(pindex, NULL, 0);
                emit_event(pindex, "ChildrenChanged", "add", 0, 0, NULL);
                (void)n;
            }
        }
        break;
    }
    case FDK_A11Y_CARET_MOVED:
        /* C-2: the text signals — the caret moved (and the selection
         * with it, whatever changed). */
        if (index >= 0 && fdk_a11y_has_text_interface(ev->widget)) {
            emit_event(index, "TextCaretMoved", "", 0, 0, NULL);
            emit_event(index, "TextSelectionChanged", "", 0, 0, NULL);
        }
        break;
    case FDK_A11Y_VALUE_CHANGED:
        /* Text edits arrive as VALUE_CHANGED on text-interface
         * widgets (the editors' own chokepoint): the TextChanged
         * signal screen readers announce reading lines by. Detail
         * precision (insert vs delete + position) is a level-3
         * fence; the generic change is the honest level-2 answer. */
        if (index >= 0 && ev->widget != NULL &&
            fdk_a11y_has_text_interface(ev->widget)) {
            emit_event(index, "TextChanged", "change", 0, 0, NULL);
        }
        break;
    default:
        break; /* bounds/relations: no AT-SPI signal maps (the
                * PropertyChange for bounds is level-3 polish) */
    }
}

/* ---- the bus handshake --------------------------------------------------- */

static void atspi_bus_ready(fdk_dbus_connection *c, int ok, const char *err,
                            void *user);

static void session_getaddress_reply(fdk_dbus_connection *c,
                                     fdk_dbus_message *reply, void *user) {
    (void)c;
    (void)user;
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        FDK_INFO("atspi: no a11y bus address yet (no screen reader "
                 "stack running); retrying on the %d ms throttle (the "
                 "narrator still serves the internal tree)",
                 ATSPI_RETRY_MS);
        g_atspi.engaged = 0; /* the retry posture */
        if (g_atspi.session != NULL) {
            fdk_dbus_connection_close(g_atspi.session);
            g_atspi.session = NULL;
        }
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    const char *address = NULL;
    if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK && n >= 1 &&
        args != NULL) {
        address = fdk_dbus_value_string(args[0]);
    }
    if (address == NULL || address[0] == '\0') {
        fdk_dbus_values_free_array(args, n);
        g_atspi.engaged = 0; /* retry */
        return;
    }
    char *copy = fdk_alloc(strlen(address) + 1);
    if (copy == NULL) {
        fdk_dbus_values_free_array(args, n);
        g_atspi.engaged = 0; /* retry */
        return;
    }
    strcpy(copy, address);
    fdk_dbus_values_free_array(args, n);

    fdk_dbus_connection *bus = fdk_dbus_connect_address(
        g_atspi.ctx, copy, atspi_bus_ready, NULL);
    fdk_free(copy);
    if (bus == NULL) {
        FDK_INFO("atspi: the a11y bus address refused the connection; "
                 "retrying (the narrator still works)");
        g_atspi.engaged = 0; /* retry */
    }
}

static void atspi_bus_ready(fdk_dbus_connection *c, int ok, const char *err,
                            void *user) {
    (void)user;
    if (!ok) {
        FDK_INFO("atspi: the a11y bus connect failed (%s); retrying",
                 err ? err : "?");
        g_atspi.engaged = 0; /* retry */
        return;
    }
    g_atspi.bus = c;
    fdk_dbus_connection_set_method_handler(c, atspi_method, NULL);

    /* The internal tree -> the wire: subscribe globally (the bridge
     * pattern) and seed the registry with the live roots. */
    if (!g_atspi.sub_active) {
        if (fdk_ok(fdk_a11y_subscribe(NULL, atspi_a11y_notify, NULL))) {
            g_atspi.sub_active = 1;
        }
    }
    memset(g_atspi.objs, 0, sizeof g_atspi.objs);
    g_atspi.objs[0].used = 1;
    g_atspi.objs[0].kind = ATK_OBJ_APP;
    g_atspi.objs[0].w = NULL;
    for (fdk_widget *root = fdk__widget_roots_head(); root != NULL;
         root = root->root_next) {
        (void)ensure_widget_object(root);
    }
    /* The DISCOVERY announcement: at-spi2's registry learns apps from
     * the signals they emit on the a11y bus (the sender's unique name
     * + the root path is the probe address) — the atk-bridge emits
     * Window events for exactly this reason. ChildrenChanged on the
     * root says "the application object has children"; a registry (or
     * the rig's mock) re-reads GetChildren and the tree is found. */
    emit_event(0, "ChildrenChanged", "add", 0, 0, NULL);
    FDK_INFO("atspi: the accessibility bridge is live (level 1: roles, "
             "names, states, focus, activate)");
}

static void session_ready(fdk_dbus_connection *c, int ok, const char *err,
                          void *user) {
    (void)user;
    if (!ok) {
        FDK_INFO("atspi: session bus handshake failed (%s); retrying",
                 err ? err : "?");
        g_atspi.engaged = 0; /* retry */
        return;
    }
    g_atspi.session = c;
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        A11Y_BUS_NAME, A11Y_BUS_PATH, A11Y_BUS_IFACE, "GetAddress");
    if (m == NULL ||
        fdk_dbus_connection_call(c, m, FDK_DBUS_TIMEOUT_DEFAULT,
                                 session_getaddress_reply, NULL) == NULL) {
        fdk_dbus_message_free(m);
        g_atspi.engaged = 0; /* retry */
    }
}

/* ---- the tick / teardown ------------------------------------------------- */

void fdk__atspi_tick(fdk_context *ctx) {
    if (g_atspi.engaged || g_atspi.degraded) {
        return;
    }
    const char *v = getenv("FDK_ATSPI");
    if (v != NULL && (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
                      strcasecmp(v, "no") == 0)) {
        g_atspi.degraded = 1; /* explicitly off */
        return;
    }
    if (ctx == NULL || ctx->window_count == 0) {
        return; /* nothing to expose yet; retry next tick */
    }
    /* The throttle (monotonic ms; CLOCK_REALTIME would jump under
     * NTP and either stall or storm the retry). */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u +
                   (uint64_t)ts.tv_nsec / 1000000u;
    if (g_atspi.last_try_ms != 0 &&
        now - g_atspi.last_try_ms < (uint64_t)ATSPI_RETRY_MS) {
        return;
    }
    g_atspi.last_try_ms = now;
    g_atspi.engaged = 1;
    g_atspi.ctx = ctx;
    g_atspi.objs[0].used = 1;
    g_atspi.objs[0].kind = ATK_OBJ_APP;

    fdk_dbus_connection *session = fdk_dbus_connect(
        ctx, FDK_DBUS_SESSION_BUS, session_ready, NULL);
    if (session == NULL) {
        /* No session bus RIGHT NOW — the retry posture (the session
         * may come up around us; the desktop may be starting). */
        g_atspi.engaged = 0;
    }
}

void fdk__atspi_shutdown(void) {
    if (g_atspi.sub_active) {
        (void)fdk_a11y_unsubscribe(NULL, atspi_a11y_notify, NULL);
        g_atspi.sub_active = 0;
    }
    if (g_atspi.bus != NULL) {
        fdk_dbus_connection_close(g_atspi.bus);
        g_atspi.bus = NULL;
    }
    if (g_atspi.session != NULL) {
        fdk_dbus_connection_close(g_atspi.session);
        g_atspi.session = NULL;
    }
    memset(&g_atspi, 0, sizeof g_atspi);
}

/* ---- the Text interface (C-2, level 2) --------------------------------
 *
 * AT-SPI text offsets are CODEPOINT indices (the interface's own
 * doc: "character offset... may differ from the number of bytes");
 * FDK's text interface is byte-based. Every method converts through
 * the whole-text copy (fdk__widget_text_content), and every
 * returned range converts back — the two walks below are the only
 * places that arithmetic exists.
 *
 * Boundary types: AT-SPI2's 0..6 (CHAR, WORD_START, WORD_END,
 * SENTENCE_START, SENTENCE_END, LINE_START, LINE_END) map onto the
 * internal CHAR/WORD/LINE granularities; the START/END distinction
 * and SENTENCE both degrade (the internal granularity is the whole
 * word; sentence segmentation is not a toolkit job at this level —
 * both documented in D-027). */

/* Walks `cp_index` codepoints; returns the byte offset (clamped to
 * len; negative indices clamp to 0). */
static size_t atspi_cp_to_byte(const char *s, size_t len,
                               int32_t cp_index) {
    if (cp_index <= 0) {
        return 0;
    }
    size_t at = 0;
    int32_t n = 0;
    while (at < len && n < cp_index) {
        fdk_u32 cp = 0;
        int step = fdk_text_utf8_next(s, len, at, &cp);
        if (step <= 0) {
            break;
        }
        at += (size_t)step;
        n++;
    }
    return at;
}

/* Counts the codepoints before byte `at` (clamped). */
static int32_t atspi_byte_to_cp(const char *s, size_t len, size_t at) {
    if (at > len) {
        at = len;
    }
    int32_t n = 0;
    size_t i = 0;
    while (i < at) {
        fdk_u32 cp = 0;
        int step = fdk_text_utf8_next(s, len, i, &cp);
        if (step <= 0) {
            break;
        }
        i += (size_t)step;
        n++;
    }
    return n;
}

/* The AT-SPI boundary type -> the internal granularity. */
static fdk_a11y_text_granularity atspi_boundary_map(uint32_t type) {
    switch (type) {
    case 1: /* WORD_START */
    case 2: /* WORD_END */
        return FDK_A11Y_TEXT_WORD;
    case 3: /* SENTENCE_START */
    case 4: /* SENTENCE_END */
        return FDK_A11Y_TEXT_LINE; /* the documented degrade */
    case 5: /* LINE_START */
    case 6: /* LINE_END */
        return FDK_A11Y_TEXT_LINE;
    case 0: /* CHAR */
    default:
        return FDK_A11Y_TEXT_CHAR;
    }
}

/* One run, fetched through the internal interface with byte bounds;
 * writes the run text (NUL-terminated, truncated to cap-1) and its
 * FULL byte range. Returns false when the class has no text. */
static bool atspi_run_at(const fdk_widget *w, size_t byte_offset,
                         fdk_a11y_text_granularity g, char *buf,
                         size_t cap, size_t *start, size_t *end) {
    return fdk_a11y_text_at_offset(w, byte_offset, g, buf, cap, start,
                                   end) == FDK_OK;
}

/* Whitespace-only run? (Empty runs count: they are boundaries to
 * hop across, not answers.) */
static bool atspi_run_is_ws(const char *text, size_t start, size_t end) {
    if (start >= end) {
        return true;
    }
    for (size_t i = start; i < end; i++) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\n' &&
            text[i] != '\r') {
            return false;
        }
    }
    return true;
}

/* Hops from the run [start, end) toward dir (-1: earlier, +1:
 * later) until a non-whitespace run is found (WORD granularity only;
 * 4 hops cap). Writes the found run; false when the walk runs out. */
static bool atspi_hop_run(const fdk_widget *w, const char *text,
                          size_t len, size_t start, size_t end,
                          fdk_a11y_text_granularity gran, int dir,
                          char *buf, size_t cap, size_t *out_start,
                          size_t *out_end) {
    if (gran != FDK_A11Y_TEXT_WORD) {
        /* CHAR/LINE runs answer as-is — a space IS the character at
         * a space, and lines include their spaces. */
        *out_start = start;
        *out_end = end;
        return true;
    }
    for (int hop = 0; hop < 4; hop++) {
        size_t probe;
        if (dir < 0) {
            if (start == 0) {
                return false;
            }
            probe = start - 1;
        } else {
            if (end >= len) {
                return false;
            }
            probe = end;
        }
        size_t s2 = 0, e2 = 0;
        if (!atspi_run_at(w, probe, gran, buf, cap, &s2, &e2)) {
            return false;
        }
        if (!atspi_run_is_ws(text, s2, e2)) {
            *out_start = s2;
            *out_end = e2;
            return true;
        }
        start = s2;
        end = e2;
    }
    return false;
}

/* The meaningful run containing (or adjacent to) `byte`: the run
 * itself when it carries ink; for a whitespace run at WORD
 * granularity, the word before it (then the word after — the
 * left-to-right reading order answer). */
static bool atspi_meaningful_run(const fdk_widget *w, const char *text,
                                 size_t len, size_t byte,
                                 fdk_a11y_text_granularity gran,
                                 char *buf, size_t cap, size_t *out_start,
                                 size_t *out_end) {
    size_t start = 0, end = 0;
    if (!atspi_run_at(w, byte, gran, buf, cap, &start, &end)) {
        return false;
    }
    if (!atspi_run_is_ws(text, start, end)) {
        *out_start = start;
        *out_end = end;
        return true;
    }
    if (atspi_hop_run(w, text, len, start, end, gran, -1, buf, cap,
                      out_start, out_end)) {
        return true;
    }
    return atspi_hop_run(w, text, len, start, end, gran, +1, buf, cap,
                         out_start, out_end);
}

/* Replies (s, start_cp, end_cp) for a GetText{At,After,Before}Offset
 * call — the byte range converts to codepoint indices. */
static void reply_text_run(fdk_dbus_connection *c, fdk_dbus_message *call,
                           const char *text, size_t len, size_t start,
                           size_t end) {
    fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
    if (r == NULL) {
        return;
    }
    /* Clamp the run into the text and copy it out. */
    if (start > len) {
        start = len;
    }
    if (end > len || end < start) {
        end = start;
    }
    size_t n = end - start;
    char *copy = fdk_alloc(n + 1u);
    if (copy == NULL) {
        fdk_dbus_message_free(r);
        reply_error(c, call, "org.freedesktop.DBus.Error.NoMemory");
        return;
    }
    memcpy(copy, text + start, n);
    copy[n] = '\0';
    (void)fdk_dbus_message_append_string(r, copy);
    fdk_free(copy);
    (void)fdk_dbus_message_append_int32(
        r, atspi_byte_to_cp(text, len, start));
    (void)fdk_dbus_message_append_int32(
        r, atspi_byte_to_cp(text, len, end));
    (void)fdk_dbus_connection_send(c, r);
}

/* The (whole-text, codepoint-index) variant the direct GetText uses. */
static void reply_text_slice(fdk_dbus_connection *c,
                             fdk_dbus_message *call, const char *text,
                             size_t len, int32_t cp_start,
                             int32_t cp_end) {
    size_t start = atspi_cp_to_byte(text, len, cp_start);
    size_t end = atspi_cp_to_byte(text, len, cp_end);
    fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
    if (r == NULL) {
        return;
    }
    if (end < start) {
        end = start;
    }
    size_t n = end - start;
    char *copy = fdk_alloc(n + 1u);
    if (copy == NULL) {
        fdk_dbus_message_free(r);
        reply_error(c, call, "org.freedesktop.DBus.Error.NoMemory");
        return;
    }
    memcpy(copy, text + start, n);
    copy[n] = '\0';
    (void)fdk_dbus_message_append_string(r, copy);
    fdk_free(copy);
    (void)fdk_dbus_connection_send(c, r);
}

/* The widget's desktop origin (the Component extents math, shared). */
static void widget_desktop_origin(const fdk_widget *w, int *out_x,
                                  int *out_y) {
    *out_x = 0;
    *out_y = 0;
    fdk_window *owner = fdk__window_of_owner(
        fdk__widget_window_owner((fdk_widget *)(uintptr_t)w));
    if (owner != NULL && owner->ops != NULL &&
        owner->ops->window_get_position != NULL) {
        fdk_i32 ox = 0, oy = 0;
        if (fdk_ok(owner->ops->window_get_position(owner->pwindow, &ox,
                                                   &oy))) {
            *out_x = (int)ox;
            *out_y = (int)oy;
        }
    }
}

/* Replies the (x, y, w, h) struct the four Extents methods share,
 * from a root-absolute rect. */
static void reply_extents_rect(fdk_dbus_connection *c,
                               fdk_dbus_message *call, int win_x,
                               int win_y, fdk_rect rect) {
    fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
    if (r == NULL) {
        return;
    }
    fdk_dbus_value *items[4] = {
        fdk_dbus_value_new_int32((int32_t)(win_x + rect.x)),
        fdk_dbus_value_new_int32((int32_t)(win_y + rect.y)),
        fdk_dbus_value_new_int32((int32_t)rect.width),
        fdk_dbus_value_new_int32((int32_t)rect.height),
    };
    fdk_dbus_value *st = fdk_dbus_value_new_struct(items, 4);
    if (st != NULL) {
        (void)fdk_dbus_message_append(r, st);
        fdk_dbus_value_free(st);
    }
    (void)fdk_dbus_connection_send(c, r);
}

static void serve_text(fdk_dbus_connection *c, fdk_dbus_message *call,
                       int index, const char *member) {
    fdk_widget *w = NULL;
    if (index == 0 || !slot_widget(index, &w) ||
        !fdk_a11y_has_text_interface(w)) {
        reply_error(c, call, "org.freedesktop.atspi.Error.Defunct");
        return;
    }
    size_t len = 0;
    char *text = fdk__widget_text_content(w, &len);
    if (text == NULL) {
        reply_error(c, call, "org.freedesktop.atspi.Error.Defunct");
        return;
    }

    /* Read the call's arguments once. */
    fdk_dbus_value **args = NULL;
    size_t n_args = 0;
    (void)fdk_dbus_message_read_args(call, &args, &n_args);
    int32_t a0 = (n_args >= 1 && args != NULL)
                     ? fdk_dbus_value_int32(args[0])
                     : 0;
    int32_t a1 = (n_args >= 2 && args != NULL)
                     ? fdk_dbus_value_int32(args[1])
                     : 0;
    /* u0 (the coord type on the extent calls) is deliberately
     * ignored: both AT-SPI coordinate spaces (SCREEN=0, WINDOW=1)
     * answer in the desktop space the Component interface reports —
     * the documented level-2 posture. */
    uint32_t u1 = (n_args >= 2 && args != NULL)
                      ? fdk_dbus_value_uint32(args[1])
                      : 0u;
    fdk_dbus_values_free_array(args, n_args);

    fdk_a11y_text_granularity gran = atspi_boundary_map(u1);

    if (strcmp(member, "GetCharacterCount") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_int32(
                r, atspi_byte_to_cp(text, len, len));
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetCaretOffset") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_int32(
                r, atspi_byte_to_cp(text, len,
                                    fdk_a11y_text_caret(w)));
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "SetCaretOffset") == 0) {
        size_t byte = atspi_cp_to_byte(text, len, a0);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            /* Snap inward to a codepoint boundary the editor accepts. */
            while (byte > 0 && !fdk_ok(fdk_a11y_text_set_caret(w, byte))) {
                byte = atspi_cp_to_byte(text, len,
                                        atspi_byte_to_cp(text, len,
                                                         byte) - 1);
                if (byte == 0) {
                    break;
                }
            }
            (void)fdk_dbus_message_append_boolean(
                r, fdk_ok(fdk_a11y_text_set_caret(w, byte)));
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetText") == 0) {
        reply_text_slice(c, call, text, len, a0, a1);
    } else if (strcmp(member, "GetTextAtOffset") == 0) {
        char buf[512];
        size_t start = 0, end = 0;
        size_t byte = atspi_cp_to_byte(text, len, a0);
        if (atspi_meaningful_run(w, text, len, byte, gran, buf,
                                 sizeof buf, &start, &end)) {
            reply_text_run(c, call, text, len, start, end);
        } else {
            reply_text_run(c, call, text, len, 0, 0);
        }
    } else if (strcmp(member, "GetTextBeforeOffset") == 0) {
        char buf[512];
        size_t start = 0, end = 0;
        size_t byte = atspi_cp_to_byte(text, len, a0);
        if (atspi_meaningful_run(w, text, len, byte, gran, buf,
                                 sizeof buf, &start, &end) &&
            atspi_hop_run(w, text, len, start, end, gran, -1, buf,
                          sizeof buf, &start, &end)) {
            reply_text_run(c, call, text, len, start, end);
        } else {
            reply_text_run(c, call, text, len, 0, 0);
        }
    } else if (strcmp(member, "GetTextAfterOffset") == 0) {
        char buf[512];
        size_t start = 0, end = 0;
        size_t byte = atspi_cp_to_byte(text, len, a0);
        if (atspi_meaningful_run(w, text, len, byte, gran, buf,
                                 sizeof buf, &start, &end) &&
            atspi_hop_run(w, text, len, start, end, gran, +1, buf,
                          sizeof buf, &start, &end)) {
            reply_text_run(c, call, text, len, start, end);
        } else {
            reply_text_run(c, call, text, len, 0, 0);
        }
    } else if (strcmp(member, "GetCharacterAtOffset") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            size_t byte = atspi_cp_to_byte(text, len, a0);
            fdk_u32 cp = 0;
            if (byte < len) {
                (void)fdk_text_utf8_next(text, len, byte, &cp);
            }
            (void)fdk_dbus_message_append_int32(r, (int32_t)cp);
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetStringAtOffset") == 0) {
        /* The newer granularity API (CHAR/WORD/SENTENCE/LINE/
         * PARAGRAPH, values 0..4): same mapping, string-only reply. */
        fdk_a11y_text_granularity g2 = atspi_boundary_map(
            u1 == 4 ? 5u : (u1 == 2 ? 1u : u1));
        char buf[512];
        size_t start = 0, end = 0;
        size_t byte = atspi_cp_to_byte(text, len, a0);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r == NULL) {
            fdk_free(text);
            return;
        }
        if (atspi_run_at(w, byte, g2, buf, sizeof buf, &start, &end)) {
            (void)fdk_dbus_message_append_string(r, buf);
        } else {
            (void)fdk_dbus_message_append_string(r, "");
        }
        (void)fdk_dbus_connection_send(c, r);
    } else if (strcmp(member, "GetNSelections") == 0) {
        size_t anchor = 0, caret = 0;
        int has = fdk_a11y_text_selection(w, &anchor, &caret) &&
                  anchor != caret;
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_int32(r, has ? 1 : 0);
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetSelection") == 0) {
        size_t anchor = 0, caret = 0;
        bool has = fdk_a11y_text_selection(w, &anchor, &caret) &&
                   anchor != caret;
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            if (has && a0 == 0) {
                size_t lo = anchor < caret ? anchor : caret;
                size_t hi = anchor < caret ? caret : anchor;
                (void)fdk_dbus_message_append_int32(
                    r, atspi_byte_to_cp(text, len, lo));
                (void)fdk_dbus_message_append_int32(
                    r, atspi_byte_to_cp(text, len, hi));
            } else {
                (void)fdk_dbus_message_append_int32(r, -1);
                (void)fdk_dbus_message_append_int32(r, -1);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "AddSelection") == 0 ||
               strcmp(member, "SetSelection") == 0) {
        /* Selection 0 is the only selection an FDK editor has. */
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r == NULL) {
            fdk_free(text);
            return;
        }
        bool ok = false;
        if (a0 == 0 || strcmp(member, "AddSelection") == 0) {
            int32_t lo = a0 < a1 ? a0 : a1;
            int32_t hi = a0 < a1 ? a1 : a0;
            size_t b_lo = atspi_cp_to_byte(text, len, lo);
            size_t b_hi = atspi_cp_to_byte(text, len, hi);
            ok = fdk_ok(fdk_a11y_text_set_selection(w, b_lo, b_hi));
        }
        (void)fdk_dbus_message_append_boolean(r, ok);
        (void)fdk_dbus_connection_send(c, r);
    } else if (strcmp(member, "RemoveSelection") == 0) {
        /* Collapse the selection to the caret. */
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r == NULL) {
            fdk_free(text);
            return;
        }
        size_t caret = fdk_a11y_text_caret(w);
        bool ok = fdk_ok(fdk_a11y_text_set_selection(w, caret, caret));
        (void)fdk_dbus_message_append_boolean(r, ok);
        (void)fdk_dbus_connection_send(c, r);
    } else if (strcmp(member, "GetAttributes") == 0 ||
               strcmp(member, "GetAttributeRun") == 0) {
        /* No text attributes exist in FDK's model at this level;
         * the run bounds are still honest (screen readers key layout
         * changes off them). */
        char buf[512];
        size_t start = 0, end = 0;
        size_t byte = atspi_cp_to_byte(text, len, a0);
        (void)atspi_run_at(w, byte, FDK_A11Y_TEXT_CHAR, buf,
                           sizeof buf, &start, &end);
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            fdk_dbus_value *arr =
                fdk_dbus_value_new_array("{ss}", NULL, 0);
            if (arr != NULL) {
                (void)fdk_dbus_message_append(r, arr);
                fdk_dbus_value_free(arr);
            }
            (void)fdk_dbus_message_append_int32(
                r, atspi_byte_to_cp(text, len, start));
            (void)fdk_dbus_message_append_int32(
                r, atspi_byte_to_cp(text, len, end));
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetDefaultAttributes") == 0 ||
               strcmp(member, "GetDefaultAttributeSet") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            fdk_dbus_value *arr =
                fdk_dbus_value_new_array("{ss}", NULL, 0);
            if (arr != NULL) {
                (void)fdk_dbus_message_append(r, arr);
                fdk_dbus_value_free(arr);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetAttributeValue") == 0) {
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_string(r, "");
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetCharacterExtents") == 0) {
        int win_x = 0, win_y = 0;
        widget_desktop_origin(w, &win_x, &win_y);
        size_t byte = atspi_cp_to_byte(text, len, a0);
        fdk_rect rect = {0, 0, 0, 0};
        if (!fdk__widget_text_char_rect(w, byte, &rect)) {
            rect = fdk_widget_get_absolute_bounds(w);
        }
        reply_extents_rect(c, call, win_x, win_y, rect);
    } else if (strcmp(member, "GetRangeExtents") == 0) {
        int win_x = 0, win_y = 0;
        widget_desktop_origin(w, &win_x, &win_y);
        size_t b0 = atspi_cp_to_byte(text, len, a0);
        size_t b1 = atspi_cp_to_byte(text, len, a1);
        fdk_rect r0 = {0, 0, 0, 0}, r1 = {0, 0, 0, 0};
        if (fdk__widget_text_char_rect(w, b0, &r0) &&
            fdk__widget_text_char_rect(
                w, b1 > 0 ? b1 - 1 : 0, &r1)) {
            fdk_rect union_rect = {
                r0.x < r1.x ? r0.x : r1.x,
                r0.y < r1.y ? r0.y : r1.y,
                0, 0,
            };
            union_rect.width =
                (r0.x + r0.width > r1.x + r1.width ? r0.x + r0.width
                                                   : r1.x + r1.width) -
                union_rect.x;
            union_rect.height =
                (r0.y + r0.height > r1.y + r1.height
                     ? r0.y + r0.height
                     : r1.y + r1.height) -
                union_rect.y;
            reply_extents_rect(c, call, win_x, win_y, union_rect);
        } else {
            reply_extents_rect(c, call, win_x, win_y,
                               fdk_widget_get_absolute_bounds(w));
        }
    } else if (strcmp(member, "GetOffsetAtPoint") == 0) {
        int win_x = 0, win_y = 0;
        widget_desktop_origin(w, &win_x, &win_y);
        size_t byte = 0;
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            if (fdk__widget_text_offset_at(w, a0 - win_x, a1 - win_y,
                                           &byte)) {
                (void)fdk_dbus_message_append_int32(
                    r, atspi_byte_to_cp(text, len, byte));
            } else {
                (void)fdk_dbus_message_append_int32(r, -1);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "ScrollSubstringTo") == 0 ||
               strcmp(member, "ScrollSubstringToPoint") == 0) {
        /* Editors already keep the caret visible; an explicit scroll
         * request is accepted and answered honestly: not needed. */
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            (void)fdk_dbus_message_append_boolean(r, 0);
            (void)fdk_dbus_connection_send(c, r);
        }
    } else if (strcmp(member, "GetBoundedRanges") == 0) {
        /* The clip-type matrix is magnifier territory; an empty
         * answer is the documented level-2 fence. */
        fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
        if (r != NULL) {
            fdk_dbus_value *arr =
                fdk_dbus_value_new_array("(iisv)", NULL, 0);
            if (arr != NULL) {
                (void)fdk_dbus_message_append(r, arr);
                fdk_dbus_value_free(arr);
            }
            (void)fdk_dbus_connection_send(c, r);
        }
    } else {
        reply_error(c, call, "org.freedesktop.DBus.Error.UnknownMethod");
    }
    fdk_free(text);
}

/* ---- GetRelationSet (C-2) ---------------------------------------------- */

/* FDK relation types -> the AT-SPI numbers (atspi-constants.h's
 * AtspiRelationType order, pinned against the reference header). */
static uint32_t relation_to_atspi(fdk_a11y_relation_type t) {
    switch (t) {
    case FDK_A11Y_RELATION_LABEL_FOR:
        return 1u;
    case FDK_A11Y_RELATION_LABELLED_BY:
        return 2u;
    case FDK_A11Y_RELATION_CONTROLLER_FOR:
        return 3u;
    case FDK_A11Y_RELATION_CONTROLLED_BY:
        return 4u;
    case FDK_A11Y_RELATION_DESCRIPTION_FOR:
        return 17u;
    case FDK_A11Y_RELATION_DESCRIBED_BY:
        return 18u;
    default:
        return 0u;
    }
}

/* Serves Accessible.GetRelationSet: a(ua(so)) — one struct per
 * relation TYPE with the targets as (app-name, object-path) pairs
 * (the app name is this bridge's own unique bus name — what the
 * registry's cache keys object identity by). */
static void serve_relation_set(fdk_dbus_connection *c,
                               fdk_dbus_message *call, int index) {
    fdk_dbus_message *r = fdk_dbus_message_new_method_return(call);
    if (r == NULL) {
        return;
    }
    const char *app = fdk_dbus_connection_unique_name(g_atspi.bus);
    if (app == NULL) {
        app = "";
    }
    static const fdk_a11y_relation_type k_types[] = {
        FDK_A11Y_RELATION_LABEL_FOR,
        FDK_A11Y_RELATION_LABELLED_BY,
        FDK_A11Y_RELATION_CONTROLLER_FOR,
        FDK_A11Y_RELATION_CONTROLLED_BY,
        FDK_A11Y_RELATION_DESCRIPTION_FOR,
        FDK_A11Y_RELATION_DESCRIBED_BY,
    };
    fdk_dbus_value *rels[8];
    size_t n_rels = 0;
    for (size_t t = 0;
         t < sizeof k_types / sizeof k_types[0] && n_rels < 8; t++) {
        size_t count =
            (index > 0) ? fdk_a11y_relation_count(
                              g_atspi.objs[index].w, k_types[t])
                        : 0;
        if (count == 0) {
            continue;
        }
        /* (u type, a(so) targets) */
        fdk_dbus_value *pair_items[16];
        size_t n_pairs = 0;
        for (size_t i = 0; i < count && i < 16; i++) {
            fdk_widget *target = NULL;
            if (!fdk_ok(fdk_a11y_relation_at(g_atspi.objs[index].w,
                                             k_types[t], i, &target)) ||
                target == NULL) {
                continue;
            }
            int ti = ensure_widget_object(target);
            if (ti < 0) {
                continue;
            }
            char path[96];
            obj_path(path, sizeof path, ti);
            fdk_dbus_value *pair[2] = {
                fdk_dbus_value_new_string(app),
                fdk_dbus_value_new_object_path(path),
            };
            pair_items[n_pairs] = fdk_dbus_value_new_struct(pair, 2);
            if (pair_items[n_pairs] != NULL) {
                n_pairs++;
            }
        }
        if (n_pairs == 0) {
            continue;
        }
        /* new_array takes ownership of the pair structs (freeing it
         * frees them) — no manual frees here. */
        fdk_dbus_value *targets =
            fdk_dbus_value_new_array("(so)", pair_items, n_pairs);
        if (targets == NULL) {
            /* Ownership NOT transferred on failure: free the pairs
             * we still hold. */
            for (size_t i = 0; i < n_pairs; i++) {
                fdk_dbus_value_free(pair_items[i]);
            }
            continue;
        }
        fdk_dbus_value *rel[2] = {
            fdk_dbus_value_new_uint32(relation_to_atspi(k_types[t])),
            targets,
        };
        rels[n_rels] = fdk_dbus_value_new_struct(rel, 2);
        if (rels[n_rels] != NULL) {
            n_rels++;
        }
    }
    fdk_dbus_value *arr =
        fdk_dbus_value_new_array("(ua(so))", rels, n_rels);
    if (arr == NULL) {
        for (size_t i = 0; i < n_rels; i++) {
            fdk_dbus_value_free(rels[i]);
        }
    }
    if (arr != NULL) {
        (void)fdk_dbus_message_append(r, arr);
        fdk_dbus_value_free(arr);
    }
    (void)fdk_dbus_connection_send(c, r);
}
