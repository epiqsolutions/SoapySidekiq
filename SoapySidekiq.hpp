/**
 * @file SoapySidekiq.hpp
 * @brief Declares the SoapySDR device implementation for Epiq Sidekiq radios.
 */

#pragma once

#include <functional>
#include <sidekiq_api.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.h>
#include <SoapySidekiq/RxStreamSession.hpp>

/** Legacy number of preallocated host TX blocks per configured stream. */
inline constexpr int DEFAULT_NUM_BUFFERS = 30000;

/**
 * @brief Owns a Sidekiq card's streams and callback state.
 *
 * Instances remain at a stable address while SDK callbacks and worker threads
 * refer to them. Copying and moving are explicitly disabled so these references
 * and the ownership of device resources cannot be transferred.
 */
class SoapySidekiq : public SoapySDR::Device
{
  public:
    /** Initialize the selected Sidekiq card from validated constructor keywords. */
    SoapySidekiq(const SoapySDR::Kwargs &args);

    /** Release device resources; callers must serialize destruction with API calls. */
    ~SoapySidekiq(void) override;

    SoapySidekiq(const SoapySidekiq &) = delete;
    SoapySidekiq &operator=(const SoapySidekiq &) = delete;
    SoapySidekiq(SoapySidekiq &&) = delete;
    SoapySidekiq &operator=(SoapySidekiq &&) = delete;

    /*******************************************************************
     * Identification API
     ******************************************************************/

    /** Return the SoapySDR driver key used for Sidekiq discovery. */
    std::string getDriverKey(void) const override;

    /** Return the SDK-reported Sidekiq product name. */
    std::string getHardwareKey(void) const override;

    /** Return copied serial, product, and channel metadata; SDK owns its serial buffer. */
    SoapySDR::Kwargs getHardwareInfo(void) const override;

    /*******************************************************************
     * Channels API
     ******************************************************************/

    /** Return the SDK channel count for RX or TX; reject unknown directions. */
    size_t getNumChannels(const int) const override;

    /*******************************************************************
     * Stream API
     ******************************************************************/

    /** List CS16 and CF32 for a validated direction and logical channel. */
    std::vector<std::string> getStreamFormats(const int direction,
                                              const size_t channel) const override;

    /** Return CS16 and the card-specific host full scale for the selected direction. */
    std::string getNativeStreamFormat(const int direction, const size_t channel,
                                      double &fullScale) const override;

