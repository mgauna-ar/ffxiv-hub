#include "test_framework.hpp"
#include "common/os/single_instance.hpp"
#include "common/os/auto_start.hpp"
#include "common/os/logger.hpp"
#include <filesystem>

using namespace hub::os;

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
