/**
 * @file TestMain.cpp
 * @brief Runs all cases registered with the non-hardware test harness.
 */

#include "TestHarness.hpp"

#include <iostream>

/** Execute registered cases and return a conventional process status. */
int main()
{
    // Run every statically registered case so one failure does not hide others.
    std::size_t failures = 0;
    for (const auto &test : test_harness::registry())
    {
        try
        {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
        catch (...)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << test_harness::registry().size() - failures << " passed, "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
