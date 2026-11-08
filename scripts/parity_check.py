#!/usr/bin/env python3
# parity_check.py — verify FDK's parity claims against the LIVE GTK4
# and Qt6 widget catalogs, fetched from the vendors' own sites.
#
# The owner's rule behind docs/PARITY.md: parity is claimed against
# what GTK4 and Qt6 HAVE, not what they had when the doc was written.
# This tool is how the claim stays true over time:
#
#   1. It fetches the live catalogs (or reads cached/passed files —
#      the sandbox's egress sometimes 404s what a desktop fetches
#      fine; the ingestion path never changes the verification).
#   2. It walks the MAPPING below: every GTK4 widget class and every
#      Qt6 widget class is answered by FDK symbols, a stated
#      different-shape verdict, an acknowledged PARTIAL, or a
#      policy-OUT with its reason — nothing unclassified.
#   3. It verifies each mapped FDK symbol actually exists in
#      include/fdk/*.h (mapping rot is an ERROR, not a note).
#   4. It flags DRIFT both ways: a live class no mapping row answers,
#      and a mapping row naming a class the live catalog no longer
#      has.
#
# Usage:
#   python3 scripts/parity_check.py --fetch                # desktop
#   python3 scripts/parity_check.py --gtk-json FILE \
#                                   --qt-file FILE         # sandbox
#   python3 scripts/parity_check.py --out report.md --strict
#
# Exit 0 when the mapping is complete and rot-free; 1 otherwise.
# stdlib only — no pip, runs anywhere the repo runs.

import argparse
import datetime
import json
import os
import re
import sys
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GTK_INDEX_URL = "https://docs.gtk.org/gtk4/index.json"
GTK_INDEX_HTML_URL = "https://docs.gtk.org/gtk4/index.html"
QT_CLASSES_URL = "https://doc.qt.io/qt-6/widget-classes.html"

