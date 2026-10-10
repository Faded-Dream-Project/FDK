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
#   make tools      build the command-line tools (fdk-theme, fdk-prefs;
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
X11_CFLAGS     := $(shell pkg-config --cflags x11)
# Xext: the MIT-SHM extension (XShmPutImage presentation fast path,
# Phase 3 completion). It is part of the base X11 distribution and
# present wherever libX11 is.
X11_LIBS       := $(shell pkg-config --libs x11) -lXext

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
DEBUG_FLAGS := -g -O0 -DFDK_DEBUG_BUILD=1 -fsanitize=address,undefined
REL_FLAGS   := -O2 -DNDEBUG

# Build configuration (debug by default; `make release` re-invokes
# make with FDK_CONFIG=release). The two configs MUST NOT share
# objects: they differ in -O level, NDEBUG, and the sanitizers, and
# make compares only timestamps — objects left over from the other
# config are always "up to date" and get silently linked in. That
# exact mixing (release objects inside an ASan-linked binary) is
# what made the compositor-death rig report fontconfig's
# process-lifetime allocations as leaks in 1.3.4: the LSan bracket
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
# live in 1.3.7: the sway examples rig linked apps built against the
# Wayland-disabled stub, which then ignored a perfectly valid
# WAYLAND_DISPLAY and fell through to a dead X11). Both axes live in
# ONE stamp name so a flip of either forces one full rebuild.
CONFIG_STAMP := $(BUILD_DIR)/.config-$(FDK_CONFIG)-wl$(BUILD_WAYLAND)

CFLAGS  ?= $(STD) $(WARN) $(FEATURE) $(CONFIG_FLAGS)
CPPFLAGS:= -Iinclude -Isrc
# -ldl: the text layer dlopen()s fontconfig at run time (optional
# system font discovery, see src/text/fontscan.c). No-op stub on
# glibc >= 2.34 where dlopen lives in libc; required for static
# linking on older glibc.
LDFLAGS ?= $(X11_LIBS) $(WAYLAND_LIBS) -lm -ldl -lz

# pkg-config exposure (fdk.pc): private deps are the ones the .so
# already links (apps need nothing); Requires.private lets static
# linking pull them in transitively.
PC_REQUIRES := $(if $(FDK_HAS_WAYLAND),wayland-client xkbcommon,) x11
# -ldl for static linking (see LDFLAGS above); -lm is appended by
# fdk.pc.in itself.
PC_LIBS := -ldl

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
              $(if $(findstring src/render/,$(1)),-Ithird_party/stb)

# --- Sources ------------------------------------------------------------

