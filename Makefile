CXX ?= clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -Iplugins -Iplugins/latency_mitigator/include -Itests

COMMON_SRCS = $(wildcard src/common/*.cpp) \
              $(wildcard src/common/ipc/*.cpp) \
              $(wildcard src/common/config/*.cpp) \
              $(wildcard src/common/os/*.cpp)

PLUGIN_SRCS = $(wildcard plugins/latency_mitigator/src/*.cpp)

TEST_SRCS = $(wildcard tests/*.cpp)

all: test

test: hub_test_runner
	./hub_test_runner

hub_test_runner: $(COMMON_SRCS) $(PLUGIN_SRCS) $(TEST_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -f hub_test_runner *.o

.PHONY: all test clean