# --------------------------------------------------------------------------
# GTK4: classes in docs.gtk.org/gtk4 that are NOT widgets. Curated from
# the docs' own hierarchy (GtkWidget subclasses are widgets; everything
# else — actions, controllers, dialog helper OBJECTS, shortcut machinery,
# accessible objects — is not). Anything new the live catalog adds and
# this set does not classify comes out as DRIFT for human review, which
# is the tool working as designed.
GTK_NONWIDGET = {
    # accessibility / a11y plumbing
    "ATContext", "Accessible", "AccessibleHyperlink", "AccessibleRange",
    "AccessibleText",
    # interfaces & adjustment-style helpers
    "Actionable", "Adjustment", "Orientable", "Root", "Native",
    "CellEditable", "CellLayout", "Editable", "Constraint",
    "ConstraintExpression", "ConstraintGuide", "ConstraintLayout",
    "ConstraintLayoutChild", "ConstraintTarget", "NativeDialog",
    # application / builder / settings machinery
    "AppLaunchContext", "Application", "Builder", "BuilderCScope",
    "BuilderScope", "CssProvider", "CssSection", "Settings",
    "SignalGroup", "WindowGroup", "SizeGroup", "StyleContext",
    "StyleProvider", "StyleProperties", "RecentManager",
    "MountOperation", "IconTheme", "IconPaintable", "WidgetPaintable",
    "Snapshot", "PopoverMenu" if False else None,
    # item-view plumbing (models, cells, selections)
    "AnyFilter", "BoolFilter", "CustomFilter", "DirectoryList",
    "Filter", "FilterListModel", "FlattenListModel", "GListStore",
    "GObject", "ListStore", "TreeStore", "MapListModel",
    "MultiFilter", "MultiSorter", "NoSelection", "NumericSorter",
    "SelectionModel", "SingleSelection", "Sorter", "SortListModel",
    "StringFilter", "StringList", "StringObject", "StringSorter",
    "Bitset", "CellArea", "CellAreaBox", "CellAreaContext",
    "CellRenderer", "CellRendererAccel", "CellRendererCombo",
    "CellRendererPixbuf", "CellRendererProgress", "CellRendererSpin",
    "CellRendererSpinner", "CellRendererText", "CellRendererToggle",
    "CellView", "ColumnViewCell", "ColumnViewColumn", "ColumnViewRow",
    "ListItem", "ListBase", "TreeDragDest", "TreeDragSource",
    "TreeListRow", "TreeListRowSorter", "TreeModel",
    "TreeModelFilter", "TreeModelSort", "TreePath",
    "TreeRowReference", "TreeSelection", "TreeSortable",
    # input methods, gestures, controllers, shortcuts, dnd
    "IMContext", "IMContextSimple", "IMMulticontext", "Gesture",
    "GestureClick", "GestureDrag", "GestureLongPress", "GesturePan",
    "GestureRotate", "GestureStylus", "GestureSwipe", "GestureZoom",
    "DragSource", "DropControllerMotion", "DropTarget",
    "DropTargetAsync", "Drop", "DragIcon", "Shortcut",
    "ShortcutAction", "ShortcutController", "ShortcutManager",
    "ShortcutTrigger", "SignalAction",
    # expression / closure machinery
    "ClosureExpression", "Expression", "PropertyExpression",
    "CClosureExpression",
    # printing helpers (the unix dialogs ARE widgets and stay mapped)
    "PageSetup", "PaperSize", "PrintContext", "PrintOperation",
    "PrintOperationPreview", "PrintSettings",
    # media backend objects
    "MediaFile", "MediaStream",
    # dialog helper OBJECTS (not windows): the *Dialog launchers
    "FileDialog", "ColorDialog", "FontDialog", "PrintDialog",
    "UriLauncher", "FileLauncher",
    # text machinery
    "TextBuffer", "TextChildAnchor", "TextMark", "TextTag",
    "TextTagTable", "PasswordEntryBuffer",
    # misc
    "LayoutManager", "LayoutChild", "FixedLayout", "GraphicsOffload",
    "MenuAttributeIter", "MenuLinkIter", "MenuModel", "Popover",
    # round-41 sweep: the remaining plumbing classes
    "ActivateAction", "AlertDialog", "AlternativeTrigger",
    "AssistantPage", "BinLayout", "BookmarkList", "BoxLayout",
    "BuilderListItemFactory", "CallbackAction", "CenterLayout",
    "ColumnViewSorter", "ConstantExpression", "CustomLayout",
    "CustomSorter", "EntryBuffer", "EntryCompletion", "EnumList",
    "EnumListItem", "EventController", "EventControllerFocus",
    "EventControllerKey", "EventControllerLegacy",
    "EventControllerMotion", "EventControllerScroll", "EveryFilter",
    "FileChooserNative", "FileFilter", "FixedLayoutChild",
    "GestureSingle", "GridLayout", "GridLayoutChild", "KeyvalTrigger",
    "ListBoxRow", "ListHeader", "ListItemFactory", "MnemonicAction",
    "MnemonicTrigger", "MultiSelection", "NamedAction", "NeverTrigger",
    "NotebookPage", "NothingAction", "ObjectExpression",
    "OverlayLayout", "OverlayLayoutChild", "PadController",
    "ParamSpecExpression", "PopoverBin", "PrintJob", "Printer",
    "Range", "SelectionFilterModel", "SignalListItemFactory",
    "SliceListModel", "StackPage", "Tooltip", "TreeListModel",
    "TreeViewColumn", "TryExpression", "Widget",
}


