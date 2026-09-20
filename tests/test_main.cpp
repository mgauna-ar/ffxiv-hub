#include "test_framework.hpp"
#include <iostream>
#include <iomanip>

int main() {
    const auto& tests = hub::test::TestRegistry::instance().tests();
    size_t passed = 0;
    size_t failed = 0;

    std::cout << "\n=======================================================\n";
    std::cout << "  FFXIV Hub Test Suite (" << tests.size() << " tests registered)\n";
    std::cout << "=======================================================\n\n";

    for (const auto& test : tests) {
        std::cout << "[" << std::left << std::setw(20) << test.suite << "] "
                  << std::setw(35) << test.name << " ... ";
        try {
            test.func();
            std::cout << "\033[32mPASS\033[0m\n";
            passed++;
        } catch (const std::exception& e) {
            std::cout << "\033[31mFAIL\033[0m\n";
            std::cerr << "    Error: " << e.what() << "\n";
            failed++;
        } catch (...) {
            std::cout << "\033[31mFAIL (Unknown Exception)\033[0m\n";
            failed++;
        }
    }

    std::cout << "\n-------------------------------------------------------\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed, "
              << tests.size() << " total.\n";
    std::cout << "-------------------------------------------------------\n\n";

    return (failed == 0) ? 0 : 1;
}
