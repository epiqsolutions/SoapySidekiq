/**
 * @file TxCallbackInitializationTests.cpp
 * @brief Injects failures into the production TX callback registration transaction.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/ScopeExit.hpp>

#include <cerrno>
#include <cstdint>
#include <new>
#include <pthread.h>
#include <string>
#include <system_error>

struct skiq_tx_block_t
{
};
namespace
{
enum class Failure
{
    none,
    mutex,
    condition,
    registry,
    sdk,
    logging
};
struct State
{
    Failure fail{};
    bool mutex{}, condition{}, registry{}, callback{};
    bool invalid_cleanup{};
    std::string events;
} state;

int fake_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *)
{
    state.events += "M";
    if (state.fail == Failure::mutex)
        return EAGAIN;
    state.mutex = true;
    return 0;
}
int fake_cond_init(pthread_cond_t *, const pthread_condattr_t *)
{
    state.events += "C";
    if (state.fail == Failure::condition)
        return EINVAL;
    state.condition = true;
    return 0;
}
int fake_mutex_destroy(pthread_mutex_t *)
{
    state.events += "m";
    state.invalid_cleanup |= !state.mutex || state.condition || state.registry || state.callback;
    state.mutex = false;
    return 0;
}
int fake_cond_destroy(pthread_cond_t *)
{
    state.events += "c";
    state.invalid_cleanup |= !state.condition || state.registry || state.callback;
    state.condition = false;
    return 0;
}
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
    REQUIRE(!state.mutex && !state.condition && !state.registry && !state.callback);
    REQUIRE(!state.invalid_cleanup);
}
} // namespace

class SoapySidekiq
{
  public:
    std::uint8_t card{3};
    pthread_mutex_t space_avail_mutex{};
    pthread_cond_t space_avail_cond{};
    static void tx_complete_callback(int32_t, skiq_tx_block_t *, void *) {}
    static void registerInstance(std::uint8_t, SoapySidekiq *)
    {
        state.events += "R";
        if (state.fail == Failure::registry)
            throw std::bad_alloc();
        state.registry = true;
    }
    static void unregisterInstance(std::uint8_t, SoapySidekiq *)
    {
        state.events += "r";
        state.invalid_cleanup |= !state.registry || state.callback;
        state.registry = false;
    }
    void initializeTxCallbacks();
};

// Only the API calls are replaced; execute the actual production method body.
#define pthread_mutex_init fake_mutex_init
#define pthread_cond_init fake_cond_init
#define pthread_mutex_destroy fake_mutex_destroy
#define pthread_cond_destroy fake_cond_destroy
#include "TxCallbackInitialization.inc"

TEST_CASE("TX callback setup stops immediately when mutex initialization fails")
{
    reset(Failure::mutex);
    SoapySidekiq device;
    try
    {
        device.initializeTxCallbacks();
        REQUIRE(false);
    }
    catch (const std::system_error &error)
    {
        REQUIRE_EQ(error.code().value(), EAGAIN);
        REQUIRE(std::string(error.what()).find("mutex") != std::string::npos);
    }
    requireReleased("M");
}

TEST_CASE("TX callback setup releases its mutex when condition initialization fails")
{
    reset(Failure::condition);
    SoapySidekiq device;
    try
    {
        device.initializeTxCallbacks();
        REQUIRE(false);
    }
    catch (const std::system_error &error)
    {
        REQUIRE_EQ(error.code().value(), EINVAL);
        REQUIRE(std::string(error.what()).find("condition") != std::string::npos);
    }
    requireReleased("MCm");
}

TEST_CASE("TX callback setup releases pthread resources when registry allocation throws")
{
    reset(Failure::registry);
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.initializeTxCallbacks(), std::bad_alloc);
    requireReleased("MCRcm");
}

TEST_CASE("TX callback setup releases registry and pthread resources on SDK failure")
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
    requireReleased("MCRSrcm");
}

TEST_CASE("TX callback setup unregisters the callback if final logging throws")
{
    reset(Failure::logging);
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.initializeTxCallbacks(), std::runtime_error);
    requireReleased("MCRSLsrcm");
}

TEST_CASE("successful TX callback setup transfers resources to the device")
{
    reset(Failure::none);
    SoapySidekiq device;
    device.initializeTxCallbacks();
    REQUIRE_EQ(state.events, std::string("MCRSL"));
    REQUIRE(state.mutex && state.condition && state.registry && state.callback);
    REQUIRE(!state.invalid_cleanup);
}

TEST_CASE("failed TX callback setup can be retried without leftover registration")
{
    reset(Failure::logging);
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.initializeTxCallbacks(), std::runtime_error);
    requireReleased("MCRSLsrcm");
    state.events.clear();
    state.fail = Failure::none;
    device.initializeTxCallbacks();
    REQUIRE_EQ(state.events, std::string("MCRSL"));
    REQUIRE(state.mutex && state.condition && state.registry && state.callback);
    REQUIRE(!state.invalid_cleanup);
}