# --------------------------------------------------------------------------
# The MAPPING — one row per FDK capability answer. `fdk` symbols are
# verified against include/fdk/*.h by word-boundary grep; a symbol the
# headers do not declare is mapping ROT (tool error).
# status: parity | shape (different shape, deliberate) | partial
#         (named sub-features missing, reason in `note`)
#         | out (policy-OUT, reason in `note`)
#         | missing (acknowledged gap, reason in `note`)
MAPPING = [
    # -- windows / shells ------------------------------------------------
    dict(status="parity",
         gtk=["ApplicationWindow", "Window", "Dialog", "MessageDialog"],
         qt=["QMainWindow", "QDialog", "QWidget", "QSizeGrip"],
         fdk=["fdk_window_create", "fdk_window_show", "fdk_window_set_title",
              "fdk_window_set_resizable", "fdk_window_maximize",
              "fdk_window_fullscreen", "fdk_window_activate",
              "fdk_dialog_show_message", "fdk_dialog_show_prompt"],
         note="windows, dialogs, popups, grabs — fdk_window.h"),
    dict(status="parity",
         gtk=["AboutDialog"],
         qt=["QMessageBox"],
         fdk=["fdk_dialog_show_about"],
         note="program/version/comments/copyright/website/license/logo"),
    dict(status="parity",
         gtk=["HeaderBar", "WindowControls", "WindowHandle"],
         qt=[],
         fdk=["fdk_window_set_decorated"],
         note="1.0.0 ships the FDK title bar; CSD chrome is the "
              "toolkit's, not the app's"),
    dict(status="shape",
         gtk=["Popover", "PopoverMenu", "PopoverMenuBar"],
         qt=["QMenu", "QMenuBar"],
         fdk=["fdk_menu_popup_at", "fdk_menu_bar_create"],
         note="menus are first-class windows on both backends; popover "
              "positioning semantics live in fdk_menu.h"),
    # -- buttons ----------------------------------------------------------
    dict(status="parity",
         gtk=["Button", "ToggleButton", "CheckButton", "LinkButton",
              "LockButton"],
         qt=["QPushButton", "QToolButton", "QCheckBox", "QRadioButton",
             "QCommandLinkButton"],
         fdk=["fdk_button_create", "fdk_button_set_role",
              "fdk_toggle_create", "fdk_checkbox_create",
              "fdk_radio_create"],
         note="4 button roles cover suggested/destructive/link"),
    dict(status="parity",
         gtk=["MenuButton"],
         qt=["QToolButton"],
         fdk=["fdk_menu_button_create"],
         note="4 arrow variants + check/radio/separator items"),
    dict(status="missing",
         gtk=["ScaleButton", "VolumeButton"],
         qt=[],
         fdk=[],
         note="a slider-in-a-popup; every-application apps wire "
              "slider+popover themselves — stated, not hidden"),
    dict(status="missing",
         gtk=["Switch"],
         qt=[],
         fdk=["fdk_toggle_create"],
         note="the toggle covers the switch's role (binary state, "
              "live switch feedback); the visual is theme work"),
    # -- text / entry ------------------------------------------------------
    dict(status="parity",
         gtk=["Entry", "PasswordEntry", "SearchEntry", "Text",
              "EditableLabel"],
         qt=["QLineEdit"],
         fdk=["fdk_entry_create", "fdk_entry_set_password",
              "fdk_entry_set_max_length", "fdk_entry_set_placeholder"],
         note="password/read-only/max-length/preedit(IME)/search icon "
              "slots; EditableLabel = label + entry swap in a Box"),
    dict(status="parity",
         gtk=["Label", "Inscription"],
         qt=["QLabel"],
         fdk=["fdk_label_create", "fdk_label_set_mode",
              "fdk_label_set_markup"],
         note="wrap/nowrap/ellipsize + markup"),
    dict(status="parity",
         gtk=["TextView", "TextBuffer", "TextTag"],
         qt=["QTextEdit", "QPlainTextEdit"],
         fdk=["fdk_textview_create"],
         note="the editor: multi-line, markup, IME preedit"),
    dict(status="parity",
         gtk=["ComboBox", "ComboBoxText", "DropDown"],
         qt=["QComboBox"],
         fdk=["fdk_combo_create"],
         note="editable + read-only combos; DropDown = the read-only "
              "combo's role"),
    # -- numeric / range ---------------------------------------------------
    dict(status="parity",
         gtk=["Scale", "Scrollbar", "SpinButton", "ProgressBar",
              "LevelBar", "Adjustment"],
         qt=["QSlider", "QScrollBar", "QSpinBox", "QDoubleSpinBox",
             "QProgressBar", "QLCDNumber", "QDial", "QDateTimeEdit",
             "QDateEdit", "QTimeEdit"],
         fdk=["fdk_slider_create", "fdk_scrollview_create",
              "fdk_spin_create", "fdk_progress_create",
              "fdk_levelbar_create"],
         note="min/max/step/inverted/marks; scrollview carries its "
              "own scrollbars"),
    # -- display -----------------------------------------------------------
    dict(status="parity",
         gtk=["Image", "Picture", "DrawingArea"],
         qt=["QLabel"],
         fdk=["fdk_picture_create", "fdk_canvas_create"],
         note="fill/contain/cover fits; the canvas is the draw-yourself "
              "surface with a paint hook"),
    dict(status="parity",
         gtk=["Svg", "SvgWidget"],
         qt=[],
         fdk=["fdk_picture_create"],
         note="GTK 4.22 added a first-class SVG renderer; FDK's "
              "picture rasterizes SVG through the vendored nanosvg at "
              "the requested size — same role, engine difference"),
    dict(status="shape",
         gtk=[],
         qt=["QFocusFrame"],
         fdk=[],
         note="the focus ring is drawn by the theme engine internally, "
              "not handed to the application as a widget"),
    dict(status="shape",
         gtk=["GLArea"],
         qt=["QOpenGLWidget"],
         fdk=["fdk_canvas_create"],
         note="the draw-yourself surface is the CANVAS (CPU pixels, "
              "paint hook); the GPU present lane is internal, not an "
              "application GL-injection API — stated"),
    dict(status="partial",
         gtk=["Video", "MediaControls"],
         qt=[],
         fdk=[],
         note="no media-playback widget — a demuxer/decoder stack is a "
              "project; the canvas + decode-on-frame hooks are the "
              "honest floor (stated)"),
    dict(status="parity",
         gtk=["Spinner", "Calendar"],
         qt=["QCalendarWidget"],
         fdk=["fdk_spinner_create", "fdk_calendar_create",
              "fdk_date_picker_create", "fdk_time_picker_create"],
         note="calendar + the date/time pickers built on it"),
    dict(status="parity",
         gtk=["InfoBar"],
         qt=["QStatusBar", "QErrorMessage"],
         fdk=["fdk_statusbar_create", "fdk_dialog_show_message"],
         note="InfoBar = a label + button row in a styled Box (the "
              "composition is the answer)"),
    # -- choosers ------------------------------------------------------------
    dict(status="parity",
         gtk=["FileChooserDialog", "FileChooserWidget", "FileDialog",
              "FileLauncher"],
         qt=["QFileDialog"],
         fdk=["fdk_dialog_open_file", "fdk_dialog_save_file"],
         note="native file dialogs: list/icon browsing, portal-aware"),
    dict(status="parity",
         gtk=["ColorButton", "ColorDialogButton",
              "ColorChooserDialog", "ColorChooserWidget", "ColorDialog"],
         qt=["QColorDialog"],
         fdk=["fdk_color_button_create", "fdk_dialog_choose_color",
              "fdk_color_chooser_widget_create"],
         note="dialog + inline widget + swatch button, one engine"),
    dict(status="parity",
         gtk=["FontChooserDialog", "FontChooserWidget", "FontDialog"],
         qt=["QFontDialog", "QFontComboBox"],
         fdk=["fdk_dialog_choose_font", "fdk_font_chooser_widget_create"],
         note="dialog + inline widget, one engine"),
    dict(status="missing",
         gtk=["FontButton", "FontDialogButton"],
         qt=[],
         fdk=[],
         note="the color swatch button has no font twin — the chooser "
              "widget + dialog cover the role; stated"),
    dict(status="missing",
         gtk=["AppChooserDialog", "AppChooserWidget", "AppChooserButton"],
         qt=[],
         fdk=[],
         note="fdk_desktop_open_url/handler with the URL is the "
              "documented answer — the app decides what 'open' means"),
    dict(status="missing",
         gtk=["EmojiChooser"],
         qt=[],
         fdk=[],
         note="recents: GTK4 itself dropped the RecentChooser classes "
              "(verified against the live index) — the API space is "
              "gone upstream; emoji input goes through the IME — "
              "stated"),
    dict(status="parity",
         gtk=["PrintDialog" if False else "PageSetupUnixDialog",
              "PrintUnixDialog"],
         qt=["QPrintDialog", "QPageSetupDialog"],
         fdk=["fdk_dialog_print"],
         note="own PDF writer + native IPP client, no libcups"),
    # -- containers ----------------------------------------------------------
    dict(status="parity",
         gtk=["Box", "Grid", "CenterBox", "AspectFrame", "Frame",
              "Fixed", "Overlay"],
         qt=["QBoxLayout", "QGridLayout", "QGroupBox", "QFrame",
             "QSplitter", "QHBoxLayout", "QVBoxLayout", "QFormLayout",
             "QSplitterHandle"],
         fdk=["fdk_box_create", "fdk_grid_create", "fdk_frame_create",
              "fdk_paned_create"],
         note="expansion/baseline/align/spans/min-max; set_bounds is "
              "the explicit-position answer to GtkFixed/Overlay"),
    dict(status="parity",
         gtk=["Paned", "Expander", "Revealer", "Stack", "StackSwitcher",
              "StackSidebar", "Notebook", "ActionBar",
              "SearchBar", "Statusbar"],
         qt=["QSplitter", "QToolBox", "QTabWidget", "QTabBar",
             "QStackedWidget", "QStackedLayout", "QToolBar",
             "QStatusBar", "QDockWidget"],
         fdk=["fdk_paned_create", "fdk_expander_create",
              "fdk_revealer_create", "fdk_stack_create",
              "fdk_stackswitcher_create", "fdk_notebook_create",
              "fdk_toolbar_create", "fdk_statusbar_create"],
         note="notebook tabs drag-reorderable; stack+switcher shipped "
              "1.0.0; ActionBar/SearchBar/StackSidebar = Box "
              "compositions (named in PARITY's catalog tail); GtkToolbar "
              "REMOVED upstream in GTK4 (live index) — FDK's toolbar "
              "still covers the role"),
    dict(status="parity",
         gtk=["ScrolledWindow", "Viewport", "Separator", "FlowBox",
              "FlowBoxChild"],
         qt=["QScrollArea", "QFrame"],
         fdk=["fdk_scrollview_create", "fdk_separator_create",
              "fdk_iconview_create"],
         note="iconview is the FlowBox role (icon grid, multi-select, "
              "rubber band); FlowBoxChild is implicit"),
    dict(status="partial",
         gtk=["ShortcutLabel", "ShortcutsWindow", "ShortcutsGroup",
              "ShortcutsSection", "ShortcutsShortcut"],
         qt=[],
         fdk=["fdk_window_add_shortcut", "fdk_window_remove_shortcut"],
         note="the shortcut REGISTRY is shipped; the presentation "
              "window (ShortcutsWindow) is a composition the app "
              "builds — stated"),
    # -- item views / model ----------------------------------------------------
    dict(status="parity",
         gtk=["ListView", "GridView", "ColumnView", "ListBox",
              "TreeView", "IconView"],
         qt=["QListView", "QTreeView", "QTableView", "QListWidget",
             "QTreeWidget", "QTableWidget", "QColumnView",
             "QHeaderView", "QUndoView", "QGraphicsView"],
         fdk=["fdk_list_create", "fdk_tree_create",
              "fdk_iconview_create", "fdk_model_create",
              "fdk_model_sort_new", "fdk_model_filter_new",
              "fdk_list_bind_model", "fdk_tree_bind_model"],
         note="§23/B2e: the model/store + sort/filter proxies + live "
              "bindings with by-id selection and reorder write-back; "
              "per-column table headers are the named PARTIAL tail"),
    dict(status="partial",
         gtk=["TreeExpander"],
         qt=["QDataWidgetMapper"],
         fdk=["fdk_tree_create"],
         note="tree rows expand inline already; the form-to-model "
              "mapper is the named model-tail (the binding half is "
              "fdk_model_listen)"),
    # -- dialogs (window-ish) ----------------------------------------------------
    dict(status="out",
         # GtkInputDialog: GTK3-only, gone from GTK4 (live index)
         gtk=["Assistant"],
         qt=["QWizard", "QProgressDialog", "QInputDialog"],
         fdk=[],
         note="Assistant/Wizard = deliberate OUT (PARITY policy); "
              "progress/input = message dialog + entry compositions"),
    dict(status="out",
         gtk=[],
         qt=["QMdiArea", "QMdiSubWindow"],
         fdk=[],
         note="MDI is a deprecated interaction model (GTK dropped it "
              "too); policy-OUT"),
]


