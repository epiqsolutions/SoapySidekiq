/**
 * @file TxCallbackInitializationTests.cpp
 * @brief Injects failures into the production TX callback registration transaction.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/ScopeExit.hpp>

#include <cstdint>
#include <string>

struct skiq_tx_block_t
{
};
namespace
{
enum class Failure
{
    none,
    sdk,
    logging
};
struct State
{
    Failure fail{};
    bool callback{};
    bool invalid_cleanup{};
    std::string events;
} state;

using Callback = void (*)(int32_t, skiq_tx_block_t *, void *);
int skiq_register_tx_complete_callback(std::uint8_t, Callback callback)
{
    if (callback != nullptr)
    {
        state.events += "S";
        if (state.fail == Failure::sdk)
            return -17;
        state.callback = true;
    }
    else
    {
        state.events += "s";
        state.invalid_cleanup |= !state.callback;
        state.callback = false;
    }
    return 0;
}
constexpr int SOAPY_SDR_TRACE = 0;
void SoapySDR_logf(int, const char *, ...)
{
    state.events += "L";
    if (state.fail == Failure::logging)
        throw std::runtime_error("injected log failure");
}
void reset(Failure failure)
{
    state = {};
    state.fail = failure;
    state.events.reserve(64);
}
void requireReleased(const std::string &events)
{
    REQUIRE_EQ(state.events, events);
    REQUIRE(!state.callback);
    REQUIRE(!state.invalid_cleanup);
}
} // namespace

class SoapySidekiq
{
  public:
    std::uint8_t card{3};
    static void tx_complete_callback(int32_t, skiq_tx_block_t *, void *) {}
    void initializeTxCallbacks();
};

// Only the API calls are replaced; execute the actual production method body.
#include "TxCallbackInitialization.inc"

TEST_CASE("TX callback setup reports SDK failure without unregistering an unacquired callback")
{
    reset(Failure::sdk);
    SoapySidekiq device;
    try
    {
        device.initializeTxCallbacks();
        REQUIRE(false);
    }
    catch (const std::runtime_error &error)
    {
        REQUIRE(std::string(error.what()).find("card 3, status -17") != std::string::npos);
    }
    requireReleased("S");
}

TEST_CASE("TX callback setup unregisters the callback if final logging throws")
{
    reset(Failure::logging);
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.initializeTxCallbacks(), std::runtime_error);
    requireReleased("SLs");
}

TEST_CASE("successful TX callback setup transfers resources to the device")
{
    reset(Failure::none);
    SoapySidekiq device;
    device.initializeTxCallbacks();
    REQUIRE_EQ(state.events, std::string("SL"));
    REQUIRE(state.callback);
    REQUIRE(!state.invalid_cleanup);
}

TEST_CASE("failed TX callback setup can be retried without leftover registration")
{
    reset(Failure::logging);
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.initializeTxCallbacks(), std::runtime_error);
    requireReleased("SLs");
    state.events.clear();
    state.fail = Failure::none;
    device.initializeTxCallbacks();
    REQUIRE_EQ(state.events, std::string("SL"));
    REQUIRE(state.callback);
    REQUIRE(!state.invalid_cleanup);
}
