#pragma once

#if defined(_MSC_VER)
#pragma warning(disable: 4127) // conditional expression is constant
#endif

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace hub::test {

template <typename T>
concept OStreamable = requires(std::ostream& os, const T& val) {
    os << val;
};

template <typename T>
std::string format_val(const T& val) {
    if constexpr (OStreamable<T>) {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<std::underlying_type_t<T>>(val));
    } else {
        return "<unprintable>";
    }
}

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> func;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }

    void add_test(std::string suite, std::string name, std::function<void()> func) {
        m_tests.push_back(TestCase{std::move(suite), std::move(name), std::move(func)});
    }

    [[nodiscard]] const std::vector<TestCase>& tests() const {
        return m_tests;
    }

private:
    std::vector<TestCase> m_tests;
};

struct TestRegistrar {
    TestRegistrar(std::string suite, std::string name, std::function<void()> func) {
        TestRegistry::instance().add_test(std::move(suite), std::move(name), std::move(func));
    }
};

#define TEST_CASE(suite_name, test_name) \
    static void test_##suite_name##_##test_name(); \
    static ::hub::test::TestRegistrar registrar_##suite_name##_##test_name( \
        #suite_name, #test_name, &test_##suite_name##_##test_name \
    ); \
    static void test_##suite_name##_##test_name()

#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: (" #condition ") at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (false)

#define TEST_ASSERT_TRUE(condition) TEST_ASSERT(condition)
#define TEST_ASSERT_FALSE(condition) TEST_ASSERT(!(condition))

#define TEST_ASSERT_NEAR(actual, expected, tolerance) \
    do { \
        if (std::abs((actual) - (expected)) > (tolerance)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: |" << (actual) << " - " << (expected) \
                << "| > " << (tolerance) << " at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (false)

#define TEST_ASSERT_EQ(actual, expected) \
    do { \
        if ((actual) != (expected)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: (" #actual " == " #expected ") [" \
                << ::hub::test::format_val(actual) << " != " << ::hub::test::format_val(expected) \
                << "] at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (false)

} // namespace hub::test