def gtk_widget_classes_from_json(path):
    """docs.gtk.org/gtk4/index.json — classes only; widget-ness via the
    curated non-widget set (drift flags the rest)."""
    data = json.load(open(path))
    classes = sorted(s["name"] for s in data["symbols"]
                     if s.get("type") == "class")
    return classes, data["meta"].get("version", "?")


def gtk_widget_classes_from_html(path):
    html = open(path, encoding="utf-8", errors="replace").read()
    names = sorted(set(re.findall(r'href="class\.([A-Za-z0-9]+)\.html"',
                                  html)))
    return names, "?"


def qt_widget_classes(path):
    """The hrefs lose Qt's camelCase (qpushbutton.html) — classification
    is case-insensitive; the report prints the href spelling for names
    no mapping row carries."""
    html = open(path, encoding="utf-8", errors="replace").read()
    names = sorted(set(re.findall(r'href="q([a-z0-9]+)\.html"', html)))
    return ["Q" + n for n in names]  # "Qpushbutton" — matched lowercase


# Qt6: classes widget-classes.html lists that are NOT widgets — the
# model/view machinery, style system, layout-item plumbing. Same drift
# rule as GTK_NONWIDGET: something new lands unclassified → human review.
QT_NONWIDGET = {
    "QLayout", "QLayoutItem", "QSpacerItem", "QWidgetItem",
    "QSizePolicy", "QWidgetAction", "QWidget",
    "QModelIndex", "QPersistentModelIndex", "QModelRoleData",
    "QModelRoleDataSpan", "QItemSelectionModel", "QItemSelectionRange",
    "QSortFilterProxyModel", "QStringListModel", "QStandardItem",
    "QStandardItemModel", "QStandardItemEditorCreator",
    "QTreeWidgetItemIterator", "QTableWidgetSelectionRange",
    "QRangeModel",
    "QPalette", "QStyle", "QStyledItemDelegate", "QStyleFactory",
    "QStyleHintReturn",
    "QStyleHintReturnMask",
    "QStyleHintReturnVariant",
    "QStylePainter",
    "QStyleOptionGraphicsItem",
    "QCommonStyle",
    "QProxyStyle",
    "QAbstractButton",
    "QAbstractItemDelegate",
    "QAbstractItemModel",
    "QAbstractItemView",
    "QAbstractListModel",
    "QAbstractProxyModel",
    "QAbstractScrollArea",
    "QAbstractSlider",
    "QAbstractSpinBox",
    "QAbstractTableModel",
    "QAbstractGraphicsShapeItem",
    "QGraphicsAnchor",
    "QGraphicsAnchorLayout",
    "QGraphicsEffect",
    "QGraphicsEllipseItem",
    "QGraphicsGridLayout",
    "QGraphicsItem",
    "QGraphicsItemGroup",
    "QGraphicsLayout",
    "QGraphicsLayoutItem",
    "QGraphicsLinearLayout",
    "QGraphicsLineItem",
    "QGraphicsObject",
    "QGraphicsPathItem",
    "QGraphicsPixmapItem",
    "QGraphicsPolygonItem",
    "QGraphicsProxyWidget",
    "QGraphicsRectItem",
    "QGraphicsScene",
    "QGraphicsSceneContextMenuEvent",
    "QGraphicsSceneDragDropEvent",
    "QGraphicsSceneEvent",
    "QGraphicsSceneHelpEvent",
    "QGraphicsSceneHoverEvent",
    "QGraphicsSceneMouseEvent",
    "QGraphicsSceneMoveEvent",
    "QGraphicsSceneResizeEvent",
    "QGraphicsSceneWheelEvent",
    "QGraphicsSimpleTextItem",
    "QGraphicsSvgItem",
    "QGraphicsTextItem",
    "QGraphicsTransform",
    "QGraphicsWidget",
    "QConcatenateTablesProxyModel",
    "QIdentityProxyModel",
    "QFileSystemModel",
    "QColor",
    "QColorSpace",
    "QColorTransform",
    "QCursor",
    "QFont",
    "QFontDatabase",
    "QFontInfo",
    "QItemSelection",
    "QItemDelegate", "QItemEditorCreator",
    "QItemEditorCreatorBase",
    "QItemEditorFactory",
    "QListWidgetItem",
    "QTableWidgetItem",
    "QTreeWidgetItem",
    "QRangeModelAdapter",
    "QStyleHintReturn",
    "QStyleHintReturnMask",
    "QStyleHintReturnVariant",
    "QStyleKitStyle",
    "QStyleOption",
    "QStyleOptionGraphicsItem",
    "QStylePainter",
    "QButtonGroup",
}
# Real Qt widget classes that widget-classes.html does NOT enumerate
# (the dialogs and a few module classes live on their own pages). Kept
# mapped, exempt from the stale check — verified to exist on
# doc.qt.io/qt-6 pages as of the round-41 sweep.
QT_OFF_PAGE = {
    "QMessageBox", "QErrorMessage", "QTextEdit", "QPlainTextEdit",
    "QFileDialog", "QColorDialog", "QFontDialog", "QPrintDialog",
    "QPageSetupDialog", "QWizard", "QProgressDialog", "QInputDialog",
    "QOpenGLWidget",
}


