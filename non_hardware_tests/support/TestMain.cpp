#include "TestHarness.hpp"

#include <iostream>

int main()
{
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
