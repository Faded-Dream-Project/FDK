# Makefile — fdk-shell (Faded Dream ToolKit layer-shell add-on)
#
# fdk-shell is a SEPARATE package that builds on FDK's installed
# public API (fdk/extend.h, FDK 1.1.0) — own version, own build, own
# pkg-config file, ready to move to its own repository. The sibling
# FDK checkout (../) is the default dependency; override with
# FDK_CFLAGS / FDK_LIBS (tests/isolation.sh installs FDK into a
# throwaway prefix and builds this package against ONLY that — the
# no-internals proof).
#
# Targets:
#   make / make all   build the static + shared library (debug)
#   make release      optimized + NDEBUG
#   make install      install to PREFIX (default /usr/local)
#   make test         build and run the X11 backend tests (needs a
#                     display; starts a throwaway Xvfb like FDK's
#                     own test-x11)
#   make test-wayland run the Wayland tests against $WAYLAND_DISPLAY
#                     (self-skips without one)
#   make isolation    the prefix-isolation build proof (installs FDK
#                     into build/isolation-prefix, rebuilds fdk-shell
#                     against it, links the demo, asserts no internal
#                     includes and a clean dynamic dependency)
#   make examples     build the demo bar
#   make clean
#
# Backend knobs:
#   FDKSHELL_DISABLE_WAYLAND=1 — build without the Wayland backend
#                                 (for machines without wayland-client
#                                 dev files; X11 only)
#   FDKSHELL_DISABLE_X11=1     — Wayland only (rare)

CC       ?= gcc
AR       ?= ar
PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
BINDIR   ?= $(PREFIX)/bin

# Version for the pkg-config file and the installed library name;
# derived from the header (the single source of truth, the same one
# fdk-shell/packaging/ci/common.sh reads) — never from a second
# hand-edited copy. 0.2.0 taught this the hard way: the recipes said
# 0.2.0 while this line still said 0.1.0, and every packaging
# install-test caught the .so.0.1.0 / Version: 0.1.0 mismatch.
FDKSHELL_VERSION := $(shell sed -n 's/^#define FDKSHELL_VERSION_STRING "\(.*\)"/\1/p' include/fdk-shell/fdk-shell.h)
# SONAME major, hand-set: it changes only on an ABI break (the
# exports map is append-only within a major).
FDKSHELL_SONAME  := libfdk-shell.so.0

BUILD_DIR := build

STD  := -std=c17
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
        -Wmissing-prototypes -Wconversion -Wsign-conversion \
        -Wcast-qual -Wpointer-arith -Wundef -Wwrite-strings
FEATURE := -D_POSIX_C_SOURCE=200809L

CONFIG     := debug

# Sanitizers by default (the same battery FDK itself runs under);
# RELEASE drops them (FDK's own convention: release = -O2 -DNDEBUG
# with no sanitizer runtimes linked into the shipped .so -- the
# packaging NEEDED scan fails loudly on any leak of them). An
# explicit FDKSHELL_SANITIZE=0/1 on the command line still wins.
FDKSHELL_SANITIZE ?= $(if $(filter release,$(CONFIG)),0,1)
ifeq ($(FDKSHELL_SANITIZE),0)
SAN_FLAGS :=
else
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
endif
CONFIG_STAMP := $(BUILD_DIR)/.config
DBG_FLAGS  := -g -O0 -DFDKSHELL_DEBUG_BUILD=1
REL_FLAGS  := -O2 -DNDEBUG
CFLAGS_CONFIG := $(DBG_FLAGS)
ifeq ($(CONFIG),release)
CFLAGS_CONFIG := $(REL_FLAGS)
endif