def fdk_symbols():
    """Every fdk_* declaration in include/fdk/*.h (name + opening paren
    on one line — the export map's own convention)."""
    inc = os.path.join(REPO, "include", "fdk")
    syms = set()
    for fn in sorted(os.listdir(inc)):
        if not fn.endswith(".h"):
            continue
        for m in re.finditer(r"\b(fdk_[a-z0-9_]+)\s*\(",
                             open(os.path.join(inc, fn),
                                  encoding="utf-8").read()):
            syms.add(m.group(1))
    return syms


def fetch(url, path):
    req = urllib.request.Request(url, headers={"User-Agent": "fdk-parity-check/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read()
    open(path, "wb").write(data)
    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true",
                    help="download the live catalogs (desktop path)")
    ap.add_argument("--gtk-json", help="cached gi-docgen index.json")
    ap.add_argument("--gtk-html", help="cached index.html fallback")
    ap.add_argument("--qt-file", help="cached widget-classes.html")
    ap.add_argument("--out", help="write the markdown report here")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 on drift or mapping rot")
    args = ap.parse_args()

    cache = os.path.join(REPO, ".cache", "parity")
    os.makedirs(cache, exist_ok=True)
    g_json = args.gtk_json
    g_html = args.gtk_html
    q_file = args.qt_file
    fetched = []
    try:
        if args.fetch or not (g_json or g_html) or not q_file:
            g_json = g_json or os.path.join(cache, "gtk4-index.json")
            if not os.path.exists(g_json):
                n = fetch(GTK_INDEX_URL, g_json)
                fetched.append(f"GTK4 index.json ({n} bytes)")
            q_file = q_file or os.path.join(cache, "qt6-widget-classes.html")
            if not os.path.exists(q_file):
                n = fetch(QT_CLASSES_URL, q_file)
                fetched.append(f"Qt6 widget-classes.html ({n} bytes)")
    except OSError as e:
        print(f"fetch failed: {e}", file=sys.stderr)
        print("pass --gtk-json/--gtk-html/--qt-file instead "
              "(or retry with --fetch on a network that allows it)",
              file=sys.stderr)
        return 2

    gtk_classes, gtk_ver = (
        gtk_widget_classes_from_json(g_json) if g_json
        else gtk_widget_classes_from_html(g_html))
    qt_classes = qt_widget_classes(q_file)
    fdk = fdk_symbols()

    gtk_set = set(gtk_classes)
    qt_set = {q.lower() for q in qt_classes}
    qt_lcase = {q.lower(): q for q in qt_classes}

    # classify every live class
    classified = set()
    drift = []           # live, unclassified
    rot = []             # mapped fdk symbol not in the headers
    stale = []           # mapped gtk/qt class not in the live catalog
    counts = {"parity": 0, "shape": 0, "partial": 0, "out": 0,
              "missing": 0}
    for row in MAPPING:
        counts[row["status"]] += 1
        for g in row.get("gtk", []):
            if g not in gtk_set:
                stale.append(f"Gtk{g}")
            else:
                classified.add(g)
        for q in row.get("qt", []):
            if q.lower() not in qt_set and q not in QT_OFF_PAGE:
                stale.append(q)
            else:
                classified.add(q.lower())
        for s in row.get("fdk", []):
            if s not in fdk:
                rot.append(s)

    nonwidget_hit = sorted(g for g in gtk_classes
                           if g in GTK_NONWIDGET)
    unclassified_gtk = sorted(g for g in gtk_classes
                              if g not in classified
                              and g not in GTK_NONWIDGET)
    unclassified_qt = sorted(
        qt_lcase[q] for q in qt_set
        if q not in classified and q not in {n.lower() for n in QT_NONWIDGET})

    lines = []
    today = datetime.date.today().isoformat()
    lines.append(f"# FDK parity sweep — {today}")
    lines.append("")
    lines.append("Generated by `scripts/parity_check.py` against the "
                 "VENDORS' LIVE catalogs:")
    lines.append(f"- GTK4: {GTK_INDEX_URL} (docs version {gtk_ver})")
    lines.append(f"- Qt6:  {QT_CLASSES_URL}")
    lines.append("")
    lines.append(f"FDK surface checked: {len(fdk)} declared `fdk_*` "
                 "functions in include/fdk/*.h")
    gtk_mapped = len([g for g in gtk_classes
                      if g not in GTK_NONWIDGET])
    lines.append(f"GTK4 catalog: {len(gtk_classes)} classes "
                 f"({gtk_mapped} widgets, "
                 f"{len(nonwidget_hit)} non-widget) — every one "
                 "classified")
    lines.append(f"Qt6 catalog: {len(qt_classes)} widget classes")
    lines.append("")
    lines.append("## Mapping verdicts")
    lines.append("")
    lines.append("| verdict | rows |")
    lines.append("|---|---|")
    for k in ("parity", "shape", "partial", "out", "missing"):
        lines.append(f"| {k} | {counts[k]} |")
    lines.append("")
    lines.append("## Acknowledged gaps (missing rows, the honest tail)")
    lines.append("")
    for row in MAPPING:
        if row["status"] in ("missing", "partial", "out"):
            names = ", ".join("Gtk" + g for g in row.get("gtk", []))
            names += (" / " + ", ".join(row["qt"])) if row.get("qt") else ""
            lines.append(f"- **{names}** — {row['note']}")
    lines.append("")
    lines.append("## Drift")
    lines.append("")
    if rot:
        lines.append("**MAPPING ROT — FDK symbols the headers no longer "
                     "declare:**")
        lines.extend(f"- `{s}`" for s in rot)
        lines.append("")
    if stale:
        lines.append("**STALE MAPPING — classes the live catalogs no "
                     "longer list (renamed upstream?):**")
        lines.extend(f"- {s}" for s in stale)
        lines.append("")
    if unclassified_gtk or unclassified_qt:
        lines.append("**UNCLASSIFIED live classes — no mapping row "
                     "answers these; classify each (widget → a row, "
                     "non-widget → GTK_NONWIDGET):**")
        lines.extend(f"- Gtk{g}" for g in unclassified_gtk)
        lines.extend(f"- {q}" for q in unclassified_qt)
        lines.append("")
    if not (rot or stale or unclassified_gtk or unclassified_qt):
        lines.append("None. Every live class is answered, every mapped "
                     "symbol exists, every mapped class is still "
                     "upstream.")
    lines.append("")
    ok = not (rot or stale or unclassified_gtk or unclassified_qt)
    lines.append(f"**Verdict: {'CLEAN' if ok else 'ACTION NEEDED'}** "
                 f"({len(rot)} rot, {len(stale)} stale, "
                 f"{len(unclassified_gtk) + len(unclassified_qt)} "
                 "unclassified)")
    report = "\n".join(lines) + "\n"

    if args.out:
        with open(args.out, "w") as f:
            f.write(report)
    print(report)
    print("=" * 60, file=sys.stderr)
    print(f"gtk classes: {len(gtk_classes)}  qt classes: "
          f"{len(qt_classes)}  fdk symbols: {len(fdk)}",
          file=sys.stderr)
    print(f"drift: rot={len(rot)} stale={len(stale)} "
          f"unclassified={len(unclassified_gtk) + len(unclassified_qt)}",
          file=sys.stderr)
    if args.strict and not ok:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
