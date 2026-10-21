/**
 * @file SoapySidekiq.hpp
 * @brief Declares the SoapySDR device implementation for Epiq Sidekiq radios.
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sidekiq_api.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.h>
#include <SoapySidekiq/DeviceOptions.hpp>
#include <SoapySidekiq/TxBlockPool.hpp>
#include <SoapySidekiq/TxCompletionState.hpp>

// The owners are destroyed out of line; users of this declaration do not need
// the RX worker, queue, or sample-conversion definitions.
namespace soapy_sidekiq
{
class RxStreamBackend;
class RxStreamSession;
} // namespace soapy_sidekiq

/**
 * @brief Owns a Sidekiq card's streams and callback state.
 *
 * Instances remain at a stable address while worker threads refer to them.
 * Accepted TX callbacks retain their own completion state and blocks without
 * referring to the device. Copying and moving are disabled so worker references
 * and device resource ownership cannot be transferred.
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

    /** Configure one CS16/CF32 channel per direction; reject nonempty per-stream arguments. */
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

  private:
    /** Device-owned token; callers receive only an opaque SoapySDR::Stream pointer. */
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
        /** SDK gain index corresponding to zero dB. */
        double rx_offset;
        /** SDK index increments per dB of RX gain. */
        double rx_steps_per_db;
        /** Highest aggregate RX gain in dB for this legacy profile. */
        double rx_maximum;
    };
    /** Select the legacy card-wide RX index conversion; reject unknown models. */
    GainProfile gainProfile() const;
    /** Validate channel attenuation bounds and return the TX gain span in dB. */
    double txGainMaximum(size_t channel) const;
    /** Validated fresh SDK rate/bandwidth readback for one mapped handle. */
    struct RateAndBandwidth
    {
        /** SDK configured samples/s, reused when changing only bandwidth. */
        uint32_t configured_rate;
        /** Validated hardware samples/s, retaining its fractional part. */
        double actual_rate;
        /** Validated nonzero hardware filter bandwidth in Hz. */
        uint32_t actual_bandwidth;
    };
    /** Validate constructor rate/bandwidth readback for every mapped RX/TX handle. */
    void validateInitialRateConfiguration();
    /** Read and validate fresh SDK values for the logical channel's mapped handle. */
    RateAndBandwidth readRateAndBandwidth(int direction, size_t channel) const;
    /** Write hardware and validate fresh readback; no hardware rollback guarantee. */
    void applyRateAndBandwidth(int direction, size_t channel, uint32_t rate, uint32_t bandwidth);
    /** Validate a logical RX channel and return its SDK parameter-table handle. */
    skiq_rx_hdl_t getRxHandle(const size_t channel) const;
    /** Validate a logical TX channel and return its SDK parameter-table handle. */
    skiq_tx_hdl_t getTxHandle(const size_t channel) const;
    /** Map logical RX channel zero; reject a card with no RX channels. */
    skiq_rx_hdl_t getFirstRxHandle(void) const;
    /** Map logical TX channel zero; reject a card with no TX channels. */
    skiq_tx_hdl_t getFirstTxHandle(void) const;

    /** Copied SDK product name used for hardware identification. */
    std::string part_str;
    /** SDK product discriminator used for card-specific API policy. */
    skiq_part_t part;
    /** Copied SDK parameter table; logical channels index its mapped handles. */
    skiq_param_t param;
    /** SDK card identifier selected during construction. */
    uint8_t card{};
    /** RX ADC full scale used for normalized CF32 conversion. */
    double maxValue{};
    /** TX host full scale, accounting for the card's DAC bit alignment. */
    double txMaxValue{};

    /** RX stream uses CS16 directly rather than converting to CF32. */
    bool rxUseShort{};
    /** TX stream supplies CS16 directly rather than normalized CF32. */
    bool txUseShort{};

    /** Timestamp-source setting exposed through the settings API. */
    std::string timetype{};
    /** Owned adapter; declared before the session so it outlives that borrower. */
    std::unique_ptr<soapy_sidekiq::RxStreamBackend> rx_backend;
    /** Own the RX worker/queue; stop it before releasing the adapter or SDK. */
    std::unique_ptr<soapy_sidekiq::RxStreamSession> rx_session;
    /** Own the RX token; the opaque Soapy handle borrows its stable address. */
    std::unique_ptr<StreamHandle> active_rx_stream;

    /** Validated RX parameter-table count. */
    uint8_t num_rx_channels{};
    /** RX payload size in 32-bit words, also the CS16 complex-sample MTU. */
    uint32_t rx_payload_size_in_words{};

    /** Protect PPS result publication/waits and coordinate TX admission cancellation. */
    std::mutex tx_mutex;
    /** Wake writers after PPS startup publication or admission cancellation. */
    std::condition_variable tx_cv;
    /** Validated constructor limit for TX blocks and reservation slots. */
    std::uint32_t tx_buffer_count{soapy_sidekiq::defaultTxBufferCount};
    /** Reservation/notification storage retained independently by accepted contexts. */
    using TxCompletion = soapy_sidekiq::TxCompletionState<>;
    /** Current stream bookkeeping; accepted contexts can outlive this reference. */
    std::shared_ptr<TxCompletion> tx_completion;
    /** Admission gate; write with both TX/completion mutexes, read with either held. */
    bool tx_writes_stopping{true};
    /** Admitted writes to drain before stop; guarded by tx_completion->mutex. */
    std::size_t active_tx_writers{};
    /** Admitted status calls to drain before stop; guarded by tx_completion->mutex. */
    std::size_t active_tx_status_readers{};
    /** Close write/status admission and drain admitted calls before stopping hardware. */
    void stopTxWrites();
    /** Drain API calls, join PPS startup, and stop hardware; failed stops remain retryable. */
    int stopTxStream(bool on_pps);
    /** Best-effort destruction cleanup; accepted callbacks retain their own storage. */
    void shutdownTx() noexcept;
    /** PPS worker published a result; guarded by tx_mutex. */
    bool tx_start_finished{};
    /** Published PPS SDK status; guarded by tx_mutex. */
    int tx_start_status{};
    /** Retain stop responsibility after activation, including failed-stop retries. */
    bool tx_stream_active{};
    /**
     * PPS first-write gate: activation initializes it before opening admission.
     * writeStream checks it before locking tx_mutex, then clears it under that lock.
     * Callers serialize writes and activation; status polling reads it under tx_mutex.
     */
    bool first_transmit{};
    /** Own the TX token until close or device shutdown. */
    std::unique_ptr<StreamHandle> active_tx_stream;

    /** Validated TX parameter-table count. */
    uint8_t num_tx_channels{};
    /** Serialize SDK underrun polling and startup counter reset. */
    std::timed_mutex tx_status_mutex;
    /** Last underrun count; guarded by tx_status_mutex; reset after a successful start. */
    uint32_t tx_underruns{};
    /** Configured TX complex samples per block; positive before writes are admitted. */
    uint32_t current_tx_block_size{};

    /** Last successful I/Q-order setting. */
    bool iq_swap{};
    /** Last successful all-handle RX data-source selection: counter rather than IQ. */
    bool counter{};
    /** Select RF sample timestamps; false selects the system counter. */
    bool rfTimeSource{};
    /** Last SDK system-counter frequency readback in ticks/s. */
    uint64_t sys_freq{};

    /** SDK-allocated blocks shared individually with accepted contexts. */
    using TxBlocks = soapy_sidekiq::TxBlockPool<skiq_tx_block_t>;
    /** Own the pool; its destruction leaves accepted blocks retained by callbacks. */
    std::unique_ptr<TxBlocks> tx_blocks;
    /** Prepare all block owners and bookkeeping before publishing a TX stream. */
    void initializeTxBuffers();
    /**
     * Next circular pool slot; initialized before admission and advanced by writeStream.
     * Callers serialize writes; tx_mutex does not protect this index or payload access.
     * Individual slot reservations are protected by tx_completion->mutex.
     */
    uint32_t tx_block_index{};

    /** Consume an accepted context without device access or unwinding through the SDK. */
    static void tx_complete_callback(int32_t status, skiq_tx_block_t *p_data,
                                     void *p_user) noexcept;

    /** Per-send context: owned by writeStream until accepted, then by completion. */
    struct TxSendContext
    {
        /** Retain this stream's reservations and notification state through late completion. */
        std::shared_ptr<TxCompletion> completion;
        /** Retain the accepted SDK payload until completion releases the context. */
        std::shared_ptr<skiq_tx_block_t> block;
        /** Reserved slot in completion; never an index into a newer stream's pool. */
        uint32_t txIndex;
    };

    /** Owned PPS-start worker; it must finish before device state is released. */
    std::thread _tx_streaming_thread;
    /** Publish asynchronous startup completion before exception-contained diagnostics. */
    void tx_streaming_start(skiq_tx_hdl_t tx_handle) noexcept;

    /** Publish completion using only the submitting stream's retained state. */
    static void completeTx(const std::shared_ptr<TxCompletion> &completion, uint32_t txIndex);
};
