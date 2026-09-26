#include "common/os/unload_marker.hpp"

#ifdef _WIN32
#include "common/os/unique_handle.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#else
#include <mutex>
#include <set>
#endif

namespace hub::os {

#ifdef _WIN32
namespace {

std::wstring marker_name(uint32_t pid) {
    return L"Local\\ffxiv_hub_unloaded_" + std::to_wstring(pid);
}

} // namespace

void mark_payload_unloaded(uint32_t pid) {
    // Held until the process exits, which is when the mark has to go: a restarted
    // game has another pid and a fresh payload.
    static const UniqueHandle mark(CreateEventW(nullptr, TRUE, TRUE, marker_name(pid).c_str()));
}

bool payload_marked_unloaded(uint32_t pid) {
    const UniqueHandle mark(OpenEventW(SYNCHRONIZE, FALSE, marker_name(pid).c_str()));
    return static_cast<bool>(mark);
}
#else
namespace {

std::mutex g_mock_mutex;
std::set<uint32_t> g_mock_marked;

} // namespace

void mark_payload_unloaded(uint32_t pid) {
    std::lock_guard<std::mutex> lock(g_mock_mutex);
    g_mock_marked.insert(pid);
}

bool payload_marked_unloaded(uint32_t pid) {
    std::lock_guard<std::mutex> lock(g_mock_mutex);
    return g_mock_marked.count(pid) != 0;
}
#endif

} // namespace hub::os
