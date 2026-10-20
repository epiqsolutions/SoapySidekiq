/**
 * @file SdkLoggingTests.cpp
 * @brief Exercises the production SDK callback with a capturing or failing sink.
 */
#include "TestHarness.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
enum
{
    SKIQ_LOG_DEBUG = 1,
    SKIQ_LOG_INFO,
    SKIQ_LOG_WARNING,
    SKIQ_LOG_ERROR
};
enum
{
    SOAPY_SDR_TRACE = 10,
    SOAPY_SDR_DEBUG,
    SOAPY_SDR_INFO,
    SOAPY_SDR_WARNING,
    SOAPY_SDR_ERROR
};
struct Entry
{
    int level;
    std::string text;
};
std::vector<Entry> entries;
int sink_failure = 0;

void SoapySDR_logf(int level, const char *format, ...)
{
    if (sink_failure == 1)
        throw std::bad_alloc();
    if (sink_failure == 2)
        throw std::runtime_error("sink failed");
    va_list args;
    va_start(args, format);
    va_list count_args;
    va_copy(count_args, args);
    const int length = std::vsnprintf(nullptr, 0, format, count_args);
    va_end(count_args);
    std::vector<char> output(static_cast<std::size_t>(length) + 1);
    std::vsnprintf(output.data(), output.size(), format, args);
    va_end(args);
    entries.push_back({level, output.data()});
}
} // namespace

// Generated from the actual Settings.cpp callback, not a separate implementation.
#include "SdkLogCallback.inc"

TEST_CASE("SDK log priorities preserve their Soapy mapping and prefixes")
{
    const int priorities[]{SKIQ_LOG_DEBUG, SKIQ_LOG_INFO, SKIQ_LOG_WARNING, SKIQ_LOG_ERROR, -1};
    const int levels[]{SOAPY_SDR_DEBUG, SOAPY_SDR_INFO, SOAPY_SDR_WARNING, SOAPY_SDR_ERROR,
                       SOAPY_SDR_TRACE};
    entries.clear();
    for (int priority : priorities)
        logging_handler(priority, "message");
    REQUIRE_EQ(entries.size(), std::size_t{5});
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        REQUIRE_EQ(entries[i].level, levels[i]);
        REQUIRE_EQ(entries[i].text, i == 4 ? "epiq-log undefined message" : "epiq-log: message");
    }
}

TEST_CASE("SDK logging preserves the historical single LF then CR trimming")
{
    const char *inputs[]{"",       "\n",       "\r",       "\r\n",     "text\n",
                         "text\r", "text\r\n", "text\n\n", "text\n\r", "in\nside"};
    const char *expected[]{"", "", "", "", "text", "text", "text", "text\n", "text\n", "in\nside"};
    entries.clear();
    for (const auto *input : inputs)
        logging_handler(SKIQ_LOG_INFO, input);
    REQUIRE_EQ(entries.size(), std::size_t{10});
    for (std::size_t i = 0; i < entries.size(); ++i)
        REQUIRE_EQ(entries[i].text, std::string("epiq-log: ") + expected[i]);
}

TEST_CASE("null SDK log text is safely treated as an empty message")
{
    entries.clear();
    logging_handler(SKIQ_LOG_INFO, nullptr);
    REQUIRE_EQ(entries.size(), std::size_t{1});
    REQUIRE_EQ(entries[0].text, std::string("epiq-log: "));
}

TEST_CASE("SDK log input remains untouched and percent signs stay literal")
{
    entries.clear();
    char input[] = "100% %s %n\r\n";
    const std::string original(input);
    logging_handler(SKIQ_LOG_WARNING, input);
    REQUIRE_EQ(std::string(input), original);
    REQUIRE_EQ(entries.size(), std::size_t{1});
    REQUIRE_EQ(entries[0].text, std::string("epiq-log: 100% %s %n"));
}

TEST_CASE("long SDK log messages are preserved without fixed-size truncation")
{
    entries.clear();
    const std::string text(65536, 'x');
    logging_handler(SKIQ_LOG_INFO, (text + "\r\n").c_str());
    REQUIRE_EQ(entries.size(), std::size_t{1});
    REQUIRE_EQ(entries[0].text, "epiq-log: " + text);
}

TEST_CASE("allocation and sink failures cannot escape the SDK callback")
{
    entries.clear();
    for (int failure : {1, 2})
    {
        sink_failure = failure;
        logging_handler(SKIQ_LOG_ERROR, "SDK failure");
    }
    sink_failure = 0;
    REQUIRE(entries.empty());
    logging_handler(SKIQ_LOG_INFO, "recovered");
    REQUIRE_EQ(entries.size(), std::size_t{1});
}
