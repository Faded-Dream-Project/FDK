# Makefile — Faded Dream ToolKit (FDK)
#
# Targets:
#   make            build static + shared library (debug)
#   make release    build with optimizations, NDEBUG defined
#   make static     static library only (libfdk.a)
#   make shared     shared library only (libfdk.so)
#   make test       build and run the test suite
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

BUILD_DIR   := build
DEBUG_FLAGS := -g -O0 -DFDK_DEBUG_BUILD=1 -fsanitize=address,undefined
REL_FLAGS   := -O2 -DNDEBUG

CFLAGS  ?= $(STD) $(WARN) $(FEATURE) $(DEBUG_FLAGS)
CPPFLAGS:= -Iinclude -Isrc
LDFLAGS ?=

# --- Sources ------------------------------------------------------------

CORE_SRCS := $(wildcard src/core/*.c)
# Additional module source dirs (widget, layout, render, theme, window,
# input, text, platform/x11, platform/wayland) are added here as each
# phase lands. Phase 1 = core only.
LIB_SRCS  := $(CORE_SRCS)

# Static and shared builds use separate object trees (obj/ vs obj-pic/)
# since shared objects must be position-independent (-fPIC) and static
# ones need not be — reusing one tree between `make static` and
# `make shared` in the same invocation would silently link stale
# non-PIC objects into the .so. See docs/build.md.
LIB_OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRCS))
LIB_OBJS_PIC  := $(patsubst src/%.c,$(BUILD_DIR)/obj-pic/%.o,$(LIB_SRCS))

STATIC_LIB := $(BUILD_DIR)/libfdk.a
SHARED_LIB := $(BUILD_DIR)/libfdk.so

TEST_SRCS := $(wildcard tests/*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

EXAMPLE_SRCS := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/examples/%,$(EXAMPLE_SRCS))

.PHONY: all release static shared test examples install uninstall clean

all: static shared

release: CFLAGS := $(STD) $(WARN) $(FEATURE) $(REL_FLAGS)
release: all

static: $(STATIC_LIB)
shared: $(SHARED_LIB)

$(BUILD_DIR)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/obj-pic/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

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
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Examples ---------------------------------------------------------

examples: $(EXAMPLE_BINS)

$(BUILD_DIR)/examples/%: examples/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) -o $@ $(LDFLAGS)

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
