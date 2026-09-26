#pragma once

#include <string>
#include <functional>
#include <cstdint>

#ifdef _WIN32
#define HUB_CALLBACK __stdcall
#else
#define HUB_CALLBACK
#endif

namespace hub::os {

/**
 * @brief System Tray (Taskbar Notification Area) Manager for FFXIV Hub.
 *
 * Strictly decoupled and agnostic to specific plugins.
 * Manages the Hub window visibility, Windows startup registration,
 * diagnostic folder opening, and clean application exit.
 */
class TrayManager {
public:
    enum class CommandId : uint32_t {
        ShowHubWindow = 1000,
        ToggleAutoStart = 1001,
        OpenConfig = 1002,
        OpenLogs = 1003,
        Exit = 1004
    };

    TrayManager();
    ~TrayManager();

    TrayManager(const TrayManager&) = delete;
    TrayManager& operator=(const TrayManager&) = delete;

    /// Initializes hidden message window and creates tray icon
    bool initialize(uint32_t activation_msg_id = 0);

    /// Destroys message window and removes tray icon
    void shutdown();

    /// The tooltip's status line, as the app words it. The tray re-draws the tooltip
    /// only when the text changes, so it can be called every frame.
    void set_status(std::string status);

    /// Auto-start toggle state in menu
    void set_auto_start(bool enabled) noexcept { m_auto_start = enabled; }
    [[nodiscard]] bool is_auto_start() const noexcept { return m_auto_start; }

    /// Notification visibility toggle
    void set_notifications_enabled(bool enabled) noexcept { m_notifications_enabled = enabled; }
    [[nodiscard]] bool notifications_enabled() const noexcept { return m_notifications_enabled; }

    /// Shows native Windows balloon/toast notification if enabled
    void show_notification(const std::string& title, const std::string& message);

    /// Callback setters
    void set_on_show_window(std::function<void()> cb) { m_on_show_window = std::move(cb); }
    void set_on_toggle_auto_start(std::function<void(bool)> cb) { m_on_toggle_auto_start = std::move(cb); }
    void set_on_open_config(std::function<void()> cb) { m_on_open_config = std::move(cb); }
    void set_on_open_logs(std::function<void()> cb) { m_on_open_logs = std::move(cb); }
    void set_on_exit(std::function<void()> cb) { m_on_exit = std::move(cb); }

    /// Executes command by ID (invoked by WndProc or unit tests)
    void handle_command(CommandId cmd);

    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }
    [[nodiscard]] std::string status_string() const;

    /// Pumps Win32 messages for message window
    void pump_messages();

private:
    void update_tray_icon();
    void show_context_menu();

    [[maybe_unused]] void* m_hwnd{nullptr};
    [[maybe_unused]] void* m_icon{nullptr};
    [[maybe_unused]] uint32_t m_activation_msg_id{0};
    bool m_initialized{false};

    std::string m_status;

    bool m_auto_start{false};
    bool m_notifications_enabled{true};

    std::function<void()> m_on_show_window;
    std::function<void(bool)> m_on_toggle_auto_start;
    std::function<void()> m_on_open_config;
    std::function<void()> m_on_open_logs;
    std::function<void()> m_on_exit;

    static intptr_t HUB_CALLBACK static_wnd_proc(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam);
};

} // namespace hub::os