# ---- the FDK dependency ---------------------------------------------------
#
# Default: the sibling checkout (this package lives in FDK's tree as
# a separate directory with its own build). Overrides are how the
# isolation proof points the build at an INSTALLED FDK — every FDK
# include and link goes through these two variables, nowhere else.
FDK_ROOT    ?= ..
FDK_CFLAGS  ?= -I$(FDK_ROOT)/include
FDK_LIBS    ?= $(FDK_ROOT)/build/libfdk.a -lX11 -lXext -ldl $(WL_DEPS_L) -lxkbcommon -lm -lz

# Cross-package prerequisites (found live 2026-10-26, the W1-W3 chase):
# the sibling FDK tree's artifacts are LINK INPUTS for this package's
# binaries — every rule that links them must also DEPEND on them, or
# an FDK rebuild leaves stale binaries behind (the rigs measured a
# bar binary three patch-sets old this way, silently invalidating two
# rounds of diagnosis). FDK's own archive name is stable.
FDK_ARCHIVE := $(FDK_ROOT)/build/libfdk.a
FDK_SHARED  := $(FDK_ROOT)/build/libfdk.so
# GATED to the sibling layout (2026-10-26, CI run 40298760190):
# the isolation proof and every packaging family override
# FDK_LIBS / FDK_SO_LIBS to an INSTALLED FDK (pkg-config or a
# throwaway prefix) — there the sibling artifacts are NOT link
# inputs and must not be prerequisites, and in the pristine
# fdk-shell-only tarball families (rpm/alpine/arch) ../build
# does not exist at all: make died with "No rule to make
# target '../build/libfdk.so'". The gate keys on the EXPANDED
# link inputs, so it follows whatever the caller set — the
# defaults keep the discipline, any override steps it aside.
# (=, deferred, on purpose: FDK_SO_LIBS is ?=-assigned further
# below — a := here expanded an empty FDK_SO_LIBS and silently
# dropped the shared library prerequisite; the gate must judge
# the CALLER final value: default, command line, or environment.)
FDK_LINK_PREREQS = $(if $(findstring $(FDK_ROOT)/build,$(FDK_LIBS)),$(FDK_ARCHIVE),)
FDK_SO_PREREQ    = $(if $(findstring $(FDK_ROOT)/build,$(FDK_SO_LIBS)),$(FDK_SHARED),)
# The SHARED library links FDK's SHARED library (NEEDED: libfdk.so.1)
# -- never the static archive (embedding FDK would defeat the
# separate-runtime license model AND the add-on isolation contract).
# Every consumer therefore gets the whole chain resolved by the
# loader: ldd shows libfdk-shell.so.0 -> libfdk.so.1, and the distro
# package tools derive the real runtime dependency from the ELF.
# The default points at the sibling checkout's build tree; the
# isolation/packaging flows override it with the installed -lfdk.
FDK_SO_LIBS ?= -L$(FDK_ROOT)/build -lfdk
# The backends' native dependencies (fdk-shell's OWN — see README's
# dependency note: the extension API's native accessors hand out the
# connection; speaking the native protocol is this package's right).
X11_DEPS    := $(shell pkg-config --cflags --libs x11 2>/dev/null || echo -lX11)
# XTest (the popup-grab test's fake-input channel; test-link only
# would suffice, but the discovery is shared and harmless). The HEADER
# ships only with the Xtst dev package — libxtst-dev (deb/apk),
# libXtst-devel (rpm/void), libxtst (arch); every CI container
# pre-installs it as test infrastructure (the libx11-dev class).
# No XWarpPointer substitute exists here: synthesizing a KEY is what
# activates a passive grab, and XSendEvent delivery cannot — so
# without the header the test build fails at <X11/extensions/XTest.h>,
# naming its own remedy.
X11_TEST_DEPS := $(shell pkg-config --cflags --libs xtst 2>/dev/null || echo -lXtst)
WL_DEPS_C   := $(shell pkg-config --cflags wayland-client 2>/dev/null)
WL_DEPS_L   := $(shell pkg-config --libs wayland-client 2>/dev/null || echo -lwayland-client)

# ---- backends ---------------------------------------------------------------

