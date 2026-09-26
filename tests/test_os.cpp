#include "test_framework.hpp"
#include "common/os/single_instance.hpp"
#include "common/os/auto_start.hpp"
#include "common/os/logger.hpp"
#include "common/os/safe_memory.hpp"
#include "common/os/local_time.hpp"
#include "common/os/network_monitor.hpp"
#include "common/os/paths.hpp"
#include "common/os/unload_marker.hpp"
#include "common/config/config_manager.hpp"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace hub::os;

namespace {

/// A folder of its own under the system temp folder, removed with everything in
/// it when the test ends.
struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(const char* name)
        : path(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Polls `done` until it holds or `timeout` passes; the monitor's worker runs on
/// its own clock.
template <typename Done>
bool wait_for(Done done, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace

TEST_CASE(OS, SingleInstanceGuard) {
    SingleInstance inst1("TestHubMutex1");
    TEST_ASSERT_TRUE(inst1.try_acquire());
    TEST_ASSERT_TRUE(inst1.is_primary());

    // Duplicate instance with same mutex name should fail
    SingleInstance inst2("TestHubMutex1");
    TEST_ASSERT_FALSE(inst2.try_acquire());
    TEST_ASSERT_FALSE(inst2.is_primary());

    // Releasing primary allows acquisition
    inst1.release();
    TEST_ASSERT_FALSE(inst1.is_primary());

    TEST_ASSERT_TRUE(inst2.try_acquire());
    TEST_ASSERT_TRUE(inst2.is_primary());
    inst2.release();
}

TEST_CASE(OS, UnloadMarkIsPerProcess) {
    TEST_ASSERT(!payload_marked_unloaded(424242));
    mark_payload_unloaded(424242);
    TEST_ASSERT(payload_marked_unloaded(424242));
    // A restarted game is another process and starts unmarked.
    TEST_ASSERT(!payload_marked_unloaded(424243));
}

TEST_CASE(OS, AutoStartConfiguration) {
    const std::string dummy_exe = "C:\\Tools\\ffxiv-hub.exe";
    const bool initially_enabled = AutoStart::is_enabled();

    // Set auto start
    TEST_ASSERT_TRUE(AutoStart::set_enabled(true, dummy_exe));
    TEST_ASSERT_TRUE(AutoStart::is_enabled());

    // Disable auto start
    TEST_ASSERT_TRUE(AutoStart::set_enabled(false));
    TEST_ASSERT_FALSE(AutoStart::is_enabled());

    // Restore
    AutoStart::set_enabled(initially_enabled);
}

TEST_CASE(OS, LoggerRotationAndWriting) {
    const std::string test_log = "./test_hub.log";
    TEST_ASSERT_TRUE(Logger::init(test_log, true));

    Logger::info("Test info message from unit test");
    Logger::warn("Test warning message");
    Logger::error("Test error message");

    Logger::shutdown();

    TEST_ASSERT_TRUE(std::filesystem::exists(test_log));

    // Cleanup test log
    std::error_code ec;
    std::filesystem::remove(test_log, ec);
    std::filesystem::remove(Logger::previous_log_path(test_log), ec);
}

TEST_CASE(OS, SafeCopyReadsAndRejectsNull) {
    const uint32_t source = 0xCAFEF00D;
    uint32_t copy = 0;
    TEST_ASSERT_TRUE(safe_read(&source, copy));
    TEST_ASSERT_EQ(copy, 0xCAFEF00Du);

    // An address held as an integer, as the payload keeps its resolved globals.
    copy = 0;
    TEST_ASSERT_TRUE(safe_read(reinterpret_cast<uintptr_t>(&source), copy));
    TEST_ASSERT_EQ(copy, 0xCAFEF00Du);

    // An unresolved signature leaves the address at 0: a failed read, not a fault.
    TEST_ASSERT_FALSE(safe_read(uintptr_t{0}, copy));
    TEST_ASSERT_FALSE(safe_copy(nullptr, &source, sizeof(source)));
    TEST_ASSERT_FALSE(safe_copy(&copy, nullptr, sizeof(copy)));
}

TEST_CASE(OS, PreviousLogPathGoesBeforeTheExtension) {
    const std::filesystem::path dir = std::filesystem::path("logs");
    TEST_ASSERT_EQ(Logger::previous_log_path(dir / "hub.log"), dir / "hub.prev.log");
    TEST_ASSERT_EQ(Logger::previous_log_path(dir / "trace.txt"), dir / "trace.prev.txt");
    TEST_ASSERT_EQ(Logger::previous_log_path(dir / "hub"), dir / "hub.prev");
}

TEST_CASE(OS, LoggerRotatesTheLastSessionOnInit) {
    const ScratchDir scratch("hub_os_logger_rotate");
    const auto log = scratch.path / "hub.log";
    const auto prev = Logger::previous_log_path(log);
    write_file(log, "last session\n");
    write_file(prev, "the session before\n");

    TEST_ASSERT_TRUE(Logger::init(log, true));
    Logger::info("first line");
    Logger::warn("second line");
    Logger::error("third line");
    Logger::shutdown();

    // The last session replaces the one before it, and the new log starts over.
    TEST_ASSERT_EQ(read_file(prev), std::string("last session\n"));
    const std::string text = read_file(log);
    TEST_ASSERT_TRUE(text.find("last session") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("FFXIV Hub Diagnostic Log Started") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("[INFO ] first line") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("[WARN ] second line") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("[ERROR] third line") != std::string::npos);
}

TEST_CASE(OS, LoggerKeepsThePreviousLogOverAnEmptyOne) {
    const ScratchDir scratch("hub_os_logger_empty");
    const auto log = scratch.path / "hub.log";
    const auto prev = Logger::previous_log_path(log);
    write_file(log, "");
    write_file(prev, "worth keeping\n");

    TEST_ASSERT_TRUE(Logger::init(log, true));
    Logger::shutdown();

    // An empty log is not a session: rotating it would lose the one before.
    TEST_ASSERT_EQ(read_file(prev), std::string("worth keeping\n"));
}

TEST_CASE(OS, LoggerWithoutRotationTruncatesInPlace) {
    const ScratchDir scratch("hub_os_logger_norotate");
    const auto log = scratch.path / "hub.log";
    write_file(log, "overwritten\n");

    TEST_ASSERT_TRUE(Logger::init(log, false));
    Logger::shutdown();

    TEST_ASSERT_FALSE(std::filesystem::exists(Logger::previous_log_path(log)));
    TEST_ASSERT_TRUE(read_file(log).find("overwritten") == std::string::npos);
}

TEST_CASE(OS, LoggerCreatesMissingFolders) {
    const ScratchDir scratch("hub_os_logger_folders");
    const auto log = scratch.path / "a" / "b" / "hub.log";

    TEST_ASSERT_TRUE(Logger::init(log, true));
    TEST_ASSERT_EQ(Logger::log_file_path(), log);
    Logger::shutdown();
    TEST_ASSERT_TRUE(std::filesystem::exists(log));
}

TEST_CASE(OS, DefaultLogSitsBesideTheConfig) {
    const ScratchDir scratch("hub_os_default_log");
    auto& cfg = hub::config::ConfigManager::instance();
    cfg.set_custom_path_for_testing(scratch.path / "config.json");

    TEST_ASSERT_EQ(Logger::default_log_path(), scratch.path / "hub.log");

    cfg.set_custom_path_for_testing({});
}

TEST_CASE(OS, ConfigPathIsInTheAppDataFolder) {
    const std::filesystem::path dir = app_data_dir();
    const std::filesystem::path expected = dir.empty() ? std::filesystem::path("config.json") : dir / "config.json";
    if (!dir.empty()) {
        TEST_ASSERT_EQ(dir.filename(), std::filesystem::path("ffxiv-hub"));
    }

    hub::config::ConfigManager cfg;
    TEST_ASSERT_EQ(cfg.get_config_path(), expected);

    // A custom path wins until it is cleared.
    const auto custom = std::filesystem::temp_directory_path() / "hub_os_custom_config.json";
    cfg.set_custom_path_for_testing(custom);
    TEST_ASSERT_EQ(cfg.get_config_path(), custom);
    cfg.set_custom_path_for_testing({});
    TEST_ASSERT_EQ(cfg.get_config_path(), expected);
}

TEST_CASE(OS, LocalTimeRoundTrips) {
    const std::time_t t = 1700000000;
    std::tm tm = local_time(t);
    TEST_ASSERT_TRUE(tm.tm_year >= 123);
    TEST_ASSERT_EQ(std::mktime(&tm), t);
}

TEST_CASE(OS, NetworkMonitorLifecycle) {
    NetworkMonitor monitor;
    TEST_ASSERT_FALSE(monitor.is_running());
    TEST_ASSERT_EQ(monitor.target_pid(), 0u);
    TEST_ASSERT_EQ(monitor.get_current_ping_ms(), -1.0);

    monitor.set_probe_interval_ms(10);
    monitor.start(4242);
    TEST_ASSERT_TRUE(monitor.is_running());
    TEST_ASSERT_EQ(monitor.target_pid(), 4242u);

    // A second start keeps the running worker and its target.
    monitor.start();
    TEST_ASSERT_TRUE(monitor.is_running());
    TEST_ASSERT_EQ(monitor.target_pid(), 4242u);

#ifndef _WIN32
    // The mock reports a fixed ping for any target, and keeps one set by hand.
    TEST_ASSERT_TRUE(wait_for([&] { return monitor.get_current_ping_ms() == 32.0; }));
    monitor.set_mock_ping(12.5);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    TEST_ASSERT_EQ(monitor.get_current_ping_ms(), 12.5);
#endif

    // Losing the target clears the ping, and the worker keeps it cleared.
    monitor.set_target_pid(0);
    TEST_ASSERT_EQ(monitor.get_current_ping_ms(), -1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_ASSERT_EQ(monitor.get_current_ping_ms(), -1.0);
    TEST_ASSERT_TRUE(monitor.is_running());

    monitor.stop();
    TEST_ASSERT_FALSE(monitor.is_running());
    TEST_ASSERT_EQ(monitor.get_current_ping_ms(), -1.0);
    monitor.stop();
    TEST_ASSERT_FALSE(monitor.is_running());

    // It starts again after a stop.
    monitor.start(4243);
    TEST_ASSERT_TRUE(monitor.is_running());
    TEST_ASSERT_EQ(monitor.target_pid(), 4243u);
    monitor.stop();
}

TEST_CASE(OS, NetworkMonitorStopsWhenDestroyed) {
    auto monitor = std::make_unique<NetworkMonitor>(4242);
    monitor->set_probe_interval_ms(10);
    monitor->start();
    TEST_ASSERT_TRUE(monitor->is_running());
    // The destructor joins the worker; a detached or unjoined thread would abort here.
    monitor.reset();
    TEST_ASSERT_TRUE(monitor == nullptr);
}
