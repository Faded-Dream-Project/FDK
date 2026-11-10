# Makefile — Faded Dream ToolKit (FDK)
#
# Targets:
#   make            build static + shared library (debug)
#   make release    build with optimizations, NDEBUG defined
#   make static     static library only (libfdk.a)
#   make shared     shared library only (libfdk.so)
#   make test       build and run the headless test suite (no display
#                   needed — safe for plain CI, see docs/testing.md)
#   make test-x11   build and run the X11 platform integration test;
#                   uses $DISPLAY if set, otherwise starts and tears
#                   down a throwaway Xvfb automatically
#   make examples   build example programs (linked against the static lib)
#   make tools      build the command-line tools (fdk-theme, fdk-prefs,
#                   fdk-set;
#                   also part of the default `make` — they are ordinary
#                   applications on the public API, its reference
#                   consumer)
#   make install    install headers + libraries + tools + shipped themes
#                   to PREFIX (default /usr/local)
#   make uninstall  remove what `install` installed
#   make clean      remove build output
#
# Override on the command line, e.g.:
#   make release PREFIX=/usr
#
# Backend build knobs (see docs/build.md "Optional Wayland"):
#   FDK_DISABLE_WAYLAND=1   — never build the Wayland backend, even if
#                              libwayland-client / libxkbcommon are present
#   FDK_ENABLE_WAYLAND=1     — require the Wayland backend at build time
#                              (errors out if its dev deps are missing
#                              rather than silently skipping)
#   (default)               — auto: build Wayland iff pkg-config finds
#                              wayland-client AND xkbcommon; otherwise
#                              silently skip and the runtime FDK_PLATFORM_*
#                              selection will report FDK_ERR_NO_DISPLAY
#                              or FDK_ERR_UNSUPPORTED as appropriate

