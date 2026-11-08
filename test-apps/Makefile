# test-apps/Makefile — builds the four validation applications against
# the in-tree FDK (../build/libfdk.a or ../build/libfdk.so).
#
#   make            build every app
#   make clean      remove build artifacts
#   make test-apps-smoke
#                   run each app under Xvfb with a frame budget and
#                   scripted quit (see smoke: below)
#
# The parent repo's Makefile also grows `test-apps` /
# `test-apps-smoke` targets that recurse into this directory.
#
# Zero-warning policy: the same warning set the FDK build uses. The
# apps are CLIENTS of the public API only — no ../src includes, no
# backend headers, no internal structs.

ROOT     := ..
BUILD    := build
FDK_LIB  := $(ROOT)/build/libfdk.a
FDK_PC   := $(ROOT)/build

CC       ?= gcc
STD      := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wconversion -Wsign-conversion \
            -Wcast-qual -Wpointer-arith -Wundef -Wwrite-strings
FEATURE  := -D_POSIX_C_SOURCE=200809L
CPPFLAGS := -I$(ROOT)/include -Icommon
# §31/S9: ZERO warnings is the project rule. -Werror makes a warning
# anywhere fail the build (never loosened, only ever satisfied) — the
# owner's newer gcc flags truncation paths this sandbox's gcc misses.
CFLAGS   := $(STD) $(WARN) $(FEATURE) -g -O1 -MMD -MP -Werror \
           -fsanitize=address,undefined -DFDK_DEBUG_BUILD=1

# Backends: the in-tree FDK links X11 (+ Xrender via libfdk) and
# optionally Wayland. The app links only what libfdk's static archive
# needs transitively: the same set the root Makefile uses.
X11_LIBS := $(shell pkg-config --libs x11 2>/dev/null) -lXext \
            $(shell pkg-config --libs xrandr 2>/dev/null) -lXrender
WL_LIBS  := $(shell pkg-config --libs wayland-client xkbcommon 2>/dev/null)
LIBS     := $(X11_LIBS) $(WL_LIBS) -lm -ldl -lz -fsanitize=address,undefined

APP_DIRS := fdk-files fdk-edit fdk-monitor fdk-notes fdk-term \
            fdk-chat fdk-stress fdk-workbench fdk-keys fdk-langs

# each app: every .c in its dir + common/*.c
COMMON_SRCS := common/icons.c common/app.c

BINS := $(APP_DIRS:%=$(BUILD)/%)

.PHONY: all clean test-apps-smoke release

all: $(BINS)

# release (round 18): the profiling shape the owner was promised in
# round 17's instructions but the Makefile never had — -O2, no
# sanitizers, no FDK_DEBUG_BUILD, into build-rel/. The library it
# links is whatever ../build/libfdk.a currently is, so the honest
# invocation from the repo root is `make release && make -C test-apps
# release` (the root release target rebuilds the LIB in release
# config first). BUILD is re-pointed, not duplicated: one app set
# per config directory, `make clean` still clears only `build/`
# (build-rel/ is removed by `make clean-all`).
REL_BUILD := build-rel
REL_BINS  := $(APP_DIRS:%=$(REL_BUILD)/%)

release: $(REL_BINS)

