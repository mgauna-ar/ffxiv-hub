#pragma once

#include <string>
#include <cstdint>

namespace hub::os {

/**
 * @brief Ensures single instance execution via a named Win32 mutex and wakes existing instance if duplicate.
 */
class SingleInstance {
public:
    static constexpr const char* DEFAULT_MUTEX_NAME = "FFXIVHubSingleInstanceMutex";
    static constexpr const wchar_t* ACTIVATION_MESSAGE_NAME = L"FFXIV_HUB_ACTIVATE";

    explicit SingleInstance(std::string mutex_name = DEFAULT_MUTEX_NAME);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    SingleInstance(SingleInstance&&) noexcept;
    SingleInstance& operator=(SingleInstance&&) noexcept;

    /// Attempts to acquire the single-instance lock. Returns true if this is the primary instance.
    bool try_acquire();

    /// Returns whether this instance holds the primary lock.
    [[nodiscard]] bool is_primary() const noexcept { return m_is_primary; }

    /// Sends activation message to existing instance to unhide/bring forward.
    void notify_existing_instance();

    /// Releases the mutex handle.
    void release();

    /// Returns the registered Win32 window message ID for instance activation.
    [[nodiscard]] uint32_t activation_message_id() const noexcept { return m_activation_msg_id; }

private:
    std::string m_name;
    void* m_handle{nullptr};
    bool m_is_primary{false};
    uint32_t m_activation_msg_id{0};
};

} // namespace hub::os
