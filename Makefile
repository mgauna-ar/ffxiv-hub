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
UI_CHECK_FLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Werror \
                 -Iinclude -Isrc -Iplugins -Iplugins/latency_mitigator/include -Iplugins/combat_meter/include \
                 -I$(IMGUI_DIR) -DHAVE_IMGUI=1 -include $(IMGUI_DIR)/imgui.h \
                 -Wno-nontrivial-memcall

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

clean:
	rm -f hub_test_runner *.o

.PHONY: all test clean check-ui
