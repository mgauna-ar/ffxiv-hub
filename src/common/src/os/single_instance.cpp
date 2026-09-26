#include "common/os/single_instance.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unordered_set>
#include <mutex>
#endif

namespace hub::os {

#ifndef _WIN32
namespace {
std::mutex g_mock_mutex;
std::unordered_set<std::string> g_mock_active_instances;
} // namespace
#endif

SingleInstance::SingleInstance(std::string mutex_name)
    : m_name(std::move(mutex_name)) {
#ifdef _WIN32
    m_activation_msg_id = RegisterWindowMessageW(ACTIVATION_MESSAGE_NAME);
#else
    m_activation_msg_id = 0xBEEF;
#endif
}

SingleInstance::~SingleInstance() {
    release();
}

SingleInstance::SingleInstance(SingleInstance&& other) noexcept
    : m_name(std::move(other.m_name)),
      m_handle(std::move(other.m_handle)),
      m_is_primary(other.m_is_primary),
      m_activation_msg_id(other.m_activation_msg_id) {
    other.m_is_primary = false;
}

SingleInstance& SingleInstance::operator=(SingleInstance&& other) noexcept {
    if (this != &other) {
        release();
        m_name = std::move(other.m_name);
        m_handle = std::move(other.m_handle);
        m_is_primary = other.m_is_primary;
        m_activation_msg_id = other.m_activation_msg_id;
        other.m_is_primary = false;
    }
    return *this;
}

bool SingleInstance::try_acquire() {
    if (m_is_primary) return true;

#ifdef _WIN32
    std::string full_name = "Local\\" + m_name;
    HANDLE created = CreateMutexA(nullptr, TRUE, full_name.c_str());
    const DWORD error = GetLastError();

    if (created == nullptr || error == ERROR_ALREADY_EXISTS) {
        // Failed, or another instance owns the mutex: only drop our handle to it.
        const UniqueHandle discard(created);
        m_is_primary = false;
        return false;
    }
    m_handle.reset(created);

    m_is_primary = true;
    return true;
#else
    std::lock_guard<std::mutex> lock(g_mock_mutex);
    if (g_mock_active_instances.find(m_name) != g_mock_active_instances.end()) {
        m_is_primary = false;
        return false;
    }
    g_mock_active_instances.insert(m_name);
    m_is_primary = true;
    return true;
#endif
}

void SingleInstance::notify_existing_instance() {
#ifdef _WIN32
    if (m_activation_msg_id != 0) {
        PostMessageW(HWND_BROADCAST, m_activation_msg_id, 0, 0);
    }
#endif
}

void SingleInstance::release() {
    if (!m_is_primary && !m_handle) return;

#ifdef _WIN32
    if (m_handle) {
        ReleaseMutex(static_cast<HANDLE>(m_handle.get()));
        m_handle.reset();
    }
#else
    std::lock_guard<std::mutex> lock(g_mock_mutex);
    g_mock_active_instances.erase(m_name);
#endif

    m_is_primary = false;
}

} // namespace hub::os
