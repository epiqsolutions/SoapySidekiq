/**
 * @file TestHarness.hpp
 * @brief Provides a dependency-free test registry and assertion macros.
 */

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

/** Name and callable registered by one TEST_CASE declaration. */
struct TestCase
{
    std::string name;
    std::function<void()> function;
};

/**
 * Access the process-wide registry shared by the lightweight test runner.
 * @return Mutable registry populated during static initialization.
 */
inline std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

/** Adds a test callable to registry() during static initialization. */
class Registration
{
public:
    /** Register one named test callable for execution by TestMain.cpp. */
    Registration(std::string name, std::function<void()> function)
    {
        registry().push_back({std::move(name), std::move(function)});
    }
};

/** Exception used to distinguish assertion failures from test crashes. */
class Failure : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * Throw a source-located Failure when an expression is false.
 * @param condition Evaluated assertion result.
 * @param expression Source text of the asserted expression.
 * @param file Source file containing the assertion.
 * @param line Source line containing the assertion.
 */
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

/**
 * Compare printable values and report both sides on failure.
 * @tparam Actual Actual value type.
 * @tparam Expected Expected value type.
 */
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

// TEST_CASE declares and registers a uniquely named static test function.
#define TEST_CONCAT_IMPL(left, right) left##right
#define TEST_CONCAT(left, right) TEST_CONCAT_IMPL(left, right)
#define TEST_CASE(name) TEST_CASE_IMPL(name, __COUNTER__)
#define TEST_CASE_IMPL(name, id)                                                   \
    static void TEST_CONCAT(test_function_, id)();                                \
    static ::test_harness::Registration TEST_CONCAT(test_registration_, id)(      \
        name, TEST_CONCAT(test_function_, id));                                   \
    static void TEST_CONCAT(test_function_, id)()

// Assertion macros capture the original expression and call-site location.
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