define REL_APP_RULE
$(REL_BUILD)/$(1): $$(filter-out %/gen_app_icon.c %/perf_50k.c %/drag_rig.c %/term_test.c %/click_rig.c %/menu_rig.c %/nav_rig.c %/nav_probe.c %/input_rig.c %/stress_rig.c %/delete_rig.c %/sidebar_test.c %/sidebar_view.c %/chat_test.c,$$(wildcard $(1)/*.c)) $$(COMMON_SRCS) $$(wildcard common/*.h) $$(FDK_LIB)
	@mkdir -p $(REL_BUILD)
	$$(CC) $$(CPPFLAGS) $$(STD) $$(WARN) $$(FEATURE) -O2 -DNDEBUG -MMD -MP -Werror -DPO_DIR='"$(CURDIR)/$(1)/po"' -o $$@ $$(filter-out %/gen_app_icon.c %/perf_50k.c %/drag_rig.c %/term_test.c %/click_rig.c %/menu_rig.c %/nav_rig.c %/nav_probe.c %/input_rig.c %/stress_rig.c %/delete_rig.c %/sidebar_test.c %/sidebar_view.c %/chat_test.c,$$(wildcard $(1)/*.c)) $$(COMMON_SRCS) $$(FDK_LIB) $$(REL_LIBS)
endef

$(foreach app,$(filter-out fdk-keys,$(APP_DIRS)),$(eval $(call REL_APP_RULE,$(app))))

# fdk-keys' release link: the registry rides along exactly like the
# dev rule (make release used to drop it and fail on rxkb_* — found
# on a user machine, not in the sandbox's dev-only builds)
XKBREGISTRY_LIBS := $(shell pkg-config --libs xkbregistry 2>/dev/null)
XKBREGISTRY_CFLAGS := $(shell pkg-config --cflags xkbregistry 2>/dev/null)
ifeq ($(strip $(XKBREGISTRY_LIBS)),)
XKBREGISTRY_CFLAGS := -DFDK_NO_XKBREGISTRY
endif
$(REL_BUILD)/fdk-keys: $(filter-out %/gen_app_icon.c %/perf_50k.c,$(wildcard fdk-keys/*.c)) $(COMMON_SRCS) $(wildcard common/*.h) $(FDK_LIB)
	@mkdir -p $(REL_BUILD)
	$(CC) $(CPPFLAGS) $(shell pkg-config --cflags xkbcommon 2>/dev/null) $(XKBREGISTRY_CFLAGS) $(STD) $(WARN) $(FEATURE) -O2 -DNDEBUG -MMD -MP -Werror -DPO_DIR='"$(CURDIR)/fdk-keys/po"' -o $@ $(filter-out %/gen_app_icon.c %/perf_50k.c,$(wildcard fdk-keys/*.c)) $(COMMON_SRCS) $(FDK_LIB) $(REL_LIBS) $(XKBREGISTRY_LIBS)

# The release link set: the archive's transitive backends, no
# sanitizer runtime (the LIB must be release-config too — linking a
# release app against the debug lib drags __asan_init in).
REL_X11_LIBS := $(shell pkg-config --libs x11 2>/dev/null) -lXext \
            $(shell pkg-config --libs xrandr 2>/dev/null) -lXrender
REL_WL_LIBS  := $(shell pkg-config --libs wayland-client xkbcommon 2>/dev/null)
REL_LIBS     := $(REL_X11_LIBS) $(REL_WL_LIBS) -lm -ldl -lz

clean-all: clean
	rm -rf $(REL_BUILD)

.PHONY: clean-all

TERM_PTY_FLAGS := -D_GNU_SOURCE=1 -lutil

define APP_RULE
$(BUILD)/$(1): $$(filter-out %/gen_app_icon.c %/perf_50k.c %/drag_rig.c %/term_test.c %/click_rig.c %/menu_rig.c %/nav_rig.c %/nav_probe.c %/input_rig.c %/stress_rig.c %/delete_rig.c %/sidebar_test.c %/sidebar_view.c %/chat_test.c,$$(wildcard $(1)/*.c)) $$(COMMON_SRCS) $$(wildcard common/*.h) $$(FDK_LIB)
	@mkdir -p $(BUILD)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) -DPO_DIR='"$(CURDIR)/$(1)/po"' -o $$@ $$(filter-out %/gen_app_icon.c %/perf_50k.c %/drag_rig.c %/term_test.c %/click_rig.c %/menu_rig.c %/nav_rig.c %/nav_probe.c %/input_rig.c %/stress_rig.c %/delete_rig.c %/sidebar_test.c %/sidebar_view.c %/chat_test.c,$$(wildcard $(1)/*.c)) $$(COMMON_SRCS) $$(FDK_LIB) $$(LIBS)
endef

$(foreach app,$(filter-out fdk-keys,$(APP_DIRS)),$(eval $(call APP_RULE,$(app))))

# fdk-keys' --sweep-layouts enumerates every xkb layout: the registry
# + keymap libs ride along on top of LIBS (the entry model itself
# stays on the public API only).
ifeq ($(strip $(XKBREGISTRY_LIBS)),)
XKBREGISTRY_LIBS :=
endif
$(BUILD)/fdk-keys: XKEYS_LIBS := $(shell pkg-config --libs xkbcommon 2>/dev/null) $(XKBREGISTRY_LIBS)
$(BUILD)/fdk-keys: XKEYS_CFLAGS := $(shell pkg-config --cflags xkbcommon 2>/dev/null) $(XKBREGISTRY_CFLAGS)
$(BUILD)/fdk-keys: $(filter-out %/gen_app_icon.c %/perf_50k.c,$(wildcard fdk-keys/*.c)) $(COMMON_SRCS) $(wildcard common/*.h) $(FDK_LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(XKEYS_CFLAGS) $(CFLAGS) -DPO_DIR='"$(CURDIR)/fdk-keys/po"' -o $@ $(filter-out %/gen_app_icon.c %/perf_50k.c,$(wildcard fdk-keys/*.c)) $(COMMON_SRCS) $(FDK_LIB) $(LIBS) $(XKEYS_LIBS)

$(FDK_LIB):
	$(MAKE) -C $(ROOT) static

# Smoke: each app under a throwaway Xvfb (the repo Makefile's own
# test-x11 pattern — xvfb-run needs xauth, which headless containers
# lack), frame-budgeted exit. A smoke PASS is "opened a window,
# painted, exited cleanly with rc 0 under the repo's LSan
# suppressions" — not a feature verification (those are the apps' own
# README checklists and the scripted rigs).
SMOKE_APPS := fdk-files fdk-monitor fdk-notes fdk-edit fdk-term \
               fdk-chat fdk-stress fdk-workbench \
               fdk-keys fdk-langs

test-apps-smoke:
	@rc=0; for app in $(SMOKE_APPS); do \
	  if [ -x $(BUILD)/$$app ]; then \
	    echo "=== smoke: $$app"; \
	    XVFB_NUM=$$((90 + ($$$$ % 400))); \
	    XVFB_DISP=":$$XVFB_NUM"; \
	    rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
	    Xvfb "$$XVFB_DISP" -screen 0 1280x800x24 >/tmp/fdk-testapps-xvfb.log 2>&1 & \
	    XVFB_PID=$$!; \
	    sleep 1.5; \
	    DISPLAY="$$XVFB_DISP" FDK_TESTAPP_FRAMES=40 \
	      LSAN_OPTIONS="suppressions=$(CURDIR)/$(ROOT)/tests/data/lsan-gl.supp" \
	      $(BUILD)/$$app --smoke $$( [ "$$app" = fdk-langs ] && echo "--dwell=0" ) \
      || rc=1; \
	    kill "$$XVFB_PID" 2>/dev/null; \
	    wait "$$XVFB_PID" 2>/dev/null; \
	    rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
	  else \
	    echo "=== smoke: $$app NOT BUILT"; rc=1; \
	  fi; \
	done; exit $$rc

# §26/3b: the fdk-files app-icon generator (FDK draws it, FDK saves it)
build/gen-fdk-files-icon: fdk-files/gen_app_icon.c $(FDK_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) fdk-files/gen_app_icon.c $(FDK_LIB) -o $@ $(LIBS)

.PHONY: fdk-files-icon
fdk-files-icon: build/gen-fdk-files-icon
	@mkdir -p fdk-files/desktop/icons/hicolor/48x48/apps fdk-files/desktop/icons/hicolor/128x128/apps
	./build/gen-fdk-files-icon fdk-files/desktop/icons/hicolor/48x48/apps/org.fdk.testapps.fdk-files.png 48
	./build/gen-fdk-files-icon fdk-files/desktop/icons/hicolor/128x128/apps/org.fdk.testapps.fdk-files.png 128

# §27/2b: the XTEST drag rig (Xlib + XTest only, no FDK)
# §31/PART-5: the sidebar model unit test (fixture mountinfo; no UI)
build/sidebar-test: fdk-files/sidebar.c fdk-files/sidebar_test.c fdk-files/sidebar.h common/ta_buf.h $(FDK_LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Icommon -o $@ fdk-files/sidebar.c fdk-files/sidebar_test.c $(FDK_LIB) $(LIBS)

build/drag-rig: fdk-files/drag_rig.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/my-project/local-x11/include fdk-files/drag_rig.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/my-project/local-x11/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/my-project/local-x11/lib/x86_64-linux-gnu

.PHONY: drag-rig
drag-rig: build/drag-rig

# Round 10: the wrong-row click rig (Xlib + XTest only, no FDK) —
# clicks a line down pane0's list and reads the app's PHASE: sel
# telemetry. Boots via run_clickrig.sh (private Xvfb + DRIG app).
# (drag-rig's line above predates the sandbox recycles that wiped
# /tmp/x11dev; the X11 dev tree now lives at /tmp/my-project/local-x11,
# which survives them — both rigs point there now.)
build/click-rig: fdk-files/click_rig.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/my-project/local-x11/include fdk-files/click_rig.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/my-project/local-x11/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/my-project/local-x11/lib/x86_64-linux-gnu

.PHONY: click-rig
click-rig: build/click-rig

# Round 11: the context-menu rig (Xlib + XTest only, no FDK) —
# right-clicks the folder row, finds the real popup window, clicks
# its LAST entry (Properties), and reads the app's PHASE: act
# telemetry: the action that runs must be Properties, not the
# permanent-delete confirm the round-11 off-by-one fired.
# Boots via run_menurig.sh (private Xvfb + DRIG app).
build/menu-rig: fdk-files/menu_rig.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/my-project/local-x11/include fdk-files/menu_rig.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/my-project/local-x11/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/my-project/local-x11/lib/x86_64-linux-gnu

.PHONY: menu-rig
menu-rig: build/menu-rig
	./run_menurig.sh

# Round 12: the navigation-storm CPU rig (Xlib + XTest, no FDK) —
# double-clicks into a big directory and back, N times, while
# sampling the app's CPU from /proc. Boots via run_navrig.sh.
build/input-rig: fdk-files/input_rig.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/my-project/local-x11/include fdk-files/input_rig.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/my-project/local-x11/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/my-project/local-x11/lib/x86_64-linux-gnu

build/nav-probe: fdk-files/nav_probe.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/my-project/local-x11/include fdk-files/nav_probe.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/my-project/local-x11/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/my-project/local-x11/lib/x86_64-linux-gnu

build/nav-rig: fdk-files/nav_rig.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/my-project/local-x11/include fdk-files/nav_rig.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/my-project/local-x11/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/my-project/local-x11/lib/x86_64-linux-gnu

# Round 14: the REAL-RATE stress rig (wheel at 4ms gaps, 1kHz hover,
# scrollbar thumb drags). Boots via run_stressrig.sh.
build/delete-rig: fdk-files/delete_rig.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/x11dev/usr/include fdk-files/delete_rig.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/x11dev/usr/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/x11dev/usr/lib/x86_64-linux-gnu

build/stress-rig: fdk-files/stress_rig.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -I/tmp/x11dev/usr/include fdk-files/stress_rig.c -o $@ $(shell pkg-config --libs x11) -lXext -L/tmp/x11dev/usr/lib/x86_64-linux-gnu -lXtst -Wl,-rpath,/tmp/x11dev/usr/lib/x86_64-linux-gnu

.PHONY: nav-rig
nav-rig: build/nav-rig
	./run_navrig.sh

build/gen-perf-50k: fdk-files/perf_50k.c $(FDK_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) fdk-files/perf_50k.c $(FDK_LIB) -o $@ $(LIBS)

.PHONY: fdk-files-perf-50k
fdk-files-perf-50k: build/gen-perf-50k
	./build/gen-perf-50k

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

# fdk-term: forkpty (libutil) + _GNU_SOURCE for the PTY layer
$(BUILD)/fdk-term: EXTRA_LIBS := -lutil
$(BUILD)/fdk-term: CFLAGS := $(CFLAGS) -D_GNU_SOURCE=1
$(BUILD)/fdk-term: LIBS := $(LIBS) -lutil

build/fdk-term-test: fdk-term/term_test.c fdk-term/term.c \
        fdk-term/vt.c fdk-term/wide.c fdk-term/colors.c $(FDK_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(STD) $(WARN) $(FEATURE) -D_GNU_SOURCE=1 -g -O0 \
	    -fsanitize=address,undefined -o $@ fdk-term/term_test.c fdk-term/term.c fdk-term/vt.c \
	    fdk-term/wide.c fdk-term/colors.c $(FDK_LIB) $(LIBS) -lutil

.PHONY: fdk-term-test
fdk-term-test: build/fdk-term-test
	ASAN_OPTIONS=detect_leaks=0 ./build/fdk-term-test

# fdk-chat: the engine layers headless (irc parse/mirc/state +
# the net layer against a local TCP peer). fdk-desktop_open_uri's
# xdg-open probe stays out of the app binary; no extra libs.
build/fdk-chat-test: fdk-chat/chat_test.c fdk-chat/irc.c \
        fdk-chat/net.c fdk-chat/log.c $(FDK_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(STD) $(WARN) $(FEATURE) -g -O0 \
	    -fsanitize=address,undefined -o $@ fdk-chat/chat_test.c fdk-chat/irc.c \
	    fdk-chat/net.c fdk-chat/log.c $(FDK_LIB) $(LIBS)

.PHONY: fdk-chat-test
fdk-chat-test: build/fdk-chat-test
	@if [ -n "$$DISPLAY" ]; then \
		ASAN_OPTIONS=detect_leaks=0 ./build/fdk-chat-test; \
	else \
		echo "== chat-test: no DISPLAY, throwaway Xvfb =="; \
		XVFB_NUM=$$((50 + $$$$ % 30)); \
		Xvfb ":$$XVFB_NUM" -screen 0 800x600x24 >/dev/null 2>&1 & \
		XVFB_PID=$$!; sleep 1; \
		ASAN_OPTIONS=detect_leaks=0 DISPLAY=":$$XVFB_NUM" ./build/fdk-chat-test; \
		rc=$$?; kill $$XVFB_PID 2>/dev/null; exit $$rc; \
	fi
