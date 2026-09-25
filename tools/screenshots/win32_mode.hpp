#pragma once

// Force-included into the translation units the screenshot tool builds as if on
// Windows: the payload's overlay host and the two in-game overlays, whose drawing
// code exists only under _WIN32. Every standard header those files reach is pulled
// in first, so the library never sees _WIN32. libstdc++ would otherwise give
// std::filesystem::path Windows semantics in these units alone.

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// ImGui's internals pull in the SSE intrinsics on x86, and Clang's mm_malloc.h
// takes _WIN32 to mean the MSVC allocator.
#if defined(__SSE__) || defined(__x86_64__)
#include <immintrin.h>
#endif

#define _WIN32 1