    /** Describe implemented per-stream options; constructor arguments are separate. */
    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction,
                                            const size_t channel) const override;

    /** Configure one logical channel in CS16 or CF32; at most one stream per direction. */
    SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                  const std::vector<size_t> &channels = std::vector<size_t>(),
                                  const SoapySDR::Kwargs &args = SoapySDR::Kwargs()) override;

    /** Release a deactivated stream token; failed RX stops retain ownership for retry. */
    void closeStream(SoapySDR::Stream *stream) override;

    /** Return complex samples per hardware block for a currently owned stream token. */
    size_t getStreamMTU(SoapySDR::Stream *stream) const override;

    /** Start continuous streaming immediately or at next PPS; return a stream status. */
    int activateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0,
                       const size_t numElems = 0) override;

    /** Drain admitted TX calls and stop immediately or at PPS; retain failed-stop ownership. */
    int deactivateStream(SoapySDR::Stream *stream, const int flags = 0,
                         const long long timeNs = 0) override;

    /** Read CS16 or converted CF32 with first-sample timestamps and partial-block offsets. */
    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags,
                   long long &timeNs, const long timeoutUs = 100000) override;

    /**
     * Submit CS16 or normalized CF32 samples in whole TX MTUs, without staging.
     * numElems must fit int; invalid counts/buffers return SOAPY_SDR_STREAM_ERROR.
     * An admitted active-stream zero-length write returns zero without accessing buffs.
     * Back-pressure returns an accepted sample prefix or SOAPY_SDR_TIMEOUT; callers retry only the
     * unaccepted suffix, advancing by two format components per sample.
     */
    int writeStream(SoapySDR::Stream *stream, const void *const *buffs, const size_t numElems,
                    int &flags, const long long timeNs = 0, const long timeoutUs = 100000) override;

    /** Poll mapped TX underrun progress with one timeout; RX status is unsupported. */
    int readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs,
                         const long timeoutUs = 100000) override;

    /*******************************************************************
     * Antenna API
     ******************************************************************/

    /** Advertise only the currently configured SDK RF port, when reporting is supported. */
    std::vector<std::string> listAntennas(const int direction, const size_t channel) const override;
    /** Read the RF port of the selected mapped handle; empty means no reported route. */
    std::string getAntenna(const int direction, const size_t channel) const override;
    /** Accept only the currently configured port; other route changes are unsupported. */
    void setAntenna(const int direction, const size_t channel, const std::string &name) override;

    /*******************************************************************
     * Frontend corrections API
     ******************************************************************/

    /** Report RX automatic DC-calibration support for a validated channel. */
    bool hasDCOffsetMode(const int direction, const size_t channel) const override;

    /** Select or clear the RX DC-calibration bit while preserving unrelated calibration. */
    void setDCOffsetMode(const int direction, const size_t channel, const bool automatic) override;

    /** Read whether RX DC correction is selected in automatic calibration mode. */
    bool getDCOffsetMode(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Gain API
     ******************************************************************/

    /** List the gain elements accepted by the selected direction and channel. */
    std::vector<std::string> listGains(const int direction, const size_t channel) const override;

    /** Report RX AGC support; TX supports manual gain only. */
    bool hasGainMode(const int direction, const size_t channel) const override;

    /** Select RX manual/automatic gain; reject TX automatic-gain requests. */
    void setGainMode(const int direction, const size_t channel, const bool automatic) override;

    /** Read RX AGC mode; TX always reports manual gain. */
    bool getGainMode(const int direction, const size_t channel) const override;

    /** Set aggregate gain in dB, or validate a named element before forwarding to it. */
    void setGain(const int direction, const size_t channel, const std::string &name,
                 const double value) override;

    /** Set aggregate gain in dB, or validate a named element before forwarding to it. */
    void setGain(const int direction, const size_t channel, const double value) override;

    /** Read aggregate gain in dB, optionally validating the requested gain element. */
    double getGain(const int direction, const size_t channel,
                   const std::string &name) const override;

    /** Read aggregate gain in dB, optionally validating the requested gain element. */
    double getGain(const int direction, const size_t channel) const override;

    /** Return gain bounds in dB for the aggregate control or a validated named element. */
    SoapySDR::Range getGainRange(const int direction, const size_t channel,
                                 const std::string &name) const override;

    /** Return gain bounds in dB for the aggregate control or a validated named element. */
    SoapySDR::Range getGainRange(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Frequency API
     ******************************************************************/
    /** Tune the selected mapped LO to a finite, representable frequency in Hz. */
    void setFrequency(const int direction, const size_t channel, const double frequency,
                      const SoapySDR::Kwargs &args = SoapySDR::Kwargs()) override;

    /** Read the selected mapped LO frequency in Hz. */
    double getFrequency(const int direction, const size_t channel) const override;

    /** Return SDK frequency bounds for the selected logical channel. */
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Sample Rate API
     ******************************************************************/

    /** Write rate/bandwidth together and commit cached values only after checked readback. */
    void setSampleRate(const int direction, const size_t channel, const double rate) override;

    /** Read the current actual SDK sample rate, including fractional Hz. */
    double getSampleRate(const int direction, const size_t channel) const override;

    // Older supported SoapySDR APIs do not declare this virtual method.
#ifdef SOAPY_SDR_API_HAS_GET_SAMPLE_RATE_RANGE
    /** Return validated SDK sample-rate bounds for the selected logical channel. */
    SoapySDR::RangeList getSampleRateRange(const int direction,
                                           const size_t channel) const override;
#else
    /** Return validated SDK sample-rate bounds for the selected logical channel. */
    SoapySDR::RangeList getSampleRateRange(const int direction, const size_t channel) const;
#endif

    /** Return the compatibility rate suggestions within SDK bounds, not exact card profiles. */
    std::vector<double> listSampleRates(const int direction, const size_t channel) const override;

    /** Apply bandwidth using a fresh SDK rate readback to account for coupled clocks. */
    void setBandwidth(const int direction, const size_t channel, const double bw) override;

    /** Read the current actual SDK bandwidth in Hz. */
    double getBandwidth(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Sensor API
     ******************************************************************/
    /** List card temperature and accelerometer diagnostics; support is checked on read. */
    std::vector<std::string> listSensors(void) const override;
    /** Describe sensor units, value encoding, and legacy backend-error fallback values. */
    SoapySDR::ArgInfo getSensorInfo(const std::string &key) const override;
    /** Read temperature or accelerometer JSON; preserve documented legacy error fallbacks. */
    std::string readSensor(const std::string &key) const override;

    /*******************************************************************
     * Settings API
     ******************************************************************/

    /** Describe mutable settings and read-only device scalars with current cached values. */
    SoapySDR::ArgInfoList getSettingInfo(void) const override;

    /** Validate and apply IQ, counter, or timestamp settings; reject unknown/read-only keys. */
    void writeSetting(const std::string &key, const std::string &value) override;

    /** Read cached settings, device scalars, or legacy RX diagnostic aliases. */
    std::string readSetting(const std::string &key) const override;

    /*******************************************************************
     * Time API
     ******************************************************************/

    /** List the supported PPS source names, including the unavailable source. */
    std::vector<std::string> listTimeSources(void) const override;

    /** Select an SDK/kernel PPS source and verify an edge when PPS is enabled. */
    void setTimeSource(const std::string &source) override;

    /** Read the selected PPS source, using the kernel fallback when required. */
    std::string getTimeSource(void) const override;

    /** Report system-time or first-mapped-handle RF-time selector availability. */
    bool hasHardwareTime(const std::string &) const override;

    /** Read signed nanoseconds; the empty selector uses the system timestamp clock. */
    long long getHardwareTime(const std::string &) const override;

    /** Set timestamp counters immediately or schedule RF-zero/system-time update at PPS. */
    void setHardwareTime(const long long timeNs, const std::string &what) override;

    /*******************************************************************
     * Clocking API
     ******************************************************************/

    /** List the internal and external reference-clock selection names. */
    std::vector<std::string> listClockSources(void) const override;

    /** Select an internal or external reference clock after validating the name. */
    void setClockSource(const std::string &source) override;

    /** Read reference-clock selection; the SDK host reference retains the internal alias. */
    std::string getClockSource(void) const override;

    // Reference clock access was introduced in the SoapySDR 0.8 API.
#if defined(SOAPY_SDR_API_VERSION) && SOAPY_SDR_API_VERSION >= 0x00080000
    /** Return configured reference frequency in Hz, not measured lock/presence. */
    double getReferenceClockRate(void) const override;
#else
    /** Return configured reference frequency in Hz, not measured lock/presence. */
    double getReferenceClockRate(void) const;
#endif

    /** Device-owned token linking a logical stream channel to its SDK handle. */
    struct StreamHandle
    {
        /** Soapy RX/TX direction used to select stream lifecycle and I/O. */
        int direction;
        /** Logical channel selected when the stream was configured. */
        size_t channel;
        /** Mapped RX handle, or the SDK end sentinel for a TX stream. */
        skiq_rx_hdl_t rx_handle;
        /** Mapped TX handle, or the SDK end sentinel for an RX stream. */
        skiq_tx_hdl_t tx_handle;
    };

  private:
    /** Serialize coupled RX/TX rate changes with the RX session's hardware ownership. */
    void configureWhileRxInactive(const std::function<void()> &configure);

    /** Throwing activation implementation behind the public stream-status boundary. */
    int activateStreamImpl(SoapySDR::Stream *stream, int flags, long long timeNs, size_t numElems);

    /** RX read body; the public boundary translates allocation/timing exceptions to status. */
    int readStreamImpl(SoapySDR::Stream *stream, void *const *buffs, size_t numElems, int &flags,
                       long long &timeNs, long timeoutUs);

    /** Resolve only this device's currently owned opaque stream tokens. */
    StreamHandle *findStreamHandle(SoapySDR::Stream *stream) const noexcept;

    /** Acquire TX callback resources; unwind acquired state on any failure. */
    void initializeTxCallbacks();

    /** Reject invalid directions/channels before metadata or SDK operations. */
    void validateChannel(int direction, size_t channel) const;
    /** Require a named gain element advertised for the selected channel. */
    void validateGainName(int direction, size_t channel, const std::string &name) const;
    /** Read the configured RF port; empty when reporting is unavailable. */
    std::string currentAntenna(int direction, size_t channel) const;
    /** Existing card-specific RX index conversion. */
    struct GainProfile
    {
        double rx_offset;
        double rx_steps_per_db;
        double rx_maximum;
    };
    GainProfile gainProfile() const;
    double txGainMaximum(size_t channel) const;
    /** Validated SDK configuration; caches change only after complete readback. */
    struct RateAndBandwidth
    {
        uint32_t configured_rate;
        double actual_rate;
        uint32_t actual_bandwidth;
    };
    /** Populate all initial RF caches only after every SDK readback is validated. */
    void initializeRateCaches();
    RateAndBandwidth readRateAndBandwidth(int direction, size_t channel) const;
    void applyRateAndBandwidth(int direction, size_t channel, uint32_t rate, uint32_t bandwidth);
    skiq_rx_hdl_t getRxHandle(const size_t channel) const;
    skiq_tx_hdl_t getTxHandle(const size_t channel) const;
    skiq_rx_hdl_t getFirstRxHandle(void) const;
    skiq_tx_hdl_t getFirstTxHandle(void) const;

    //  sidekiq card
    std::string part_str;
    skiq_part_t part;
    skiq_param_t param;
    uint8_t card{};
    uint32_t resolution{};
    double maxValue{};   // RX ADC full scale.
    double txMaxValue{}; // TX host full scale accounts for DAC bit alignment.

    bool rxUseShort{};
    bool txUseShort{};

    // RX hardware adapter must outlive the session that references it.
    std::string timetype{};
    std::unique_ptr<soapy_sidekiq::RxStreamBackend> rx_backend;
    std::unique_ptr<soapy_sidekiq::RxStreamSession> rx_session;
    StreamHandle *active_rx_stream{};

    uint8_t num_rx_channels{};
    std::vector<uint32_t> rx_sample_rates;
    std::vector<uint32_t> rx_bandwidths;
    uint32_t rx_block_size_in_words{};
    uint32_t rx_block_size_in_bytes{};
    uint32_t rx_payload_size_in_bytes{};
    uint32_t rx_payload_size_in_words{};

    //  tx
    std::mutex tx_mutex;
    std::condition_variable tx_cv;
    std::mutex tx_buf_mutex;
    std::condition_variable tx_buf_cv;
    // Written with tx_mutex and tx_buf_mutex held; read with either held.
    bool tx_writes_stopping{true};
    std::size_t active_tx_writers{}; // Guarded by tx_buf_mutex.
    std::size_t active_tx_status_readers{}; // Guarded by tx_buf_mutex.
    /** Close write/status admission and drain admitted calls before stopping hardware. */
    void stopTxWrites();
    bool tx_start_finished{};
    int tx_start_status{};
    bool tx_stream_active{};
    // Fixed-size status storage is allocated before callbacks are registered.
    std::unique_ptr<int32_t[]> p_tx_status;
    bool first_transmit{};
    StreamHandle *active_tx_stream{};

    uint8_t num_tx_channels{};
    std::vector<uint32_t> tx_sample_rates;
    std::vector<uint32_t> tx_bandwidths;
    std::timed_mutex tx_status_mutex;
    uint32_t tx_underruns{};   // Guarded by tx_status_mutex; reset after successful SDK start.
    uint64_t complete_count{}; // Guarded by tx_buf_mutex.
    uint32_t current_tx_block_size{};

    //  setting
    bool iq_swap{};
    bool counter{};
    bool rfTimeSource{};
    uint64_t sys_freq{};

    // TX buffer
    skiq_tx_block_t *p_tx_block[DEFAULT_NUM_BUFFERS]{};
    uint32_t currTXBuffIndex{};

    // TX callback static function
    // The registration requires a static function instead of a method so
    // this must be created to be able to register it.
    // This function calls the tx_complete method.
    static void tx_complete_callback(int32_t status, skiq_tx_block_t *p_data,
                                     void *p_user) noexcept;

  public:
    /** Per-send context: owned by writeStream until accepted, then by completion. */
    struct passedStruct
    {
        /** Completion routing target for this accepted submission. */
        SoapySidekiq *classAddr;
        /** Reserved pool slot released by completion publication. */
        uint32_t txIndex;
    };

    /** Owned PPS-start worker; it must finish before device state is released. */
    std::thread _tx_streaming_thread;
    /** Publish asynchronous startup completion before exception-contained diagnostics. */
    void tx_streaming_start(skiq_tx_hdl_t tx_handle) noexcept;

    // tx callback method
    void tx_complete(int32_t status, skiq_tx_block_t *p_data, uint32_t txIndex);
};
