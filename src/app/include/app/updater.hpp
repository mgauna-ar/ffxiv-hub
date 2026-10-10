#pragma once

#include "app/update_check.hpp"
#include "common/os/http_client.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace hub::app {

enum class UpdateState : uint8_t {
    /// Nothing checked yet.
    Idle,
    Checking,
    UpToDate,
    /// A newer release was found.
    Available,
    Downloading,
    /// Downloaded, verified and staged, for apply_staged() on the main thread.
    Ready,
    /// The new files are in place; the running build is the old one until a restart.
    RestartPending,
    /// The last check or install failed, for the reason in UpdateStatus::error.
    Failed,
};

struct UpdateStatus {
    UpdateState state{UpdateState::Idle};
    /// The newer release, once a check found one.
    std::optional<ReleaseInfo> release;
    /// While Downloading, 0 to 1. Stays 0 when the size is unknown.
    float progress{0.0f};
    std::string error;

    /// A check or an install is under way.
    [[nodiscard]] bool busy() const noexcept {
        return state == UpdateState::Checking || state == UpdateState::Downloading || state == UpdateState::Ready;
    }
    /// install() would start.
    [[nodiscard]] bool can_install() const noexcept {
        return release && release->installable() &&
               (state == UpdateState::Available || state == UpdateState::Failed);
    }
};

/// The new build is started with this flag and the PID of the one that installed
/// it, which it waits on before taking the single-instance lock.
inline constexpr std::string_view RELAUNCH_ARG = "--updated-from";

/// Where install() stages the new files: a folder inside `install_dir`, so moving
/// them into place is a rename on the same volume.
[[nodiscard]] std::filesystem::path staging_dir(const std::filesystem::path& install_dir);

/// Renames `from` to `to`; false when it could not.
using RenameFn = std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)>;

/// Moves each of UPDATE_FILES from `staging` into `install_dir`, renaming the live
/// file aside to "<name>.<backup_tag>.old" first: a running exe, or a DLL the game
/// has mapped, can be renamed but not overwritten or deleted. All or nothing: a
/// failure undoes every step already taken. Empty on success, else what failed.
/// `rename_fn` replaces the filesystem's, for tests.
[[nodiscard]] std::string apply_staged_update(const std::filesystem::path& install_dir,
                                              const std::filesystem::path& staging,
                                              std::string_view backup_tag, const RenameFn& rename_fn = {});

/// Removes the staging folder and the backups apply_staged_update() leaves. Best
/// effort: a DLL backup a running game still has mapped stays for a later start.
void remove_update_leftovers(const std::filesystem::path& install_dir);

/**
 * @brief Finds, downloads and installs a newer release of the Hub.
 *
 * Checks and downloads run on a worker thread, one at a time. Every member but
 * state() and status() is called from one thread, the app's main thread.
 */
class Updater {
public:
    using Fetch = std::function<std::optional<os::HttpResponse>(const os::HttpRequest&, std::string*)>;

    /// How long after the first tick the one automatic check runs.
    static constexpr std::chrono::seconds kStartupCheckDelay{10};
    static constexpr size_t kMaxApiBytes = 1024 * 1024;
    static constexpr size_t kMaxDownloadBytes = 64 * 1024 * 1024;
    static constexpr size_t kMaxFileBytes = 64 * 1024 * 1024;

    /// `folder` holds the running exe and the payload DLL.
    explicit Updater(std::filesystem::path folder, Fetch fetch = &os::https_get);
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    /// Runs the startup check kStartupCheckDelay after the first tick, if
    /// `auto_check` holds then. Never again after that, whatever it found.
    void tick(std::chrono::steady_clock::time_point now, bool auto_check);

    /// Checks for a newer release now. Ignored while a check or install runs, and
    /// once one is installed.
    void check_now();

    /// Downloads the release found, checks its SHA-256, and stages its two files.
    /// Ignored unless UpdateStatus::can_install().
    void install();

    /// Once Ready: moves the staged files into place and goes to RestartPending.
    /// False, with the state Failed and the old files back, when that failed.
    [[nodiscard]] bool apply_staged();

    [[nodiscard]] UpdateState state() const noexcept { return m_state.load(); }
    [[nodiscard]] UpdateStatus status() const;
    [[nodiscard]] const std::filesystem::path& install_dir() const noexcept { return m_install_dir; }

    /// Cancels a running download and waits for the worker.
    void stop();

    /// Waits for the running check or install to finish.
    void wait_idle();

private:
    void start(std::function<void()> job);
    void run_check();
    void run_install(const ReleaseInfo& release);
    /// Downloads, verifies and extracts into the staging folder. Empty on success.
    [[nodiscard]] std::string stage(const ReleaseInfo& release);
    void set_state(UpdateState next);
    void fail(std::string error);

    std::filesystem::path m_install_dir;
    Fetch m_fetch;

    std::optional<std::chrono::steady_clock::time_point> m_first_tick;
    bool m_startup_check_done{false};

    mutable std::mutex m_mutex;
    UpdateStatus m_status;
    /// m_status.state, readable without the lock.
    std::atomic<UpdateState> m_state{UpdateState::Idle};

    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_busy{false};
    std::thread m_worker;
};

} // namespace hub::app