BUILD_WAYLAND := 1
ifeq ($(FDKSHELL_DISABLE_WAYLAND),1)
  BUILD_WAYLAND := 0
endif
BUILD_X11 := 1
ifeq ($(FDKSHELL_DISABLE_X11),1)
  BUILD_X11 := 0
endif

# Two vendored protocols: wlr-layer-shell (the point of the package)
# and xdg-shell (the generated layer-shell code references
# xdg_popup_interface for get_popup's argument table, and FDK keeps
# its own copy private — an add-on vendors what it speaks; this is
# the standard multi-protocol client shape).
WL_PROTO_SRC := src/wl/protocol/wlr-layer-shell-unstable-v1.xml
WL_PROTO_HDR := src/wl/protocol/wlr-layer-shell-unstable-v1-client-protocol.h
WL_PROTO_C   := src/wl/protocol/wlr-layer-shell-unstable-v1-protocol.c
XDG_PROTO_SRC := src/wl/protocol/xdg-shell.xml
XDG_PROTO_HDR := src/wl/protocol/xdg-shell-client-protocol.h
XDG_PROTO_C   := src/wl/protocol/xdg-shell-protocol.c

SRCS := src/shell.c
ifeq ($(BUILD_X11),1)
  SRCS += src/x11/x11.c
  DEFS += -DFDKSHELL_BUILD_X11=1
  BACKEND_CFLAGS += $(X11_DEPS)
  BACKEND_LIBS += $(X11_DEPS)
endif
ifeq ($(BUILD_WAYLAND),1)
  SRCS += src/wl/wl.c $(WL_PROTO_C) $(XDG_PROTO_C)
  DEFS += -DFDKSHELL_BUILD_WAYLAND=1
  # -Wno-cast-qual: the generated protocol inlines (and
  # wayland-client.h itself) cast away const in their listener
  # installers — the same suppression FDK's own Wayland backend
  # carries; it applies to this package's wayland-consuming build,
  # never to the package's own logic (x11-only builds keep the full
  # warning set).
  BACKEND_CFLAGS += $(WL_DEPS_C) -Wno-cast-qual
  BACKEND_LIBS += $(WL_DEPS_L)
endif

OBJS     := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(SRCS))
OBJS_PIC := $(patsubst src/%.c,$(BUILD_DIR)/obj-pic/%.o,$(SRCS))

STATIC_LIB := $(BUILD_DIR)/libfdk-shell.a
SHARED_LIB := $(BUILD_DIR)/libfdk-shell.so