CC       ?= gcc
AR       ?= ar
PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share
# Version for the pkg-config file and the shared-library SONAME;
# kept in one place, mirroring include/fdk/fdk_version.h (the header
# is the source of truth — a mismatch is a release-blocker).
FDK_PC_VERSION := $(shell sed -n 's/^#define FDK_VERSION_STRING "\(.*\)"/\1/p' include/fdk/fdk_version.h)
# SONAME major, also derived from the header: ABI-breaking changes
# bump FDK_VERSION_MAJOR (docs/versioning.md's soname policy) and
# with it the SONAME (libfdk.so.<major>). ABI-compatible releases
# (minor/patch bumps) keep the SONAME and only move the file version
# - the standard Linux convention, active since the public 1.0.0
# decision of 2026-10-12 (before that the .so carried no SONAME at
# all and installed as a single unversioned file).
FDK_VERSION_MAJOR := $(shell sed -n 's/^#define FDK_VERSION_MAJOR \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)
# The REAL .so file is named with the numeric MAJOR.MINOR.PATCH only —
# no pre-release suffix, ever. Two reasons: it is the GNU convention
# (versioned .so filenames are numeric), and Alpine's abuild derives
# the apk so:provides version from the filename tail
# (sover=${file##*.so.}) — a suffixed tail (libfdk.so.1.0.0-rc1) makes
# apk index reject the package ("package file format error", proven
# live on CI 2026-10-25). The rc suffix lives in the library's version
# string, pkg-config Version, and package metadata — not here.
FDK_VERSION_MINOR := $(shell sed -n 's/^#define FDK_VERSION_MINOR \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)
FDK_VERSION_PATCH := $(shell sed -n 's/^#define FDK_VERSION_PATCH \([0-9]*\).*/\1/p' include/fdk/fdk_version.h)
FDK_SO_VERSION := $(FDK_VERSION_MAJOR).$(FDK_VERSION_MINOR).$(FDK_VERSION_PATCH)
FDK_SONAME := libfdk.so.$(FDK_VERSION_MAJOR)

STD      := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wconversion -Wsign-conversion \
            -Wcast-qual -Wpointer-arith -Wundef -Wwrite-strings
FEATURE  := -D_POSIX_C_SOURCE=200809L

# ---- Platform backend dependencies -------------------------------------
# X11 is required (FDK's documented baseline — an FDK build without X11
# support is not a configuration the project currently supports; the
# runtime auto-detection in src/core/context.c still falls through to
# the X11 backend when Wayland is unavailable, so a build without X11
# would have no Wayland-only fall-through to offer).
X11_CFLAGS     := $(shell pkg-config --cflags x11) \
                 $(shell pkg-config --cflags xrandr 2>/dev/null)
# Xext: the MIT-SHM extension (XShmPutImage presentation fast path,
# Phase 3 completion). It is part of the base X11 distribution and
# present wherever libX11 is.
X11_LIBS       := $(shell pkg-config --libs x11) -lXext \
                 $(shell pkg-config --libs xrandr 2>/dev/null) \
                 -lXrender -ldl

# Wayland is optional. Detection order:
# 1. If FDK_DISABLE_WAYLAND=1 is set, never build Wayland (skip).
# 2. Else if FDK_ENABLE_WAYLAND=1 is set, require both dev packages —
#    a missing one is a hard build error, not a silent skip.
# 3. Else (default): build Wayland iff both pkg-config packages are
#    found. If either is missing, silently skip Wayland and continue
#    with an X11-only build. The runtime fdk_platform_wayland_ops()
#    then returns NULL (see wayland_ops.c) and the context's
#    select_and_connect() cleanly skips it.
WAYLAND_PKGS    := wayland-client xkbcommon
HAVE_WAYLAND    := $(shell pkg-config --exists $(WAYLAND_PKGS) && echo yes || echo no)

ifeq ($(FDK_DISABLE_WAYLAND),1)
  BUILD_WAYLAND := 0
else ifeq ($(FDK_ENABLE_WAYLAND),1)
  ifeq ($(HAVE_WAYLAND),yes)
    BUILD_WAYLAND := 1
  else
    $(error FDK_ENABLE_WAYLAND=1 but pkg-config did not find $(WAYLAND_PKGS) — install libwayland-dev / libxkbcommon-dev or unset FDK_ENABLE_WAYLAND to auto-skip)
  endif
else
  BUILD_WAYLAND := $(HAVE_WAYLAND:yes=1)
  BUILD_WAYLAND := $(filter 1,$(BUILD_WAYLAND))
endif

ifeq ($(BUILD_WAYLAND),1)
  WAYLAND_CFLAGS := $(shell pkg-config --cflags $(WAYLAND_PKGS))
  WAYLAND_LIBS   := $(shell pkg-config --libs $(WAYLAND_PKGS))
  $(info FDK build: X11 + Wayland backends)
else
  WAYLAND_CFLAGS :=
  WAYLAND_LIBS   :=
  WAYLAND_DEFS   := -DFDK_DISABLE_WAYLAND=1
  $(info FDK build: X11 backend only (Wayland not available — set FDK_ENABLE_WAYLAND=1 with libwayland-dev/libxkbcommon-dev installed to enable))
endif

BUILD_DIR   := build
# Sanitizers are the debug default on glibc toolchains. Musl-based
# or minimal environments that ship no sanitizer runtimes (Alpine's
# build-base, the lean Void image) set FDK_SANITIZE=0 to build the
# same debug configuration without -fsanitize; the sanitizer gates
# stay enforced on the glibc runs (CI ubuntu job + development
# machines). The config stamp below keys on this knob too, so a
# sanitized and an unsanitized build can never mix objects.
FDK_SANITIZE ?= 1
ifeq ($(FDK_SANITIZE),0)
  DEBUG_FLAGS := -g -O0 -DFDK_DEBUG_BUILD=1 -DFDK_DEBUG_THREAD_CHECKS=1
else
  DEBUG_FLAGS := -g -O0 -DFDK_DEBUG_BUILD=1 -DFDK_DEBUG_THREAD_CHECKS=1 -fsanitize=address,undefined
endif
REL_FLAGS   := -O2 -DNDEBUG

# Build configuration (debug by default; `make release` re-invokes
# make with FDK_CONFIG=release). The two configs MUST NOT share
# objects: they differ in -O level, NDEBUG, and the sanitizers, and
# make compares only timestamps — objects left over from the other
# config are always "up to date" and get silently linked in. That
# exact mixing (release objects inside an ASan-linked binary) is
# what made the compositor-death rig report fontconfig's
# process-lifetime allocations as leaks in 1.0.0: the LSan bracket
# in fontscan.c compiles to NOTHING without -fsanitize. The config
# stamp below forces a full rebuild whenever the configuration
# changes, and is a no-op (one stat) when it hasn't.
FDK_CONFIG  ?= debug
CONFIG_FLAGS := $(if $(filter release,$(FDK_CONFIG)),$(REL_FLAGS),$(DEBUG_FLAGS))
# The stamp key carries BOTH axes that change what an object file
# contains: debug/release AND Wayland on/off. The Wayland axis is
# environment-derived (pkg-config visibility), which makes it the
# dangerous one — an environment reset or a rig running make with a
# different PKG_CONFIG_PATH flips BUILD_WAYLAND without touching any
# source file, and without the stamp the stale objects/archive from
# the other configuration remain "up to date" by timestamp (found
# live in 1.0.0: the sway examples rig linked apps built against the
# Wayland-disabled stub, which then ignored a perfectly valid
# WAYLAND_DISPLAY and fell through to a dead X11). Both axes live in
# ONE stamp name so a flip of either forces one full rebuild.
CONFIG_STAMP := $(BUILD_DIR)/.config-$(FDK_CONFIG)-wl$(BUILD_WAYLAND)

# Flag composition (v1.0.0): the project's flag set is the BASE and
# any environment-provided CFLAGS/LDFLAGS are APPENDED. Distro build
# environments — abuild, makepkg, xbps-src, rpmbuild — export their
# own hardening flags there; under the old ?= definitions those
# exports REPLACED this list entirely, silently dropping -std=c17,
# -D_POSIX_C_SOURCE, and every -l the links need (observed live: the
# Alpine abuild-built CLI tools failed with undefined X11 references,
# the Arch makepkg-built tests with undefined libm symbols). Composing
# keeps the project's flags mandatory AND lets distro hardening
# apply. Command-line overrides (make CFLAGS=... LDFLAGS=...) still
# win over both, as always.
ENV_CFLAGS  := $(CFLAGS)
ENV_LDFLAGS := $(LDFLAGS)
CFLAGS  := $(STD) $(WARN) $(FEATURE) $(CONFIG_FLAGS) $(ENV_CFLAGS)
CPPFLAGS:= -Iinclude -Isrc
# -ldl: the text layer dlopen()s fontconfig at run time (optional
# system font discovery, see src/text/fontscan.c). No-op stub on
# glibc >= 2.34 where dlopen lives in libc; required for static
# linking on older glibc. Project -l flags precede the environment's
# so a distro-exported -Wl,--as-needed cannot drop them.
LDFLAGS := $(X11_LIBS) $(WAYLAND_LIBS) -lm -ldl -lz -pthread $(ENV_LDFLAGS)

# pkg-config exposure (fdk.pc): private deps are the ones the .so
# already links (apps need nothing); Requires.private lets static
# linking pull them in transitively.
# v1.0.0: the private link set ships as RAW FLAGS in fdk.pc's
# Libs.private, not as Requires.private package names — pkg-config
# resolves the Requires.private graph on every query and fails on
# machines without the X11 dev packages, which contradicts "a
# dynamically linked application needs none of them" (observed live
# in the Debian install-test container).
PC_LIBS_RAW := $(X11_LIBS) $(WAYLAND_LIBS) -ldl
# -ldl for static linking (see LDFLAGS above); -lm is appended by
# fdk.pc.in itself.
PC_LIBS := $(PC_LIBS_RAW)

# Per-source-file extra flags: platform backends need their own
# pkg-config include paths, and the Wayland backend additionally
# disables -Wcast-qual (only for its own translation units) because
# wayland-scanner's generated xdg-shell-client-protocol.h and
# wayland-client.h's own listener-registration inlines
# (wl_proxy_add_listener's `(void (**)(void))` cast) trigger it
# upstream, in code FDK does not own or control — see docs/build.md
# for the full rationale. No other warning is suppressed anywhere in
# the project.
extra_flags = $(if $(findstring src/platform/x11/,$(1)),$(X11_CFLAGS)) \
              $(if $(findstring src/platform/wayland/,$(1)),$(WAYLAND_CFLAGS) -Wno-cast-qual) \
              $(if $(findstring src/text/,$(1)),-Ithird_party/stb) \
              $(if $(findstring src/widget/,$(1)),-Ithird_party/stb) \
              $(if $(findstring src/render/,$(1)),-Ithird_party/stb) \
              $(if $(findstring src/print/,$(1)),-Ithird_party/stb) \
              $(if $(findstring src/theme/,$(1)),-Ithird_party/nanosvg)

# --- Sources ------------------------------------------------------------

CORE_SRCS     := $(wildcard src/core/*.c)
PLATFORM_X11_SRCS     := $(wildcard src/platform/x11/*.c)
ifeq ($(BUILD_WAYLAND),1)
  PLATFORM_WAYLAND_SRCS := $(wildcard src/platform/wayland/*.c) \
                           src/platform/wayland/generated/xdg-shell-protocol.c \
                           src/platform/wayland/generated/xdg-decoration-unstable-v1-protocol.c \
                           src/platform/wayland/generated/viewporter-protocol.c \
                           src/platform/wayland/generated/fractional-scale-v1-protocol.c \
                           src/platform/wayland/generated/primary-selection-unstable-v1-protocol.c \
                           src/platform/wayland/generated/cursor-shape-v1-protocol.c \
                           src/platform/wayland/generated/xdg-toplevel-icon-v1-protocol.c \
                           src/platform/wayland/generated/text-input-unstable-v3-protocol.c \
                           src/platform/wayland/generated/idle-inhibit-unstable-v1-protocol.c \
                           src/platform/wayland/generated/xdg-activation-v1-protocol.c \
                           src/platform/wayland/generated/tablet-unstable-v2-protocol.c \
                           src/platform/wayland/generated/wlr-foreign-toplevel-management-unstable-v1-protocol.c \
                           src/platform/wayland/generated/xdg-session-management-v1-protocol.c \
                           src/platform/wayland/generated/wlr-layer-shell-unstable-v1-protocol.c \
                          src/platform/wayland/generated/ext-session-lock-v1-protocol.c \
                          src/platform/wayland/generated/input-method-unstable-v2-protocol.c \
                          src/platform/wayland/generated/virtual-keyboard-unstable-v1-protocol.c
else
  # Stub providing fdk_platform_wayland_ops() returning NULL and
  # fdk_platform_wayland_display_present() returning 0, so
  # src/core/context.c's select_and_connect() cleanly skips Wayland
  # and falls through to X11. See wayland_disabled.c for why this is
  # a separate file (NOT under src/platform/wayland/, which would
  # break the wildcard that's active when Wayland IS enabled).
  PLATFORM_WAYLAND_SRCS := src/platform/wayland_disabled.c
endif

# Platform-common sources (explicitly listed; the src/platform root
# has no wildcard — the wayland_disabled.c precedent). cursor_theme.c
# is the shared XCursor theme loader both backends ride (1.0.0-rc2
# S-5; needs neither backend's flags — plain libc).
PLATFORM_COMMON_SRCS := src/platform/cursor_theme.c
WINDOW_SRCS   := $(wildcard src/window/*.c)
RENDER_SRCS   := $(wildcard src/render/*.c)
WIDGET_SRCS   := $(wildcard src/widget/*.c)
LAYOUT_SRCS   := $(wildcard src/layout/*.c)
TEXT_SRCS     := $(wildcard src/text/*.c)
THEME_SRCS    := $(wildcard src/theme/*.c)
I18N_SRCS     := $(wildcard src/i18n/*.c)
DBUS_SRCS     := $(wildcard src/dbus/*.c)
DESKTOP_SRCS  := $(wildcard src/desktop/*.c)
PRINT_SRCS    := $(wildcard src/print/*.c)
LIB_SRCS      := $(CORE_SRCS) $(PLATFORM_X11_SRCS) $(PLATFORM_WAYLAND_SRCS) $(PLATFORM_COMMON_SRCS) $(WINDOW_SRCS) $(RENDER_SRCS) $(WIDGET_SRCS) $(LAYOUT_SRCS) $(TEXT_SRCS) $(THEME_SRCS) $(I18N_SRCS) $(DBUS_SRCS) $(DESKTOP_SRCS) $(PRINT_SRCS)

# Static and shared builds use separate object trees (obj/ vs obj-pic/)
# since shared objects must be position-independent (-fPIC) and static
# ones need not be — reusing one tree between `make static` and
# `make shared` in the same invocation would silently link stale
# non-PIC objects into the .so. See docs/build.md.
LIB_OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRCS))
LIB_OBJS_PIC  := $(patsubst src/%.c,$(BUILD_DIR)/obj-pic/%.o,$(LIB_SRCS))

STATIC_LIB := $(BUILD_DIR)/libfdk.a
SHARED_LIB := $(BUILD_DIR)/libfdk.so

# Shared-library export map (1.0.0). The .so must export EXACTLY the
# public API (include/fdk) — nothing else. Without a version script it
# exports every global in the archive: backend internals (fdk_x11_*,
# fdk_wayland_*), cross-module seams (fdk__*), the vendored stb symbols
# (stbi_*/stbtt_*), the wayland-scanner interface structs — ~230 symbols
# an application has no business resolving. Beyond surface noise that
# is an interposition hazard: with default visibility, an app that
# vendors its own stb_image gets FDK's internal stbi_* calls bound to
# the app's copy (or vice versa) by the dynamic linker — silently
# mixing two stb versions across a DSO boundary.
#
# The map is GENERATED at build time, never committed: the export list
# is the intersection of (symbols declared in include/fdk/*.h) and
# (symbols the static archive actually defines). Intersection, not
# union: header mentions that are not linkable symbols (typedefs like
# fdk_a11y_action_set, static-inline helpers like fdk_ok) drop out on
# the archive side; archive globals that are internal (fdk__plural_*,
# fdk_x11_*, *_class_def, ...) drop out on the header side. Both sides
# regenerate from source on every build, so the exported surface can
# never drift from the headers the way a committed list would.
#
# Header style constraint this depends on: a public function's name
# and its opening paren must sit on ONE line in include/fdk/*.h (the
# generator greps the `fdk_name(` token; return types and parameter
# lists may wrap freely). Every header already follows this.
EXPORT_MAP := $(BUILD_DIR)/libfdk.exports.map

TEST_SRCS := $(filter-out tests/crosssection.c tests/test_dpi_scale.c tests/test_gpu_paint_wl.c tests/test_x11_share.c tests/test_arbitration.c tests/test_p3_protocols.c tests/test_matrix.c tests/test_dmabuf.c tests/test_x11_integration.c tests/test_gpu_paint.c tests/probe_cpu.c tests/test_wayland_integration.c tests/test_dbus_live.c tests/test_notify_client.c tests/test_tray_item.c tests/test_clip_uris.c tests/bench.c,$(wildcard tests/*.c))
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

# Every test binary ALSO depends on every FDK header (public AND
# private). The one-step test compiles inline whatever headers the
# test includes — window_internal.h, the platform headers — so a
# private-struct change with no surviving library includer would
# otherwise leave a stale test object linked against a fresh lib
# (the 0xe5 mixed-layout class, 2026-11-11). Broad by design: a
# header touch is rare and correctness beats minimality here.
ALL_FDK_HEADERS := $(wildcard include/fdk/*.h include/fdk/ext/*.h \
    src/*.h src/window/*.h src/render/*.h src/text/*.h src/widget/*.h \
    src/platform/*.h src/platform/x11/*.h src/platform/wayland/*.h \
    src/platform/wayland/generated/*.h)

X11_TEST_SRC := tests/test_x11_integration.c
X11_TEST_BIN := $(BUILD_DIR)/tests/test_x11_integration

# §23/B2a: the uri-list clipboard rig (needs a display; runs in
# test-x11's Xvfb).
X11_URI_TEST_BIN := $(BUILD_DIR)/tests/test_clip_uris

# §25/G-2: the threading suite. Runs under test-x11's throwaway Xvfb
# (it needs a real context + pump), AND under ThreadSanitizer via
# test-threads-tsan below — TSan must instrument every translation
# unit including the library, so it rebuilds its own object lane.
THREAD_TEST_SRC := tests/test_threads.c
THREAD_TEST_BIN := $(BUILD_DIR)/tests/test_threads

$(THREAD_TEST_BIN): $(THREAD_TEST_SRC) $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -pthread $(THREAD_TEST_SRC) $(STATIC_LIB) -o $@ $(LDFLAGS)

# The TSan lane: a dedicated object tree (like the release lane),
# library included, the thread suite on top. Not part of plain
# `make test` (TSan slows everything by an order of magnitude) — an
# on-demand gate for threading work, run before pushing it.
THREAD_TSAN_DIR := $(BUILD_DIR)/obj-tsan
THREAD_TSAN_LIB := $(BUILD_DIR)/libfdk-tsan.a

$(THREAD_TSAN_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(STD) $(WARN) $(FEATURE) -fsanitize=thread -g -O1 -pthread \
	    $(X11_CFLAGS) $(WAYLAND_CFLAGS) $(call extra_flags,$<) -c $< -o $@

$(THREAD_TSAN_LIB): $(patsubst src/%.c,$(THREAD_TSAN_DIR)/%.o,$(LIB_SRCS))
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD_DIR)/tests/test_threads-tsan: $(THREAD_TEST_SRC) $(THREAD_TSAN_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(STD) $(WARN) $(FEATURE) -fsanitize=thread -g -O1 -pthread $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $(THREAD_TEST_SRC) $(THREAD_TSAN_LIB) -o $@ $(LDFLAGS)

.PHONY: test-threads-tsan
test-threads-tsan: $(BUILD_DIR)/tests/test_threads-tsan
	@if [ -n "$$DISPLAY" ]; then \
	    TSAN_OPTIONS="halt_on_error=1" "$(BUILD_DIR)/tests/test_threads-tsan"; \
	else \
	    XVFB_NUM=$$((90 + ($$$$ % 400))); \
	    XVFB_DISP=":$$XVFB_NUM"; \
	    rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
	    Xvfb "$$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdk-xvfb-tsan.log 2>&1 & \
	    XVFB_PID=$$!; \
	    sleep 1.5; \
	    echo "== running the thread suite under TSan (Xvfb :$$XVFB_NUM) =="; \
	    DISPLAY="$$XVFB_DISP" TSAN_OPTIONS="halt_on_error=1" "$(BUILD_DIR)/tests/test_threads-tsan"; \
	    TEST_RESULT=$$?; \
	    kill "$$XVFB_PID" 2>/dev/null; wait "$$XVFB_PID" 2>/dev/null; \
	    rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
	    exit $$TEST_RESULT; \
	fi

EXAMPLE_SRCS := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/examples/%,$(EXAMPLE_SRCS))

# Command-line tools (1.0.0). Built like tests and examples — against
# the static archive, on the public headers only (tools/toolutil.h is
# the one tools-local header). They link the full LDFLAGS set like
# every other consumer of libfdk.a.
# widget_sizes.c is NOT a shipped CLI tool: it is the D1 measurement
# probe (private include path), built by the `sizes` target below.
TOOL_SRCS := $(filter-out tools/widget_sizes.c,$(wildcard tools/*.c))
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/tools/%,$(TOOL_SRCS))

# test_cli_tools stat()s the three tools next to itself; the test
# binary therefore depends on them (a bare `make test` builds them).
$(BUILD_DIR)/tests/test_cli_tools: $(TOOL_BINS)

# Shipped themes: the source tree's .FDKThemes/ folder (1.0.0 —
# the custom folder the maintainer asked for by name), installed
# into $(DATADIR)/fdk/.FDKThemes, which the runtime discovery path
# reaches through the default $XDG_DATA_DIRS (/usr/local/share and
# /usr/share both are) — an installed FDK finds its own themes with
# zero configuration, and `fdk-theme list` after `make install`
# shows exactly these next to the built-in.
#
# 1.0.0 alignment (the owner's directive, 2026-10-12): the package
# ships EXACTLY ONE face — faded-dream, the packaged copy of the
# built-in default. The list is PINNED (not a wildcard) so a stray
# file in .FDKThemes/ can never silently ride into a release; the
# next faces are .FDKThemes drop-ins the user installs (the
# $HOME/.FDKThemes slot discovery already scans), not package
# content. The nine former faces live on as engine fixtures in
# tests/data/faces/ (pinned by test_shipped_themes.c, never
# installed).
SHIPPED_THEMES := .FDKThemes/faded-dream.fdk

# Header dependency tracking: -MMD -MP writes a .d beside each object
# naming every header it included. Without this, editing a struct in
# an internal header did NOT recompile the .c files that include it
# (make only knows .c -> .o), producing stale objects writing struct
# fields at pre-edit offsets - exactly the class of bug that bit the
# Phase 8 session (a field added to x11_platform.h mid-struct; the
# registry kept using the old offsets and ASan caught the corruption
# on the first window creation). Tests and examples compile in one
# step and depend on $(STATIC_LIB), so they rebuild whenever any
# library object did.
DEPS := $(LIB_OBJS:.o=.d) $(LIB_OBJS_PIC:.o=.d)

.PHONY: all release static shared test test-x11 test-dbus test-textdemo test-wayland bench examples tools test-apps test-apps-smoke install uninstall clean verify-exports sizes

# D1 (PRE-1.0-DECISIONS): the embed-budget MEASUREMENT. The decision's
# rule — keep 1024 iff the largest widget is under 640 bytes — is
# decided by DATA, so the data has a target. One shared struct serves
# every family (klass->size == sizeof(fdk_widget) for all), so the
# "per family" measurement collapses to the one number + the headroom.
sizes: $(BUILD_DIR)/widget_sizes
	@echo "== widget embed budget (D1) =="
	@$(BUILD_DIR)/widget_sizes

$(BUILD_DIR)/widget_sizes: tools/widget_sizes.c src/widget/widget_internal.h \
    $(wildcard include/fdk/*.h)
	@mkdir -p $(dir $@)
	$(CC) -Iinclude -Isrc/widget -Isrc -O0 $< -o $@

# The default goal is `all` BY NAME, not by file position: the
# thread-test rule moved above `all:` once (G-2, 4d06933) and a
# bare `make` silently built only that binary + the archive —
# CI's clean checkouts then failed test_cli_tools' stat() on the
# never-built tools while every incremental sandbox (whose
# build/tools/ already existed) stayed green.
.DEFAULT_GOAL := all

all: static shared tools

# `make release` re-invokes make with FDK_CONFIG=release: every
# object then rebuilds against the release flags through the config
# stamp (see the FDK_CONFIG block above), and build/libfdk.a is the
# release artifact. A plain target-specific CFLAGS override here
# (the old design) shared build/obj between the configs and let
# timestamp-only up-to-date checks silently link release objects
# into later debug builds (and vice versa).
release:
	$(MAKE) FDK_CONFIG=release all
	$(MAKE) -C test-apps release

static: $(STATIC_LIB)
shared: $(SHARED_LIB)

# The stamp rule always RUNS (FORCE) but only rewrites itself when
# the configuration actually changed — so same-config builds stay
# incremental (the stamp keeps its old mtime), while a config switch
# produces a fresh, newer stamp that rebuilds every object.
$(CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@cur=`cat $@ 2>/dev/null || echo none`; \
	if [ x"$$cur" != x"$(FDK_CONFIG)-wl$(BUILD_WAYLAND)-san$(FDK_SANITIZE)" ]; then \
		rm -f $(BUILD_DIR)/.config-*; \
		printf '%s\n' '$(FDK_CONFIG)-wl$(BUILD_WAYLAND)-san$(FDK_SANITIZE)' > $@; \
	fi

.PHONY: FORCE
FORCE:

$(BUILD_DIR)/obj/%.o: src/%.c $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_DEFS) $(call extra_flags,$<) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj-pic/%.o: src/%.c $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_DEFS) $(call extra_flags,$<) -MMD -MP -fPIC -c $< -o $@

# `rm -f` before archiving: `ar rcs` REPLACES the named members but
# never drops members that fell out of the list — a config switch
# (Wayland on->off) left the stale wayland_*.o inside libfdk.a next
# to the new wayland_disabled.o, and every link after that pulled
# symbols from a backend the build no longer contains (found live in
# 1.0.0 after an environment reset wiped the Wayland toolchain: the
# link failed on wayland_seat.o's wl_proxy_get_version against a
# system libwayland with no dev files). Same bug class as the 1.0.0
# obj/obj-pic mixing one directory over; the archive is now always
# exactly the current member list.
$(STATIC_LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS_PIC) $(EXPORT_MAP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,$(FDK_SONAME) -Wl,--version-script=$(EXPORT_MAP) -o $@ $(LIB_OBJS_PIC) $(LDFLAGS)
	@# the SONAME link in-tree: anything linking -lfdk needs
	# libfdk.so.<major> at RUN time, not just at install time
	# (any dlopen consumer resolves the same libfdk.so.1 —
	# found live by the smoke matrix)
	ln -sf libfdk.so $(BUILD_DIR)/$(FDK_SONAME)

# Generates the export map: intersection of header-declared names and
# archive-defined globals (see the EXPORT_MAP block above). Depends on
# the static archive so `make shared` after any code change regenerates
# it; depends on the headers so adding/removing a public function does
# too. nm/sed/sort/uniq/tr are binutils+coreutils — no build-time
# dependency beyond what the build already uses.
$(EXPORT_MAP): $(STATIC_LIB) $(wildcard include/fdk/*.h) $(wildcard include/fdk/ext/*.h) Makefile
	@echo "== generating $(EXPORT_MAP) (public API only; internals stay local) =="
	@# One script, two named nodes (the standard multi-node layout):
	@# FDK_1.0 for the stable API, FDK_UNSTABLE for the explicitly
	@# unstable tier (include/fdk/unstable.h — "may break any
	@# release"); verify-exports checks both tiers.
	@printf 'FDK_1.0 {\n  global:\n' > $@
	@{ nm --defined-only $(STATIC_LIB) \
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' | sort -u; \
	   grep -hoE '\bfdk_[a-z0-9_]+[[:space:]]*\(' \
	           include/fdk/*.h include/fdk/ext/*.h \
	           | tr -d ' (' | sort -u; } \
	  | sort | uniq -d | grep -v '^fdk_unstable_' \
	  | sed 's/^/  /; s/$$/;/' >> $@
	@printf '  local:\n    *;\n};\n' >> $@
	@printf 'FDK_UNSTABLE {\n  global:\n' >> $@
	@{ nm --defined-only $(STATIC_LIB) \
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' | sort -u; \
	   grep -hoE '\bfdk_unstable_[a-z0-9_]+[[:space:]]*\(' \
	           include/fdk/unstable.h | tr -d ' (' | sort -u; } \
	  | sort | uniq -d | sed 's/^/  /; s/$$/;/' >> $@
	@printf '};\n' >> $@

# Belt-and-braces check for the one blind spot the map generator has
# (a declaration whose name/paren wrapped across lines would be missed
# by the grep): every fdk_* global in the archive that is NOT exported
# must NOT be mentioned in include/fdk at all. If it is, it is public
# but unexported — a real bug, fail loudly. Internal symbols
# (fdk__prefixed, fdk_x11_*/fdk_wayland_*, *_class_def, ...) pass
# because they never appear in the public headers. Run in the battery
# alongside the builds (docs/testing.md).
verify-exports: $(SHARED_LIB)
	@echo "== verifying the shared library's exported surface =="
	@exports=$$(nm -D --defined-only $(SHARED_LIB) \
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' \
	           | sed 's/@@.*//' | sort -u); \
	echo "exported symbols: $$(printf '%s\n' "$$exports" | wc -l)"; \
	bad=$$(printf '%s\n' "$$exports" | grep -v '^fdk_' || true); \
	if [ -n "$$bad" ]; then \
	        echo "FAIL: non-public exports leaked into the .so:"; \
	        printf '%s\n' "$$bad"; exit 1; \
	fi; \
	missing=0; \
	for s in $$(nm --defined-only $(STATIC_LIB) \
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' \
	           | grep '^fdk_' | grep -v '^fdk__' || true); do \
	        if ! printf '%s\n' "$$exports" | grep -qx "$$s"; then \
	                if grep -rqw --include='*.h' "$$s" include/fdk/; then \
	                        echo "FAIL: $$s is declared in include/fdk but NOT exported"; \
	                        missing=1; \
	                fi; \
	        fi; \
	done; \
	[ $$missing -eq 0 ] || exit 1; \
	echo "EXPORT VERIFICATION OK: the .so exports exactly the public API"; \
	unstable=$$(printf '%s\n' "$$exports" | grep '^fdk_unstable_' || true); \
	[ -n "$$unstable" ] && echo "unstable tier: $$(printf '%s\n' "$$unstable" | wc -l) symbol(s) in the FDK_UNSTABLE node"; \
	true

# Tests may include src/ internal headers (precedent: window_internal.h,
# widget_internal.h, ...) and some of those embed third-party headers
# (text_internal.h embeds stb_truetype.h), so the stb include path is
# on the test compile lines too.

# --- Tests ----------------------------------------------------------------

test: $(TEST_BINS)
	@echo "== running tests =="
	@for t in $(TEST_BINS); do \
		echo "-- $$t --"; \
		"$$t" || exit 1; \
	done
	@echo "== all tests passed =="

$(BUILD_DIR)/tests/%: tests/%.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_DEFS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< $(STATIC_LIB) $(shell pkg-config --cflags xkbcommon 2>/dev/null) -o $@ $(LDFLAGS) $(shell pkg-config --libs xkbcommon 2>/dev/null)

# X11 platform integration test — NOT part of plain `make test` (see
# docs/testing.md's headless-by-default policy). Requires a reachable
# X11 display. If $DISPLAY is already set, runs against it directly
# (a real desktop session, or a Xvfb/Xephyr you started yourself). If
# not, starts a throwaway Xvfb, runs the test against it, and tears it
# down after — so `make test-x11` works out of the box in a headless
# CI container with no manual setup. See docs/testing.md for exactly
# what this test does and does not cover (window-manager-dependent
# behavior like WM_DELETE_WINDOW delivery is skipped under bare Xvfb,
# not faked — the test says so when it runs).
GPU_PAINT_TEST_BIN := $(BUILD_DIR)/tests/test_gpu_paint

$(GPU_PAINT_TEST_BIN): tests/test_gpu_paint.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual tests/test_gpu_paint.c $(STATIC_LIB) -o $@ $(LDFLAGS) -lX11

# The GPU paint engine's runtime proof: CPU-vs-GPU pixel equivalence
# under llvmpipe (FDK_GL_FORCE=1) in a throwaway Xvfb, plus the
# GPU-mode contracts (UNSUPPORTED without GL, texture roundtrip,
# resize). Standalone gate; not part of plain `make test` (needs GL).
test-gpu-paint: $(GPU_PAINT_TEST_BIN)
	@mkdir -p /tmp/fdk-gpu-test
	XVFB_NUM=$$((90 + ($$$$ % 400))); \
	    rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
	    Xvfb ":$$XVFB_NUM" -screen 0 1024x768x24 >/tmp/fdk-gpu-xvfb.log 2>&1 & \
	    XVFB_PID=$$!; \
	    DISPLAY=:$$XVFB_NUM LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-gl.supp \
	    $(BUILD_DIR)/tests/test_gpu_paint;
	    rc=$$?; kill $$XVFB_PID 2>/dev/null; exit $$rc

test-x11: $(X11_TEST_BIN) $(X11_URI_TEST_BIN) $(THREAD_TEST_BIN)
	@if [ -n "$$DISPLAY" ]; then \
		echo "== running X11 integration test against DISPLAY=$$DISPLAY =="; \
		LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/tests/data/lsan-gl.supp" "$(X11_TEST_BIN)" || exit 1; \
		echo "== running the uri-list clipboard rig =="; \
		LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/tests/data/lsan-gl.supp" "$(X11_URI_TEST_BIN)" || exit 1; \
		echo "== running the G-2 thread suite =="; \
		LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/tests/data/lsan-gl.supp" "$(THREAD_TEST_BIN)" || exit 1; \
	else \
		echo "== no DISPLAY set, starting a throwaway Xvfb =="; \
		XVFB_NUM=$$((90 + ($$$$ % 400))); \
		XVFB_DISP=":$$XVFB_NUM"; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		Xvfb "$$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdk-xvfb-test.log 2>&1 & \
		XVFB_PID=$$!; \
		sleep 2; \
		echo "== running X11 integration test against DISPLAY=$$XVFB_DISP (Xvfb pid $$XVFB_PID) =="; \
		DISPLAY="$$XVFB_DISP" LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/tests/data/lsan-gl.supp" "$(X11_TEST_BIN)"; \
		TEST_RESULT=$$?; \
		if [ $$TEST_RESULT -eq 0 ]; then \
			echo "== running the uri-list clipboard rig =="; \
			DISPLAY="$$XVFB_DISP" LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/tests/data/lsan-gl.supp" "$(X11_URI_TEST_BIN)"; \
			TEST_RESULT=$$?; \
			if [ $$TEST_RESULT -eq 0 ]; then \
				echo "== running the G-2 thread suite (Xvfb) =="; \
				DISPLAY="$$XVFB_DISP" LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/tests/data/lsan-gl.supp" "$(THREAD_TEST_BIN)"; \
				TEST_RESULT=$$?; \
			fi; \
		fi; \
		kill "$$XVFB_PID" 2>/dev/null; \
		wait "$$XVFB_PID" 2>/dev/null; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		exit $$TEST_RESULT; \
	fi

# D-Bus live-bus integration test (1.0.0) — NOT part of plain
# `make test`. Wraps the test binary in dbus-run-session(1) (a real,
# throwaway dbus-daemon) and a throwaway Xvfb (the fdk_context the
# native client rides needs a display). Skips loudly when either is
# unavailable so packaging containers without dbus stay green; the
# ubuntu CI job sets FDK_DBUS_TEST_REQUIRE=1 to make skips fail.
DBUS_LIVE_SRC := tests/test_dbus_live.c
DBUS_LIVE_BIN := $(BUILD_DIR)/tests/test_dbus_live
DBUS_NOTIFY_BIN := $(BUILD_DIR)/tests/test_notify_client
DBUS_TRAY_BIN := $(BUILD_DIR)/tests/test_tray_item
# One session, every live test: each binary skips individually when it
# must, and REQUIRE knobs (FDK_DBUS_TEST_REQUIRE / FDK_TRAY_TEST_REQUIRE)
# make the skips failures where CI demands it.
# 1.0.0, C-1: the live test's portal-fallback leg opens the toolkit's
# own browser window — the GL lane's exit-time Mesa allocations ride
# it, same as the X11 suite (the suppression file is shared).
DBUS_LIVE_ALL := "LSAN_OPTIONS=\"suppressions=$(CURDIR)/tests/data/lsan-gl.supp\" $(DBUS_LIVE_BIN) && $(DBUS_NOTIFY_BIN) && $(DBUS_TRAY_BIN)"

test-dbus: $(DBUS_LIVE_BIN) $(DBUS_NOTIFY_BIN) $(DBUS_TRAY_BIN)
	@if ! command -v dbus-run-session >/dev/null 2>&1; then \
		echo "[skip] dbus-run-session not available"; exit 0; fi
	@if [ -z "$$DISPLAY" ]; then \
		XVFB_NUM=$$((74 + ($$$$ % 20))); \
		XVFB_DISP=":$$XVFB_NUM"; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		Xvfb "$$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdk-xvfb-dbus.log 2>&1 & \
		XVFB_PID=$$!; \
		sleep 2; \
		echo "== running D-Bus live test under dbus-run-session + Xvfb :$$XVFB_NUM =="; \
		DISPLAY="$$XVFB_DISP" dbus-run-session -- sh -c $(DBUS_LIVE_ALL); \
		TEST_RESULT=$$?; \
		kill "$$XVFB_PID" 2>/dev/null; \
		wait "$$XVFB_PID" 2>/dev/null; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		exit $$TEST_RESULT; \
	else \
		echo "== running D-Bus live test under dbus-run-session (DISPLAY=$$DISPLAY) =="; \
		dbus-run-session -- sh -c $(DBUS_LIVE_ALL); \
	fi

$(DBUS_LIVE_BIN): $(DBUS_LIVE_SRC) $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# text_demo screenshot rig (the §10 hand-equivalent, 1.0.0): what the
# owner's eyes verify on the real desktop — complex-script ink, joined
# Arabic, the Devanagari shirorekha, dense Han ideographs — proven
# mechanically under a throwaway Xvfb against examples/text_demo
# --screenshot. The glyph-level truth (fallback resolved, no glyph 0,
# contextual forms) is test_shaping.c's six-script matrix (plain
# `make test`); this target pins the PIXELS (shaped-vs-legacy
# differential + join anchors + a legacy negative control). Skips
# honestly (exit 0) when Xvfb/python3-PIL are absent or a script's
# font is not installed; the CI images install the per-script Noto
# packages so every leg runs there.
TEXTDEMO_BIN := $(BUILD_DIR)/examples/text_demo

test-textdemo: $(TEXTDEMO_BIN)
	@sh tests/textdemo_rig.sh

# Wayland platform integration test — NOT part of plain `make test`.
# Requires a RUNNING Wayland compositor reachable via $WAYLAND_DISPLAY
# (see docs/testing.md for the weston headless recipe; the maintainer
# rigs in the staging environment start one automatically). Without
# it the test binary self-skips with [skip], it does not fail.
WL_TEST_SRC := tests/test_wayland_integration.c
WL_TEST_BIN := $(BUILD_DIR)/tests/test_wayland_integration

test-wayland: $(WL_TEST_BIN)
	@if [ -n "$$WAYLAND_DISPLAY" ]; then \
		echo "== running Wayland integration test against WAYLAND_DISPLAY=$$WAYLAND_DISPLAY =="; \
		"$(WL_TEST_BIN)" || exit 1; \
	else \
		echo "== no WAYLAND_DISPLAY set; the test binary will self-skip =="; \
		"$(WL_TEST_BIN)"; \
	fi

$(WL_TEST_BIN): $(WL_TEST_SRC) \
                 src/platform/wayland/generated/input-method-unstable-v2-protocol.c \
                 $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       src/platform/wayland/generated/input-method-unstable-v2-protocol.c \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

$(X11_TEST_BIN): $(X11_TEST_SRC) $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	# -lXtst: the input-region click-through legs drive REAL XTEST pointer
	# events (S-4, rc2) — XSendEvent bypasses shapes and proves nothing.
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_DEFS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< $(STATIC_LIB) -o $@ $(LDFLAGS) -lXtst

# --- Examples ---------------------------------------------------------

examples: $(EXAMPLE_BINS)

$(BUILD_DIR)/examples/%: examples/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Tools (1.0.0) ------------------------------------------------------

tools: $(TOOL_BINS)

$(BUILD_DIR)/tools/%: tools/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Test applications (§24) --------------------------------------------
#
# The validation suite under test-apps/ — four real applications
# built ONLY against the public API (no -Isrc, no internal headers;
# enforced by their own Makefile). Not part of `all` or `test`:
# an on-demand audit surface, like examples.

test-apps:
	$(MAKE) -C test-apps

test-apps-smoke:
	$(MAKE) -C test-apps test-apps-smoke

.PHONY: test-apps test-apps-smoke

# --- Install ------------------------------------------------------------

# Performance baseline harness (Phase 11): NOT part of `make test`
# (machine-dependent numbers); builds and runs a timing report.
# Bench builds against a dedicated RELEASE object tree
# (build/obj-rel + build/libfdk-rel.a): the default build is
# ASan+UBSan-instrumented, and sanitizer overhead would distort the
# numbers by multiples. The release flags match `make release`.
REL_LIB := $(BUILD_DIR)/libfdk-rel.a

$(BUILD_DIR)/obj-rel/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(STD) $(WARN) $(FEATURE) $(REL_FLAGS) \
	    $(WAYLAND_DEFS) $(call extra_flags,$<) -MMD -MP -c $< -o $@

$(REL_LIB): $(patsubst src/%.c,$(BUILD_DIR)/obj-rel/%.o,$(LIB_SRCS))
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD_DIR)/bench: tests/bench.c $(REL_LIB)
	$(CC) $(STD) $(WARN) $(FEATURE) $(REL_FLAGS) -Iinclude -Isrc \
	    tests/bench.c -o $@ $(REL_LIB) $(LDFLAGS)

bench: $(BUILD_DIR)/bench
	$(BUILD_DIR)/bench

# pkg-config file: generated from fdk.pc.in so the version comes
# from fdk_version.h, never from a second hand-edited copy. The
# prefix-side stamp matters: `make install PREFIX=/custom` after a
# default-prefix build must REGENERATE the .pc, not silently install
# one that points applications at the old prefix (command-line
# variables do not invalidate file targets on their own — the same
# lesson as the config stamp, scoped to the one file that cares).
$(BUILD_DIR)/fdk.pc: fdk.pc.in include/fdk/fdk_version.h Makefile \
                     $(BUILD_DIR)/fdk.pc.prefix
	@mkdir -p $(BUILD_DIR)
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCDIR@|$(INCDIR)|g' \
	     -e 's|@VERSION@|$(FDK_PC_VERSION)|g' \
	     -e 's|@PC_LIBS@|$(PC_LIBS)|g' fdk.pc.in > $@

# Rewritten only when PREFIX/LIBDIR/INCDIR actually change, so
# same-prefix installs stay incremental (the CONFIG_STAMP pattern).
$(BUILD_DIR)/fdk.pc.prefix: FORCE
	@mkdir -p $(BUILD_DIR)
	@cur=`cat $@ 2>/dev/null || echo none`; \
	if [ x"$$cur" != x"prefix=$(PREFIX)|libdir=$(LIBDIR)|incdir=$(INCDIR)" ]; then \
		printf '%s\n' 'prefix=$(PREFIX)|libdir=$(LIBDIR)|incdir=$(INCDIR)' > $@; \
	fi

install: all $(BUILD_DIR)/fdk.pc
	install -d $(DESTDIR)$(INCDIR)/fdk
	install -m 644 include/fdk/*.h $(DESTDIR)$(INCDIR)/fdk/
	mkdir -p $(DESTDIR)$(INCDIR)/fdk/ext
	install -m 644 include/fdk/ext/*.h $(DESTDIR)$(INCDIR)/fdk/ext/
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/
# Versioned shared-library install (the soname scheme, active since
# the public 1.0.0 decision of 2026-10-12): the real file carries
# the numeric MAJOR.MINOR.PATCH (FDK_SO_VERSION — suffixes never
# appear in the .so filename, see the derivation block above), the
# SONAME symlink is what executables record
# as NEEDED, and the unversioned libfdk.so is the development
# symlink pkg-config's -lfdk resolves against.
	install -m 755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/libfdk.so.$(FDK_SO_VERSION)
	ln -sf libfdk.so.$(FDK_SO_VERSION) $(DESTDIR)$(LIBDIR)/$(FDK_SONAME)
	ln -sf $(FDK_SONAME) $(DESTDIR)$(LIBDIR)/libfdk.so
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 644 $(BUILD_DIR)/fdk.pc \
		               $(DESTDIR)$(LIBDIR)/pkgconfig/fdk.pc
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TOOL_BINS) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(DATADIR)/fdk/.FDKThemes
	install -m 644 $(SHIPPED_THEMES) $(DESTDIR)$(DATADIR)/fdk/.FDKThemes/

uninstall:
	rm -rf $(DESTDIR)$(INCDIR)/fdk
	rm -f $(DESTDIR)$(LIBDIR)/libfdk.a
	rm -f $(DESTDIR)$(LIBDIR)/libfdk.so
	rm -f $(DESTDIR)$(LIBDIR)/$(FDK_SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/libfdk.so.$(FDK_SO_VERSION)
	rm -f $(DESTDIR)$(LIBDIR)/pkgconfig/fdk.pc
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(notdir $(TOOL_BINS)))
	rm -rf $(DESTDIR)$(DATADIR)/fdk

clean:
	rm -rf $(BUILD_DIR)

# Included LAST on purpose: -MMD dependency files each start with a
# rule, and an -include before any real target would make the first
# .d's object the default goal (make pitfall; hit during the Phase 8
# session). After every target is defined, including them is inert.
-include $(DEPS)

# Wayland GLES GPU paint smoke — the GLES-only lane under a headless
# compositor (WLR_BACKENDS=headless sway, or any weston). Self-skips
# without WAYLAND_DISPLAY.
WL_GPU_TEST_BIN := $(BUILD_DIR)/tests/test_gpu_paint_wl

$(WL_GPU_TEST_BIN): tests/test_gpu_paint_wl.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

test-gpu-paint-wl: $(WL_GPU_TEST_BIN)
	@if [ -n "$$WAYLAND_DISPLAY" ]; then \
	    echo "== GLES-only GPU paint smoke (wayland lane) =="; \
	    "$(WL_GPU_TEST_BIN)" || exit 1; \
	else \
	    echo "== no WAYLAND_DISPLAY; self-skip =="; \
	    "$(WL_GPU_TEST_BIN)"; \
	fi

# Cross-section evidence tool (P1 close-out): CPU-vs-GPU coverage
# profiles for one circle edge and one rotated edge, with the
# shift analysis that separates a geometry offset from the AA model.
XSEC_BIN := $(BUILD_DIR)/tests/crosssection

$(XSEC_BIN): tests/crosssection.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS) -lm

test-crosssection: $(XSEC_BIN)
	"$(XSEC_BIN)"

# DPI/scale evidence (P1 close-out): X11 (Xvfb at 96 and 192 dpi) and
# Wayland (the sway headless rig with output scale 2).
DPI_BIN := $(BUILD_DIR)/tests/test_dpi_scale

$(DPI_BIN): tests/test_dpi_scale.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

test-dpi-x11: $(DPI_BIN)
	@pkill -x Xvfb 2>/dev/null; sleep 1; set -e; N=$$((90 + RANDOM % 400)); rm -f /tmp/.X11-unix/X$$N; \
	Xvfb :$$N -screen 0 800x600x24 >/dev/null 2>&1 & XP=$$!; sleep 1; \
	DISPLAY=:$$N FDK_GL_FORCE=1 LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-gl.supp "$(DPI_BIN)" x11 1; rc=$$?; kill $$XP 2>/dev/null; wait $$XP 2>/dev/null; \
	if [ $$rc -eq 1 ]; then exit 1; fi; \
	sleep 1; N=$$((90 + RANDOM % 400)); rm -f /tmp/.X11-unix/X$$N; \
	Xvfb :$$N -screen 0 800x600x24 -dpi 192 >/dev/null 2>&1 & XP=$$!; sleep 1; \
	DISPLAY=:$$N FDK_GL_FORCE=1 LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-gl.supp "$(DPI_BIN)" x11 2; rc=$$?; kill $$XP 2>/dev/null; \
	if [ $$rc -eq 1 ]; then exit 1; fi

test-dpi-wayland: $(DPI_BIN)
	@if [ -z "$$WAYLAND_DISPLAY" ]; then echo "needs a compositor (the sway rig)"; exit 1; fi
	WAYLAND_DISPLAY=$$WAYLAND_DISPLAY FDK_GL_FORCE=1 \
	LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-gl.supp \
	"$(DPI_BIN)" wayland 2

# P5 proof examples — built against an INSTALLED FDK (make install
# first; no internal include paths), public API only. (a) needs a
# layer-shell compositor (the sway rig); (b)(c)(e) run under Xvfb.
EX_PREFIX ?= /tmp/fdk-inst

$(BUILD_DIR)/examples/%: examples/%.c | $(EX_PREFIX)/lib/libfdk.a
	@mkdir -p $(dir $@)
	$(CC) -I$(EX_PREFIX)/include $< -L$(EX_PREFIX)/lib -lfdk -lm -ldl -lpthread -o $@

$(EX_PREFIX)/lib/libfdk.a:
	make install PREFIX=$(EX_PREFIX)

.PHONY: test-examples
test-examples: $(BUILD_DIR)/examples/gl-canvas $(BUILD_DIR)/examples/custom-widget $(BUILD_DIR)/examples/worker-post $(BUILD_DIR)/examples/panel-layer-shell
	@set -e; N=$$((90 + RANDOM % 400)); rm -f /tmp/.X11-unix/X$$N; \
	Xvfb :$$N -screen 0 800x600x24 >/dev/null 2>&1 & XP=$$!; sleep 1; \
	LD_LIBRARY_PATH=$(EX_PREFIX)/lib DISPLAY=:$$N \
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/examples/gl-canvas; \
	LD_LIBRARY_PATH=$(EX_PREFIX)/lib DISPLAY=:$$N \
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/examples/custom-widget; \
	LD_LIBRARY_PATH=$(EX_PREFIX)/lib DISPLAY=:$$N \
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/examples/worker-post; \
	kill $$XP 2>/dev/null; \
	echo "examples (a) runs under a layer-shell compositor: see test-examples-wayland"

.PHONY: test-examples-wayland
test-examples-wayland: $(BUILD_DIR)/examples/panel-layer-shell
	@if [ -z "$$WAYLAND_DISPLAY" ]; then echo "needs a compositor"; exit 1; fi
	LD_LIBRARY_PATH=$(EX_PREFIX)/lib WAYLAND_DISPLAY=$$WAYLAND_DISPLAY \
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/examples/panel-layer-shell

# X11 share-root evidence (P2): two GPU windows exchange a texture
# through the lane's share group (the fdk_ext_gl contract).
XSHARE_BIN := $(BUILD_DIR)/tests/test_x11_share

$(XSHARE_BIN): tests/test_x11_share.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

test-x11-share: $(XSHARE_BIN)
	@pkill -x Xvfb 2>/dev/null; sleep 1; set -e; N=$$((90 + RANDOM % 400)); \
	rm -f /tmp/.X11-unix/X$$N; Xvfb :$$N -screen 0 800x600x24 >/dev/null 2>&1 & XP=$$!; sleep 1; \
	DISPLAY=:$$N FDK_GL_FORCE=1 LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-gl.supp \
	"$(XSHARE_BIN)"; rc=$$?; kill $$XP 2>/dev/null; wait $$XP 2>/dev/null; \
	if [ $$rc -eq 1 ]; then exit 1; fi

# Present arbitration evidence (P2): an add-on GL surface alongside
# CPU and HYBRID windows in one context.
ARB_BIN := $(BUILD_DIR)/tests/test_arbitration

$(ARB_BIN): tests/test_arbitration.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

test-arbitration: $(ARB_BIN)
	@pkill -x Xvfb 2>/dev/null; sleep 1; set -e; N=$$((90 + RANDOM % 400)); \
	rm -f /tmp/.X11-unix/X$$N; Xvfb :$$N -screen 0 800x600x24 >/dev/null 2>&1 & XP=$$!; sleep 1; \
	DISPLAY=:$$N FDK_GL_FORCE=1 LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-gl.supp \
	"$(ARB_BIN)"; rc=$$?; kill $$XP 2>/dev/null; wait $$XP 2>/dev/null; \
	if [ $$rc -eq 1 ]; then exit 1; fi

# P3 protocol surface (the sway rig + the honest X11 negatives).
P3_BIN := $(BUILD_DIR)/tests/test_p3_protocols

$(P3_BIN): tests/test_p3_protocols.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

test-p3-x11: $(P3_BIN)
	@pkill -x Xvfb 2>/dev/null; sleep 1; set -e; N=$$((90 + RANDOM % 400)); \
	rm -f /tmp/.X11-unix/X$$N; Xvfb :$$N -screen 0 800x600x24 >/dev/null 2>&1 & XP=$$!; sleep 1; \
	DISPLAY=:$$N "$(P3_BIN)" x11; rc=$$?; kill $$XP 2>/dev/null; wait $$XP 2>/dev/null; \
	if [ $$rc -eq 1 ]; then exit 1; fi

test-p3-wayland: $(P3_BIN)
	@if [ -z "$$WAYLAND_DISPLAY" ]; then echo "needs a compositor (the sway rig)"; exit 1; fi
	WAYLAND_DISPLAY=$$WAYLAND_DISPLAY "$(P3_BIN)" wayland

# P5 (d)/(f): the protocol add-on examples — built against the
# INSTALLED tree with public headers only (make install first).
EX_PROTOCOLS_DIR := examples/protocols

.PHONY: test-examples-protocols
test-examples-protocols:
	@if [ ! -d "$(EX_PROTOCOLS_DIR)" ]; then echo "no vendored protocol headers"; exit 1; fi
	@set -e; P=$(CURDIR)/build/fdk-install; \
	$(MAKE) install PREFIX=$$P > /dev/null; \
	WL_CFLAGS=$$(pkg-config --cflags wayland-client 2>/dev/null || echo -I/usr/include); \
	WL_LIBS=$$(pkg-config --libs wayland-client 2>/dev/null || echo -lwayland-client); \
	WL_LIBDIR=$$(pkg-config --variable=libdir wayland-client 2>/dev/null || echo /usr/lib); \
	mkdir -p $(BUILD_DIR)/examples; \
	$(CC) $$WL_CFLAGS -I$$P/include -Iexamples examples/d-input-method.c -L$$P/lib -L$$WL_LIBDIR $$WL_LIBS -lfdk -lm -ldl -lpthread -o $(BUILD_DIR)/examples/d-input-method; \
	$(CC) $$WL_CFLAGS -I$$P/include -Iexamples examples/f-session-lock-raw.c -L$$P/lib -L$$WL_LIBDIR $$WL_LIBS -lfdk -lwayland-client -lm -ldl -lpthread -o $(BUILD_DIR)/examples/f-session-lock-raw; \
	$(CC) $$WL_CFLAGS -I$$P/include -Iexamples examples/d-input-method-raw.c -L$$P/lib -L$$WL_LIBDIR $$WL_LIBS -lfdk -lwayland-client -lm -ldl -lpthread -o $(BUILD_DIR)/examples/d-input-method-raw; \
	$(CC) $$WL_CFLAGS -I$$P/include -Iexamples examples/f-session-lock.c -L$$P/lib -L$$WL_LIBDIR $$WL_LIBS -lfdk -lm -ldl -lpthread -o $(BUILD_DIR)/examples/f-session-lock; \
	$(CC) $$WL_CFLAGS -I$$P/include -Iexamples examples/d-input-method.c -L$$P/lib -L$$WL_LIBDIR $$WL_LIBS -lfdk -lwayland-client -lm -ldl -lpthread -o $(BUILD_DIR)/examples/d-input-method; \
	$(CC) $$WL_CFLAGS -I$$P/include -Iexamples examples/h-status-panel.c -L$$P/lib -L$$WL_LIBDIR $$WL_LIBS -lfdk -lm -ldl -lpthread -o $(BUILD_DIR)/examples/h-status-panel; \
	$(CC) $$WL_CFLAGS -I$$P/include -Iexamples examples/g-dmabuf-video.c -L$$P/lib -L$$WL_LIBDIR $$WL_LIBS -lfdk -lm -ldl -lpthread -o $(BUILD_DIR)/examples/g-dmabuf-video; \
	echo "examples (d)/(f) built against the installed tree"

# P4: the capability x backend x API matrix (both lanes) + the export-map gate.
MATRIX_BIN := $(BUILD_DIR)/tests/test_matrix

$(MATRIX_BIN): tests/test_matrix.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

test-matrix-x11: $(MATRIX_BIN)
	@pkill -x Xvfb 2>/dev/null; sleep 1; set -e; N=$$((90 + RANDOM % 400)); \
	rm -f /tmp/.X11-unix/X$$N; Xvfb :$$N -screen 0 800x600x24 >/dev/null 2>&1 & XP=$$!; sleep 1; \
	DISPLAY=:$$N "$(MATRIX_BIN)" x11; rc=$$?; kill $$XP 2>/dev/null; wait $$XP 2>/dev/null; \
	if [ $$rc -eq 1 ]; then exit 1; fi

test-matrix-wayland: $(MATRIX_BIN)
	@set -e; test -n "$$WAYLAND_DISPLAY" || { echo "needs a compositor"; exit 1; }; \
	"$(MATRIX_BIN)" wayland

test-exports: build/libfdk.so
	@set -e; nm -D --defined-only build/libfdk.so | awk '$$2 ~ /T/ {n=$$3; sub(/@@.*/, "", n); print n}' | sort > $(BUILD_DIR)/exports.actual; \
	awk '/local:/{g=0} /global:/{g=1} g && /;/ {gsub(/;/,""); gsub(/^[ \t]+|[ \t]+$$/,""); if ($$0 != "" && index($$0,"*")==0) print}' build/libfdk.exports.map | sort -u > $(BUILD_DIR)/exports.mapped; \
	comm -23 $(BUILD_DIR)/exports.actual $(BUILD_DIR)/exports.mapped > $(BUILD_DIR)/exports.leaked; \
	if [ -s $(BUILD_DIR)/exports.leaked ]; then echo "EXPORT LEAKS:"; cat $(BUILD_DIR)/exports.leaked; exit 1; fi; \
	echo "export map: no leaks"

# P4: the dma-buf import (validation runs everywhere; the round-trip
# SKIPs without a DRM device and never fakes a PASS).
DMABUF_BIN := $(BUILD_DIR)/tests/test_dmabuf

$(DMABUF_BIN): tests/test_dmabuf.c $(STATIC_LIB) $(ALL_FDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< \
	       $(STATIC_LIB) -o $@ $(LDFLAGS)

test-dmabuf: $(DMABUF_BIN)
	@ASAN_OPTIONS=detect_leaks=0 "$(DMABUF_BIN)"
