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
#   make install    install headers + libraries to PREFIX (default /usr/local)
#   make uninstall  remove what `install` installed
#   make clean      remove build output
#
# Override on the command line, e.g.:
#   make release PREFIX=/usr

CC       ?= gcc
AR       ?= ar
PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include

STD      := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wconversion -Wsign-conversion \
            -Wcast-qual -Wpointer-arith -Wundef -Wwrite-strings
FEATURE  := -D_POSIX_C_SOURCE=200809L

# Platform backend dependencies. See docs/dependencies.md for the
# justification of each. Both are always built on Linux per project
# requirements — there is no "X11-only" or "Wayland-only" build
# configuration (the backend actually used at runtime is chosen by
# fdk_init(), not by what's compiled in — see docs/architecture.md).
X11_CFLAGS     := $(shell pkg-config --cflags x11)
X11_LIBS       := $(shell pkg-config --libs x11)
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client xkbcommon)
WAYLAND_LIBS   := $(shell pkg-config --libs wayland-client xkbcommon)

BUILD_DIR   := build
DEBUG_FLAGS := -g -O0 -DFDK_DEBUG_BUILD=1 -fsanitize=address,undefined
REL_FLAGS   := -O2 -DNDEBUG

CFLAGS  ?= $(STD) $(WARN) $(FEATURE) $(DEBUG_FLAGS)
CPPFLAGS:= -Iinclude -Isrc
LDFLAGS ?= $(X11_LIBS) $(WAYLAND_LIBS)

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
              $(if $(findstring src/platform/wayland/,$(1)),$(WAYLAND_CFLAGS) -Wno-cast-qual)

# --- Sources ------------------------------------------------------------

CORE_SRCS     := $(wildcard src/core/*.c)
PLATFORM_X11_SRCS     := $(wildcard src/platform/x11/*.c)
PLATFORM_WAYLAND_SRCS := $(wildcard src/platform/wayland/*.c) \
                         src/platform/wayland/generated/xdg-shell-protocol.c
WINDOW_SRCS   := $(wildcard src/window/*.c)
LIB_SRCS      := $(CORE_SRCS) $(PLATFORM_X11_SRCS) $(PLATFORM_WAYLAND_SRCS) $(WINDOW_SRCS)

# Static and shared builds use separate object trees (obj/ vs obj-pic/)
# since shared objects must be position-independent (-fPIC) and static
# ones need not be — reusing one tree between `make static` and
# `make shared` in the same invocation would silently link stale
# non-PIC objects into the .so. See docs/build.md.
LIB_OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRCS))
LIB_OBJS_PIC  := $(patsubst src/%.c,$(BUILD_DIR)/obj-pic/%.o,$(LIB_SRCS))

STATIC_LIB := $(BUILD_DIR)/libfdk.a
SHARED_LIB := $(BUILD_DIR)/libfdk.so

TEST_SRCS := $(filter-out tests/test_x11_integration.c tests/test_wayland_integration.c,$(wildcard tests/*.c))
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

X11_TEST_SRC := tests/test_x11_integration.c
X11_TEST_BIN := $(BUILD_DIR)/tests/test_x11_integration

EXAMPLE_SRCS := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/examples/%,$(EXAMPLE_SRCS))

.PHONY: all release static shared test test-x11 examples install uninstall clean

all: static shared

release: CFLAGS := $(STD) $(WARN) $(FEATURE) $(REL_FLAGS)
release: all

static: $(STATIC_LIB)
shared: $(SHARED_LIB)

$(BUILD_DIR)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(call extra_flags,$<) -c $< -o $@

$(BUILD_DIR)/obj-pic/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(call extra_flags,$<) -fPIC -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS_PIC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $^ $(LDFLAGS)

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
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

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

$(X11_TEST_BIN): $(X11_TEST_SRC) $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Examples ---------------------------------------------------------

examples: $(EXAMPLE_BINS)

$(BUILD_DIR)/examples/%: examples/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Install ------------------------------------------------------------

install: all
	install -d $(DESTDIR)$(INCDIR)/fdk
	install -m 644 include/fdk/*.h $(DESTDIR)$(INCDIR)/fdk/
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/

uninstall:
	rm -rf $(DESTDIR)$(INCDIR)/fdk
	rm -f $(DESTDIR)$(LIBDIR)/libfdk.a
	rm -f $(DESTDIR)$(LIBDIR)/libfdk.so

clean:
	rm -rf $(BUILD_DIR)