CORE_SRCS     := $(wildcard src/core/*.c)
PLATFORM_X11_SRCS     := $(wildcard src/platform/x11/*.c)
ifeq ($(BUILD_WAYLAND),1)
  PLATFORM_WAYLAND_SRCS := $(wildcard src/platform/wayland/*.c) \
                           src/platform/wayland/generated/xdg-shell-protocol.c \
                           src/platform/wayland/generated/xdg-decoration-unstable-v1-protocol.c \
                           src/platform/wayland/generated/viewporter-protocol.c \
                           src/platform/wayland/generated/fractional-scale-v1-protocol.c \
                           src/platform/wayland/generated/primary-selection-unstable-v1-protocol.c
else
  # Stub providing fdk_platform_wayland_ops() returning NULL and
  # fdk_platform_wayland_display_present() returning 0, so
  # src/core/context.c's select_and_connect() cleanly skips Wayland
  # and falls through to X11. See wayland_disabled.c for why this is
  # a separate file (NOT under src/platform/wayland/, which would
  # break the wildcard that's active when Wayland IS enabled).
  PLATFORM_WAYLAND_SRCS := src/platform/wayland_disabled.c
endif
WINDOW_SRCS   := $(wildcard src/window/*.c)
RENDER_SRCS   := $(wildcard src/render/*.c)
WIDGET_SRCS   := $(wildcard src/widget/*.c)
LAYOUT_SRCS   := $(wildcard src/layout/*.c)
TEXT_SRCS     := $(wildcard src/text/*.c)
THEME_SRCS    := $(wildcard src/theme/*.c)
I18N_SRCS     := $(wildcard src/i18n/*.c)
LIB_SRCS      := $(CORE_SRCS) $(PLATFORM_X11_SRCS) $(PLATFORM_WAYLAND_SRCS) $(WINDOW_SRCS) $(RENDER_SRCS) $(WIDGET_SRCS) $(LAYOUT_SRCS) $(TEXT_SRCS) $(THEME_SRCS) $(I18N_SRCS)

# Static and shared builds use separate object trees (obj/ vs obj-pic/)
# since shared objects must be position-independent (-fPIC) and static
# ones need not be — reusing one tree between `make static` and
# `make shared` in the same invocation would silently link stale
# non-PIC objects into the .so. See docs/build.md.
LIB_OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRCS))
LIB_OBJS_PIC  := $(patsubst src/%.c,$(BUILD_DIR)/obj-pic/%.o,$(LIB_SRCS))

STATIC_LIB := $(BUILD_DIR)/libfdk.a
SHARED_LIB := $(BUILD_DIR)/libfdk.so

# Shared-library export map (1.3.9). The .so must export EXACTLY the
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

TEST_SRCS := $(filter-out tests/test_x11_integration.c tests/test_wayland_integration.c tests/bench.c,$(wildcard tests/*.c))
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

X11_TEST_SRC := tests/test_x11_integration.c
X11_TEST_BIN := $(BUILD_DIR)/tests/test_x11_integration

EXAMPLE_SRCS := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/examples/%,$(EXAMPLE_SRCS))

# Command-line tools (1.4.13). Built like tests and examples — against
# the static archive, on the public headers only (tools/toolutil.h is
# the one tools-local header). They link the full LDFLAGS set like
# every other consumer of libfdk.a.
TOOL_SRCS := $(wildcard tools/*.c)
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/tools/%,$(TOOL_SRCS))

# Shipped themes: installed into $(DATADIR)/fdk/themes, which the
# runtime discovery path reaches through the default $XDG_DATA_DIRS
# (/usr/local/share and /usr/share both are) — an installed FDK finds
# its own themes with zero configuration, and `fdk-theme list` after
# `make install` shows exactly these next to the built-in.
SHIPPED_THEMES := $(wildcard themes/*.fdk)

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

.PHONY: all release static shared test test-x11 test-wayland bench examples tools install uninstall clean verify-exports

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

static: $(STATIC_LIB)
shared: $(SHARED_LIB)

# The stamp rule always RUNS (FORCE) but only rewrites itself when
# the configuration actually changed — so same-config builds stay
# incremental (the stamp keeps its old mtime), while a config switch
# produces a fresh, newer stamp that rebuilds every object.
$(CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@cur=`cat $@ 2>/dev/null || echo none`; \
	if [ x"$$cur" != x"$(FDK_CONFIG)-wl$(BUILD_WAYLAND)" ]; then \
		rm -f $(BUILD_DIR)/.config-*; \
		printf '%s\n' '$(FDK_CONFIG)-wl$(BUILD_WAYLAND)' > $@; \
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
# 1.3.7 after an environment reset wiped the Wayland toolchain: the
# link failed on wayland_seat.o's wl_proxy_get_version against a
# system libwayland with no dev files). Same bug class as the 1.3.4
# obj/obj-pic mixing one directory over; the archive is now always
# exactly the current member list.
$(STATIC_LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS_PIC) $(EXPORT_MAP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -shared -Wl,--version-script=$(EXPORT_MAP) -o $@ $(LIB_OBJS_PIC) $(LDFLAGS)

# Generates the export map: intersection of header-declared names and
# archive-defined globals (see the EXPORT_MAP block above). Depends on
# the static archive so `make shared` after any code change regenerates
# it; depends on the headers so adding/removing a public function does
# too. nm/sed/sort/uniq/tr are binutils+coreutils — no build-time
# dependency beyond what the build already uses.
$(EXPORT_MAP): $(STATIC_LIB) $(wildcard include/fdk/*.h) Makefile
	@echo "== generating $(EXPORT_MAP) (public API only; internals stay local) =="
	@printf '{\n  global:\n' > $@
	@{ nm --defined-only $(STATIC_LIB) \
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' | sort -u; \
	   grep -hoE '\bfdk_[a-z0-9_]+[[:space:]]*\(' include/fdk/*.h \
	           | tr -d ' (' | sort -u; } \
	  | sort | uniq -d | sed 's/^/    /; s/$$/;/' >> $@
	@printf '  local:\n    *;\n};\n' >> $@

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
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' | sort -u); \
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
	echo "EXPORT VERIFICATION OK: the .so exports exactly the public API"

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

$(BUILD_DIR)/tests/%: tests/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< $(STATIC_LIB) -o $@ $(LDFLAGS)

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
test-x11: $(X11_TEST_BIN)
	@if [ -n "$$DISPLAY" ]; then \
		echo "== running X11 integration test against DISPLAY=$$DISPLAY =="; \
		"$(X11_TEST_BIN)" || exit 1; \
	else \
		echo "== no DISPLAY set, starting a throwaway Xvfb =="; \
		XVFB_NUM=$$((90 + ($$$$ % 400))); \
		XVFB_DISP=":$$XVFB_NUM"; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		Xvfb "$$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdk-xvfb-test.log 2>&1 & \
		XVFB_PID=$$!; \
		sleep 2; \
		echo "== running X11 integration test against DISPLAY=$$XVFB_DISP (Xvfb pid $$XVFB_PID) =="; \
		DISPLAY="$$XVFB_DISP" "$(X11_TEST_BIN)"; \
		TEST_RESULT=$$?; \
		kill "$$XVFB_PID" 2>/dev/null; \
		wait "$$XVFB_PID" 2>/dev/null; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		exit $$TEST_RESULT; \
	fi

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

$(WL_TEST_BIN): $(WL_TEST_SRC) $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< $(STATIC_LIB) -o $@ $(LDFLAGS)

$(X11_TEST_BIN): $(X11_TEST_SRC) $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual -Ithird_party/stb $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Examples ---------------------------------------------------------

examples: $(EXAMPLE_BINS)

$(BUILD_DIR)/examples/%: examples/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Tools (1.4.13) ------------------------------------------------------

tools: $(TOOL_BINS)

$(BUILD_DIR)/tools/%: tools/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) -o $@ $(LDFLAGS)

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
# from fdk_version.h, never from a second hand-edited copy.
$(BUILD_DIR)/fdk.pc: fdk.pc.in include/fdk/fdk_version.h Makefile
	@mkdir -p $(BUILD_DIR)
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCDIR@|$(INCDIR)|g' \
	     -e 's|@VERSION@|$(FDK_PC_VERSION)|g' \
	     -e 's|@PC_REQUIRES@|$(PC_REQUIRES)|g' \
	     -e 's|@PC_LIBS@|$(PC_LIBS)|g' fdk.pc.in > $@

install: all $(BUILD_DIR)/fdk.pc
	install -d $(DESTDIR)$(INCDIR)/fdk
	install -m 644 include/fdk/*.h $(DESTDIR)$(INCDIR)/fdk/
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 644 $(BUILD_DIR)/fdk.pc \
	               $(DESTDIR)$(LIBDIR)/pkgconfig/fdk.pc
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TOOL_BINS) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(DATADIR)/fdk/themes
	install -m 644 $(SHIPPED_THEMES) $(DESTDIR)$(DATADIR)/fdk/themes/

uninstall:
	rm -rf $(DESTDIR)$(INCDIR)/fdk
	rm -f $(DESTDIR)$(LIBDIR)/libfdk.a
	rm -f $(DESTDIR)$(LIBDIR)/libfdk.so
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
