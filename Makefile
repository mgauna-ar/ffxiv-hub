CXX ?= clang++
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

test: hub_test_runner check-ui
	./hub_test_runner

hub_test_runner: $(COMMON_SRCS) $(PLUGIN_SRCS) $(PAYLOAD_SRCS) $(APP_SRCS) $(TEST_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

check-ui:
	@if [ -f $(IMGUI_DIR)/imgui.h ]; then \
		for f in $(UI_CHECK_SRCS); do \
			$(CXX) $(UI_CHECK_FLAGS) -fsyntax-only $$f || exit 1; \
		done; \
		echo "UI syntax check: OK"; \
	else \
		echo "UI syntax check: skipped (no vendored ImGui)"; \
	fi

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
	rm -rf $(SHOTS_BUILD)

.PHONY: all test clean check-ui screenshots