TEST_BINS := $(BUILD_DIR)/tests/test_x11 $(BUILD_DIR)/tests/test_wl
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/examples/%,$(wildcard examples/*.c))

.PHONY: all release static shared install uninstall test test-wayland test-x11 test-bar-rig test-notifyd test-tray isolation examples clean regen-protocol

all: static shared examples

release:
	$(MAKE) CONFIG=release all

static: $(STATIC_LIB)
shared: $(SHARED_LIB)

$(CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@cur=`cat $@ 2>/dev/null || echo none`; \
	if [ x"$$cur" != x"$(CONFIG)-wl$(BUILD_WAYLAND)-x11$(BUILD_X11)-san$(FDKSHELL_SANITIZE)" ]; then \
		rm -f $(BUILD_DIR)/.config-*; \
		printf '%s\n' '$(CONFIG)-wl$(BUILD_WAYLAND)-x11$(BUILD_X11)-san$(FDKSHELL_SANITIZE)' > $@; \
	fi

.PHONY: FORCE
FORCE:

$(BUILD_DIR)/obj/%.o: src/%.c $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) $(FEATURE) $(CFLAGS_CONFIG) $(SAN_FLAGS) \
	    $(DEFS) -Iinclude -Isrc $(FDK_CFLAGS) $(BACKEND_CFLAGS) \
	    -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj-pic/%.o: src/%.c $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) $(FEATURE) $(CFLAGS_CONFIG) $(SAN_FLAGS) \
	    -fPIC $(DEFS) -Iinclude -Isrc $(FDK_CFLAGS) $(BACKEND_CFLAGS) \
	    -MMD -MP -c $< -o $@

$(STATIC_LIB): $(OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

EXPORT_MAP := src/libfdk-shell.exports.map

$(SHARED_LIB): $(OBJS_PIC) $(EXPORT_MAP) $(FDK_SO_PREREQ)
	@mkdir -p $(dir $@)
	$(CC) $(SAN_FLAGS) -fPIC -shared \
	    -Wl,-soname,$(FDKSHELL_SONAME) \
	    -Wl,--version-script=$(EXPORT_MAP) -o $@ $(OBJS_PIC) \
	    $(FDK_SO_LIBS) $(BACKEND_LIBS)

# Regenerate the wayland bindings when a scanner is present; the
# generated files are COMMITTED so the package builds without one
# (the same discipline FDK itself uses for xdg-shell).
regen-protocol: $(WL_PROTO_SRC) $(XDG_PROTO_SRC)
	@if command -v wayland-scanner >/dev/null 2>&1; then \
		wayland-scanner client-header $(WL_PROTO_SRC) $(WL_PROTO_HDR); \
		wayland-scanner private-code $(WL_PROTO_SRC) $(WL_PROTO_C); \
		wayland-scanner client-header $(XDG_PROTO_SRC) $(XDG_PROTO_HDR); \
		wayland-scanner private-code $(XDG_PROTO_SRC) $(XDG_PROTO_C); \
		echo "regenerated the vendored protocol bindings"; \
	else \
		echo "wayland-scanner not found — using the committed bindings"; \
	fi

# ---- tests -------------------------------------------------------------------

test: test-x11

$(BUILD_DIR)/tests/test_x11: tests/test_x11.c $(STATIC_LIB) $(FDK_LINK_PREREQS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) $(FEATURE) $(CFLAGS_CONFIG) $(SAN_FLAGS) \
	    -Iinclude -Isrc $(FDK_CFLAGS) $(BACKEND_CFLAGS) \
	    $(X11_TEST_DEPS) $< $(STATIC_LIB) \
	    $(FDK_LIBS) $(BACKEND_LIBS) $(X11_TEST_DEPS) -o $@

$(BUILD_DIR)/tests/test_wl: tests/test_wl.c $(STATIC_LIB) $(FDK_LINK_PREREQS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) $(FEATURE) $(CFLAGS_CONFIG) $(SAN_FLAGS) \
	    -Iinclude -Isrc $(FDK_CFLAGS) $(BACKEND_CFLAGS) $< $(STATIC_LIB) \
	    $(FDK_LIBS) $(BACKEND_LIBS) -o $@

# The GL-lane suppression FDK's own X11 suite runs under (the same
# libGLX_mesa one-time allocations, suppressed by path — see FDK's
# tests/data/lsan-gl.supp; this package points at the sibling copy).
LSAN_SUPP := ../tests/data/lsan-gl.supp

test-x11: $(BUILD_DIR)/tests/test_x11
	@if [ -n "$$DISPLAY" ]; then \
		echo "== running fdk-shell X11 tests against DISPLAY=$$DISPLAY =="; \
		LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/$(LSAN_SUPP)" "$(BUILD_DIR)/tests/test_x11" || exit 1; \
	else \
		echo "== no DISPLAY set, starting a throwaway Xvfb =="; \
		XVFB_NUM=$$((90 + ($$$$ % 400))); \
		XVFB_DISP=":$$XVFB_NUM"; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		Xvfb "$$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdkshell-xvfb.log 2>&1 & \
		XVFB_PID=$$!; \
		sleep 2; \
		echo "== running fdk-shell X11 tests against $$XVFB_DISP (pid $$XVFB_PID) =="; \
		DISPLAY="$$XVFB_DISP" LSAN_OPTIONS="$${LSAN_OPTIONS:+$$LSAN_OPTIONS }suppressions=$(CURDIR)/$(LSAN_SUPP)" "$(BUILD_DIR)/tests/test_x11"; \
		TEST_RESULT=$$?; \
		kill "$$XVFB_PID" 2>/dev/null; \
		wait "$$XVFB_PID" 2>/dev/null; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		exit $$TEST_RESULT; \
	fi

test-wayland: $(BUILD_DIR)/tests/test_wl
	@if [ -n "$$WAYLAND_DISPLAY" ]; then \
		echo "== running fdk-shell Wayland tests against WAYLAND_DISPLAY=$$WAYLAND_DISPLAY =="; \
		"$(BUILD_DIR)/tests/test_wl" || exit 1; \
	else \
		echo "== no WAYLAND_DISPLAY; the test binary will self-skip =="; \
		"$(BUILD_DIR)/tests/test_wl"; \
	fi

# test-bar-rig — the REAL-COMPOSITOR bar integration rig (1.1.0-rc1):
# the actual examples/bar against headless sway AND labwc (when each
# is runnable): alive for seconds, pixels on the output, the
# WAYLAND_DEBUG trace clean (configure arrived, no protocol error, no
# attach before the first ack_configure), and the compositor-kill leg
# (fatal dispatch logged once, non-zero exit, no busy-loop). Needs
# the bar built (make examples) and a compositor on PATH; self-skips
# per-compositor otherwise.
test-bar-rig: examples
	@bash tests/bar_rig.sh 4

# test-notifyd -- the notification daemon's full integration rig
# (1.1.0, S-2/A-12): protocol legs against a REAL dbus-daemon with
# dbus-send (libdbus) and gdbus (GLib) as the cross-implementation
# callers and gdbus monitor as the signal receiver; hostile-input and
# rate-limit batteries through the probe; the name-etiquette matrix
# (decline / --replace handover / refusal); X11 pixel + XTEST-click
# + EWMH legs under Xvfb (openbox when present); Wayland layer-shell
# legs under headless sway. The daemon runs sanitized with the LSan
# suppressions. Needs dbus, Xvfb, python3-PIL; self-skips otherwise
# (NOTIFYD_RIG_REQUIRE=1 makes a skip fatal -- CI does).
NOTIFYD_PROBE := $(BUILD_DIR)/tests/notifyd_probe

$(NOTIFYD_PROBE): tests/notifyd_probe.c $(STATIC_LIB) $(FDK_LINK_PREREQS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) $(FEATURE) $(CFLAGS_CONFIG) $(SAN_FLAGS) \
	    $(FDK_CFLAGS) $< $(STATIC_LIB) \
	    $(FDK_LIBS) $(BACKEND_LIBS) -o $@

test-notifyd: examples $(NOTIFYD_PROBE)
	@bash tests/notifyd_rig.sh 8

# test-tray — the StatusNotifier tray host's full integration rig
# (1.1.0, S-1/A-11): protocol legs against a REAL dbus-daemon with
# dbus-send (libdbus) and gdbus (GLib) as the cross-implementation
# callers and gdbus monitor as the signal receiver; the probe — a
# synthetic StatusNotifierItem with the dual-resolution a(iiay) icon
# and a full dbusmenu tree — drives the host end to end; hostile
# registration and etiquette matrices; X11 pixel + XTEST-click +
# menu-cascade + Passive/NeedsAttention legs under Xvfb (openbox
# when present); Wayland layer-shell legs under headless sway. The
# host runs sanitized with the LSan suppressions. Needs dbus, Xvfb,
# python3-PIL; self-skips otherwise (TRAY_RIG_REQUIRE=1 makes a skip
# fatal -- CI does).
TRAY_PROBE := $(BUILD_DIR)/tests/trayhost_probe

$(TRAY_PROBE): tests/trayhost_probe.c $(STATIC_LIB) $(FDK_LINK_PREREQS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) $(FEATURE) $(CFLAGS_CONFIG) $(SAN_FLAGS) \
	    $(FDK_CFLAGS) $< $(STATIC_LIB) \
	    $(FDK_LIBS) $(BACKEND_LIBS) -o $@

test-tray: examples $(TRAY_PROBE)
	@bash tests/trayhost_rig.sh 8

# The prefix-isolation proof (the HARD REQUIREMENT of the add-on
# contract): FDK is installed into a throwaway prefix, this package
# is rebuilt against that prefix ALONE, and the checks assert the
# boundary held.
isolation: tests/isolation.sh
	bash tests/isolation.sh

# ---- examples ------------------------------------------------------------------

examples: $(EXAMPLE_BINS)

$(BUILD_DIR)/examples/%: examples/%.c $(STATIC_LIB) $(FDK_LINK_PREREQS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) $(FEATURE) $(CFLAGS_CONFIG) $(SAN_FLAGS) \
	    -Iinclude -Isrc $(FDK_CFLAGS) $(BACKEND_CFLAGS) $< $(STATIC_LIB) \
	    $(FDK_LIBS) $(BACKEND_LIBS) -o $@

# ---- install --------------------------------------------------------------------

install: all
	install -d $(DESTDIR)$(INCDIR)/fdk-shell
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 644 include/fdk-shell/fdk-shell.h \
	    $(DESTDIR)$(INCDIR)/fdk-shell/fdk-shell.h
	install -m 644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/libfdk-shell.a
	install -m 755 $(SHARED_LIB) \
	    $(DESTDIR)$(LIBDIR)/libfdk-shell.so.$(FDKSHELL_VERSION)
	ln -sf libfdk-shell.so.$(FDKSHELL_VERSION) \
	    $(DESTDIR)$(LIBDIR)/$(FDKSHELL_SONAME)
	ln -sf $(FDKSHELL_SONAME) $(DESTDIR)$(LIBDIR)/libfdk-shell.so
	@printf 'prefix=%s\nlibdir=%s\nincludedir=%s\n\nName: fdk-shell\nDescription: Layer-shell surfaces (panels, docks, overlays) for FDK\nVersion: %s\nURL: https://github.com/Faded-Dream-Project/FDK\nRequires: fdk\nCflags: -I$${includedir}\nLibs: -L$${libdir} -lfdk-shell\n# The .so already carries every platform dependency in its NEEDED\n# entries; a dynamically linked application needs none of them, and a\n# plain `pkg-config --cflags --libs fdk-shell` must succeed on machines\n# without the X11/Wayland development packages - hence raw platform\n# flags in Libs.private (used only by --static consumers) rather than\n# Requires.private or Libs.\nLibs.private: %s -lm\n' \
	    '$(PREFIX)' '$(LIBDIR)' '$(INCDIR)' '$(FDKSHELL_VERSION)' '$(shell pkg-config --libs x11 wayland-client 2>/dev/null || echo "")' \
	    > $(DESTDIR)$(LIBDIR)/pkgconfig/fdk-shell.pc

uninstall:
	rm -f $(DESTDIR)$(INCDIR)/fdk-shell/fdk-shell.h
	rm -f $(DESTDIR)$(LIBDIR)/libfdk-shell.a
	rm -f $(DESTDIR)$(LIBDIR)/libfdk-shell.so.$(FDKSHELL_VERSION)
	rm -f $(DESTDIR)$(LIBDIR)/$(FDKSHELL_SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/libfdk-shell.so
	rm -f $(DESTDIR)$(LIBDIR)/pkgconfig/fdk-shell.pc

clean:
	rm -rf $(BUILD_DIR)

DEPS := $(OBJS:.o=.d) $(OBJS_PIC:.o=.d)
-include $(DEPS)
