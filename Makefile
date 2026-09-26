# GNU make predefines CXX as g++, so `CXX ?= clang++` would never take effect.
# Only make's own default is replaced; CXX from the command line or the
# environment wins.
ifeq ($(origin CXX),default)
CXX = clang++
endif
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -Iplugins -Iplugins/latency_mitigator/include -Iplugins/combat_meter/include -Itests

COMMON_SRCS = $(wildcard src/common/*.cpp) \
              $(wildcard src/common/ipc/*.cpp) \
              $(wildcard src/common/config/*.cpp) \
              $(wildcard src/common/os/*.cpp) \
              $(wildcard src/common/ui/*.cpp)

PLUGIN_SRCS = $(wildcard plugins/latency_mitigator/src/*.cpp) \
              $(wildcard plugins/combat_meter/src/*.cpp)

PAYLOAD_SRCS = $(wildcard src/payload/*.cpp)

APP_SRCS = src/app/app_state.cpp \
           $(wildcard src/app/ui/*.cpp)

TEST_SRCS = $(wildcard tests/*.cpp)

RUNNER_SRCS = $(COMMON_SRCS) $(PLUGIN_SRCS) $(PAYLOAD_SRCS) $(APP_SRCS) $(TEST_SRCS)

# The sanitizer runs are the same suite built into build/<variant>/.
TSAN_FLAGS = -fsanitize=thread -g -O1
ASAN_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=undefined \
             -fno-omit-frame-pointer -g -O1

# The desktop UI lives behind HAVE_IMGUI, which the mock build never defines, so
# those blocks compile for the first time in CI unless they are checked here.
# ImGui is vendored and platform-independent; the Win32/DX11 backends are not,
# so this is syntax-only and does not link.
IMGUI_DIR = src/third_party/imgui
UI_CHECK_SRCS = $(APP_SRCS)
# clang 20's -Wnontrivial-memcall fires on imgui.h's own memset(this, ...) constructors.
# Older clang rejects the unknown -Wno- option under -Werror and g++ has no such
# warning, so it is only passed to a compiler that knows it.
NO_NONTRIVIAL_MEMCALL := $(shell $(CXX) -Werror -Wnontrivial-memcall -x c++ -fsyntax-only /dev/null 2>/dev/null && echo -Wno-nontrivial-memcall)
UI_CHECK_FLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Werror \
                 -Iinclude -Isrc -Iplugins -Iplugins/latency_mitigator/include -Iplugins/combat_meter/include \
                 -I$(IMGUI_DIR) -DHAVE_IMGUI=1 -include $(IMGUI_DIR)/imgui.h \
                 $(NO_NONTRIVIAL_MEMCALL)

all: test

test: hub_test_runner check-ui check-layers
	./hub_test_runner

# Rejects an #include that crosses AGENTS.md's layers (see tools/check_layers.py).
check-layers:
	@python3 tools/check_layers.py

# Each variant compiles one object per source into build/<variant>/obj, with -MMD -MP
# dependency files, so an edit recompiles only what includes it. flags records the
# compiler and flags: it is rewritten only when they change, and then every object
# of that variant is rebuilt rather than linked against another compiler's.
# $(1): variant, $(2): runner, $(3): extra compile and link flags.
define test_runner
$(1)_OBJS = $$(patsubst %.cpp,build/$(1)/obj/%.o,$$(RUNNER_SRCS))

$(2): $$($(1)_OBJS)
	$$(CXX) $$(CXXFLAGS) $(3) $$^ -o $$@ -pthread

$$($(1)_OBJS): build/$(1)/obj/%.o: %.cpp build/$(1)/flags
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $(3) -MMD -MP -c $$< -o $$@

build/$(1)/flags: FORCE
	@mkdir -p $$(dir $$@)
	@echo '$$(CXX) $$(CXXFLAGS) $(3)' | cmp -s - $$@ || echo '$$(CXX) $$(CXXFLAGS) $(3)' > $$@

-include $$($(1)_OBJS:.o=.d)
endef

$(eval $(call test_runner,test,hub_test_runner,))
$(eval $(call test_runner,tsan,build/tsan/hub_test_runner,$(TSAN_FLAGS)))
$(eval $(call test_runner,asan,build/asan/hub_test_runner,$(ASAN_FLAGS)))

# ThreadSanitizer run of the suite (see .claude/skills/tsan-check). TSan exits
# non-zero when it reported anything.
tsan: build/tsan/hub_test_runner
	TSAN_OPTIONS="halt_on_error=1 $(TSAN_OPTIONS)" ./build/tsan/hub_test_runner

# AddressSanitizer and UndefinedBehaviorSanitizer run of the suite.
asan: build/asan/hub_test_runner
	ASAN_OPTIONS="detect_leaks=1 $(ASAN_OPTIONS)" UBSAN_OPTIONS="print_stacktrace=1 $(UBSAN_OPTIONS)" \
		./build/asan/hub_test_runner

# One stamp per UI source, so only an edited file (or one whose headers changed)
# is checked again.
UI_CHECK_STAMPS = $(patsubst %.cpp,build/check-ui/%.ok,$(UI_CHECK_SRCS))

ifneq ($(wildcard $(IMGUI_DIR)/imgui.h),)
check-ui: $(UI_CHECK_STAMPS)
	@echo "UI syntax check: OK"
else
check-ui:
	@echo "UI syntax check: skipped (no vendored ImGui)"
endif

$(UI_CHECK_STAMPS): build/check-ui/%.ok: %.cpp build/check-ui/flags
	@mkdir -p $(dir $@)
	$(CXX) $(UI_CHECK_FLAGS) -fsyntax-only -MMD -MP -MF $(@:.ok=.d) -MT $@ $<
	@touch $@

build/check-ui/flags: FORCE
	@mkdir -p $(dir $@)
	@echo '$(CXX) $(UI_CHECK_FLAGS)' | cmp -s - $@ || echo '$(CXX) $(UI_CHECK_FLAGS)' > $@

-include $(UI_CHECK_STAMPS:.ok=.d)

# README screenshots (docs/images), on Linux or macOS with zlib. The desktop window
# and both overlays are drawn by their own code into a software rasterizer: the
# overlay host and the overlays are built as on Windows against a shim, and the
# rest as check-ui builds the desktop UI. See tools/screenshots/main.cpp.
SHOTS_DIR = tools/screenshots
SHOTS_BUILD = build/screenshots
# A folder holding Fonts/segoeui.ttf, segoeuib.ttf and seguisb.ttf. By default,
# Selawik (Microsoft's open Segoe UI stand-in, OFL) under those names.
SHOTS_WINDOWS_DIR ?= $(SHOTS_BUILD)/windows
SELAWIK_PACKAGE = https://registry.npmjs.org/winstrap/-/winstrap-0.5.12.tgz
SELAWIK_SHA256 = c6ed78b1b13b531588b1b10e002380740adbacbd192cd85e6c2178abec042578

SHOTS_BASE_FLAGS = -std=c++20 -O2 -MMD -MP \
                   -Iinclude -Isrc -Iplugins -Iplugins/latency_mitigator/include -Iplugins/combat_meter/include \
                   -I$(SHOTS_DIR) -I$(IMGUI_DIR) -DIMGUI_USER_CONFIG='"imconfig_screenshots.h"'
# GCC's -O2 truncation analysis flags snprintf calls in the app that the unit test
# build, at -O0, never sees.
SHOTS_FLAGS = $(SHOTS_BASE_FLAGS) -Wall -Wextra -Wpedantic -Wno-format-truncation
SHOTS_WIN32_SRCS = src/payload/overlay_host.cpp \
                   plugins/combat_meter/src/combat_overlay.cpp \
                   plugins/latency_mitigator/src/latency_overlay.cpp \
                   $(SHOTS_DIR)/overlay_scene.cpp
SHOTS_IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
                   $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
# The plugins' payload glue (combat_plugin.cpp, latency_plugin.cpp) stays out: it
# needs the game's hooks, and the tool drives the engine and the overlays directly.
SHOTS_SRCS = $(COMMON_SRCS) $(APP_SRCS) \
             $(filter-out $(SHOTS_WIN32_SRCS) %/combat_plugin.cpp %/latency_plugin.cpp,$(PLUGIN_SRCS)) \
             $(filter-out $(SHOTS_WIN32_SRCS),$(wildcard $(SHOTS_DIR)/*.cpp))
SHOTS_OBJS = $(patsubst %.cpp,$(SHOTS_BUILD)/obj/%.o,$(SHOTS_SRCS))
SHOTS_WIN32_OBJS = $(patsubst %.cpp,$(SHOTS_BUILD)/obj/win32/%.o,$(SHOTS_WIN32_SRCS))
SHOTS_IMGUI_OBJS = $(patsubst %.cpp,$(SHOTS_BUILD)/obj/%.o,$(SHOTS_IMGUI_SRCS))

screenshots: $(SHOTS_BUILD)/hub_screenshots $(SHOTS_WINDOWS_DIR)/Fonts/segoeui.ttf
	$(SHOTS_BUILD)/hub_screenshots --windows-dir $(SHOTS_WINDOWS_DIR) --out docs/images

$(SHOTS_BUILD)/hub_screenshots: $(SHOTS_OBJS) $(SHOTS_WIN32_OBJS) $(SHOTS_IMGUI_OBJS)
	$(CXX) $^ -o $@ -lz -pthread

$(SHOTS_IMGUI_OBJS): $(SHOTS_BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SHOTS_BASE_FLAGS) -w -c $< -o $@

$(SHOTS_WIN32_OBJS): $(SHOTS_BUILD)/obj/win32/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SHOTS_FLAGS) -I$(SHOTS_DIR)/shim -include $(SHOTS_DIR)/win32_mode.hpp -c $< -o $@

$(SHOTS_OBJS): $(SHOTS_BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SHOTS_FLAGS) -DHAVE_IMGUI=1 -include $(IMGUI_DIR)/imgui.h -c $< -o $@

$(SHOTS_BUILD)/windows/Fonts/segoeui.ttf:
	@mkdir -p $(SHOTS_BUILD)/windows/Fonts
	curl -fsSL -o $(SHOTS_BUILD)/winstrap.tgz $(SELAWIK_PACKAGE)
	echo "$(SELAWIK_SHA256)  $(SHOTS_BUILD)/winstrap.tgz" | shasum -a 256 -c -
	tar -xzf $(SHOTS_BUILD)/winstrap.tgz -C $(SHOTS_BUILD) package/dist/fonts
	cp $(SHOTS_BUILD)/package/dist/fonts/selawkb.ttf $(SHOTS_BUILD)/windows/Fonts/segoeuib.ttf
	cp $(SHOTS_BUILD)/package/dist/fonts/selawksb.ttf $(SHOTS_BUILD)/windows/Fonts/seguisb.ttf
	cp $(SHOTS_BUILD)/package/dist/fonts/selawk.ttf $(SHOTS_BUILD)/windows/Fonts/segoeui.ttf

-include $(SHOTS_OBJS:.o=.d) $(SHOTS_WIN32_OBJS:.o=.d)

clean:
	rm -f hub_test_runner *.o
	rm -rf build/test build/tsan build/asan build/check-ui $(SHOTS_BUILD)

FORCE:

.PHONY: all test tsan asan clean check-ui check-layers screenshots FORCE
