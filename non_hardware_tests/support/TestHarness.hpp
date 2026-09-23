#pragma once

#include <exception>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_harness
{

struct TestCase
{
    std::string name;
    std::function<void()> function;
};

inline std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

class Registration
{
public:
    Registration(std::string name, std::function<void()> function)
    {
        registry().push_back({std::move(name), std::move(function)});
    }
};

class Failure : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

inline void require(
    const bool condition,
    const char *expression,
    const char *file,
    const int line)
{
    if (!condition)
    {
        std::ostringstream message;
        message << file << ':' << line << ": requirement failed: " << expression;
        throw Failure(message.str());
    }
}

template <typename Actual, typename Expected>
void requireEqual(
    const Actual &actual,
    const Expected &expected,
    const char *actual_expression,
    const char *expected_expression,
    const char *file,
    const int line)
{
    if (!(actual == expected))
    {
        std::ostringstream message;
        message << file << ':' << line << ": expected " << actual_expression
                << " == " << expected_expression << ", but got " << actual
                << " and " << expected;
        throw Failure(message.str());
    }
}

} // namespace test_harness

#define TEST_CONCAT_IMPL(left, right) left##right
#define TEST_CONCAT(left, right) TEST_CONCAT_IMPL(left, right)
#define TEST_CASE(name) TEST_CASE_IMPL(name, __COUNTER__)
#define TEST_CASE_IMPL(name, id)                                                   \
    static void TEST_CONCAT(test_function_, id)();                                \
    static ::test_harness::Registration TEST_CONCAT(test_registration_, id)(      \
        name, TEST_CONCAT(test_function_, id));                                   \
    static void TEST_CONCAT(test_function_, id)()

#define REQUIRE(expression)                                                        \
    ::test_harness::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define REQUIRE_EQ(actual, expected)                                               \
    ::test_harness::requireEqual(                                                  \
        (actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define REQUIRE_THROWS_AS(expression, exception_type)                              \
    do                                                                             \
    {                                                                              \
        bool caught_expected_exception = false;                                    \
        try                                                                        \
        {                                                                          \
            (void)(expression);                                                    \
        }                                                                          \
        catch (const exception_type &)                                             \
        {                                                                          \
            caught_expected_exception = true;                                      \
        }                                                                          \
        REQUIRE(caught_expected_exception);                                        \
    } while (false)
