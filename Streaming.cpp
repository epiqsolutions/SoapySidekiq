#include <cstring>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>

#include "SoapySidekiq.hpp"
#include <SoapySDR/Formats.hpp>
#include <sidekiq_types.h>

// Attempted to put this in SoapySidekiq.hpp and it would not link
// could not understand why
bool   start_signal = false;

long long SoapySidekiq::convert_timestamp_to_nanos(
        const uint64_t timestamp, const uint64_t timestamp_freq) const
{
    const double nanos_per_tic = 1.0/timestamp_freq*1e9;
    const uint64_t whole_nanos_per_tic = static_cast<uint64_t>(nanos_per_tic);
    const double frac_nanos_per_tic = nanos_per_tic - whole_nanos_per_tic;
    const long long nanos =
        static_cast<long long>(timestamp * whole_nanos_per_tic) +
        static_cast<long long>(timestamp * frac_nanos_per_tic);
    return nanos;
}


std::vector<std::string> SoapySidekiq::getStreamFormats(
    const int direction, const size_t channel) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamFormats");

    std::vector<std::string> formats;

    formats.push_back(SOAPY_SDR_CS16);

    if (direction == SOAPY_SDR_RX)
    {
        formats.push_back(SOAPY_SDR_CF32);
    }

    return formats;
}

std::string SoapySidekiq::getNativeStreamFormat(const int    direction,
                                                const size_t channel,
                                                double &     fullScale) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNativeStreamFormat");

    fullScale = this->maxValue;

    return "CS16";
}

SoapySDR::ArgInfoList SoapySidekiq::getStreamArgsInfo(
            const int direction, const size_t channel) const
{
    SoapySDR::ArgInfoList streamArgs;

    SoapySDR::ArgInfo bufflenArg;
    bufflenArg.key = "bufflen";

    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamArgsInfo");

    if (direction == SOAPY_SDR_RX)
    {
        bufflenArg.name        = "Buffer Sample Count";
        bufflenArg.description = "Number of IQ samples per buffer.";
        bufflenArg.units       = "(int16_t * 2) samples";
        bufflenArg.type        = SoapySDR::ArgInfo::INT;
        bufflenArg.value       = std::to_string(rx_payload_size_in_words);

        streamArgs.push_back(bufflenArg);
    }
    else
    {
        bufflenArg.name        = "Buffer Sample Count";
        bufflenArg.description = "Number of IQ samples per buffer.";
        bufflenArg.units       = "(int16_t * 2) samples";
        bufflenArg.value       = std::to_string(current_tx_block_size);
        bufflenArg.type        = SoapySDR::ArgInfo::INT;

        streamArgs.push_back(bufflenArg);
    }

    return streamArgs;
}

/*******************************************************************
 * Sidekiq receive thread
 ******************************************************************/

void SoapySidekiq::rx_receive_operation(void)
{
    int status = 0;


    //  skiq receive params
    skiq_rx_block_t *tmp_p_rx_block;
    uint32_t         len;
    bool             first = true;
    uint64_t         expected_timestamp = 0;


    // metadata
    uint64_t overload = 0;
    skiq_rx_hdl_t rcvd_hdl;

    std::unique_lock<std::mutex> lock(rx_mutex);

    SoapySDR_log(SOAPY_SDR_TRACE, "entering rx_receive_thread");

    // wait till called to start running
    _cv.wait(lock, [this] { return start_signal; });

    SoapySDR_log(SOAPY_SDR_INFO, "Starting RX Sidekiq Thread loop");

    //  loop until stream is deactivated
    while (rx_running)
    {
        //  check for overflow
        if (rxReadIndex == ((rxWriteIndex + 1) % DEFAULT_NUM_BUFFERS))
        {
            SoapySDR_log(SOAPY_SDR_ERROR,
                         "Detected overflow event in RX worker thread");

            SoapySDR_logf(SOAPY_SDR_DEBUG, "rxReadIndex %d, rxWriteIndex %d",
                          rxReadIndex, rxWriteIndex, overload);

            throw std::runtime_error("");
        }
        else
        {
            /*
             * put the block into a ring buffer so readStream can read it out
             * at a different pace 
             * skiq_receive is in blocking mode, so it will allow the other 
             * threads to run */
            status = skiq_receive(card, &rcvd_hdl, &tmp_p_rx_block, &len);
            if (status == skiq_rx_status_success)
            {
                if (rcvd_hdl == rx_hdl)
                {
                    if (len != rx_block_size_in_bytes)
                    {
                        SoapySDR_logf(SOAPY_SDR_ERROR, "received length %d is not the correct "
                                      "block size %d\n",
                                      len, rx_block_size_in_bytes);
                        throw std::runtime_error("");
                    }

                    // get overload out of metadata
                    overload = tmp_p_rx_block->overload;

                    int num_words_read = (len / 4);

                    // check for timestamp error
                    if (first == false)
                    {
                        if (expected_timestamp != tmp_p_rx_block->rf_timestamp)
                        {
                            SoapySDR_log(SOAPY_SDR_WARNING,
                                         "Detected timestamp overflow in RX Sidekiq Thread");
                            SoapySDR_logf(SOAPY_SDR_DEBUG, "expected timestamp %lu, "
                                          "actual %lu",
                                          expected_timestamp, tmp_p_rx_block->rf_timestamp);

                            // restart the timestamp checking
                            first = true;
                        }
                        else
                        {
                            expected_timestamp += rx_payload_size_in_words;
                        }
                    }
                    else
                    {
                        first = false;
                        expected_timestamp = tmp_p_rx_block->rf_timestamp +
                                             rx_payload_size_in_words;
                    }

                    // copy the data out of the rx_buffer and into the RAM
                    // buffers copy the header in also
                    memcpy(p_rx_block[rxWriteIndex], (void *)tmp_p_rx_block,
                           (num_words_read * sizeof(uint32_t)));

                    rxWriteIndex = (rxWriteIndex + 1) % DEFAULT_NUM_BUFFERS;
                }
            }
            else
            {
                if (status != skiq_rx_status_no_data)
                {
                    SoapySDR_logf(SOAPY_SDR_FATAL,
                                  "skiq_receive failed, (card %u) status %d",
                                  card, status);
                    throw std::runtime_error("");
                }
            }
        }
    }

    SoapySDR_log(SOAPY_SDR_INFO, "Exiting RX Sidekiq Thread");
}

/*******************************************************************
 * Stream API
 ******************************************************************/

SoapySDR::Stream *SoapySidekiq::setupStream(const int direction,
                                            const std::string &format,
                                            const std::vector<size_t> &channels,
                                            const SoapySDR::Kwargs &args)
{
    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setupStream");

    if (direction == SOAPY_SDR_RX)
    {
        //  check the channel configuration
        if (channels.size() > 1)
        {
            throw std::runtime_error("only one RX channel is supported simultaneously");
        }

        rx_hdl = (skiq_rx_hdl_t)channels.at(0);

        SoapySDR_logf(SOAPY_SDR_INFO, "RX handle: %u", rx_hdl);

        status = skiq_read_rx_block_size(card, skiq_rx_stream_mode_balanced);
        if (status < 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_block_size failed: "
                                           "card: %u status: %d\n",
                                           card, status);
            throw std::runtime_error("");
        }
        rx_block_size_in_bytes = status;
        rx_block_size_in_words = status / 4;

        rx_payload_size_in_bytes = status - SKIQ_RX_HEADER_SIZE_IN_BYTES;
        rx_payload_size_in_words = rx_payload_size_in_bytes / 4;

        SoapySDR_logf(SOAPY_SDR_INFO, "RX payload size in words: %u",
                      rx_payload_size_in_words);

        // allocate the RAM buffers
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            p_rx_block[i] = (skiq_rx_block_t *)malloc(rx_block_size_in_bytes);
            if (p_rx_block[i] == NULL)
            {
                SoapySDR_log(SOAPY_SDR_ERROR, "malloc failed to allocate memory ");
                throw std::runtime_error("");
            }

            memset(p_rx_block[i], 0, rx_block_size_in_bytes);
        }
        rxWriteIndex = 0;
        rxReadIndex  = 0;

        if (format == "CS16")
        {
            rxUseShort = true;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CS16");
        }
        else if (format == "CF32")
        {
            rxUseShort = false;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32");
        }
        else
        {
            throw std::runtime_error("setupStream invalid format '" + format +
                "' -- Only CS16 or CF32 is supported by SoapySidekiq module.");
        }

        // We cannot assume the caller will set all default parameters
        // So set them here first
        if (rx_sample_rate == 0)
        {
            setSampleRate(SOAPY_SDR_RX, rx_hdl, DEFAULT_SAMPLE_RATE);
        }

        if (rx_bandwidth == 0)
        {
            setBandwidth(SOAPY_SDR_RX, rx_hdl, DEFAULT_BANDWIDTH);
        }

        if (rx_center_frequency == 0)
        {
            setFrequency(SOAPY_SDR_RX, rx_hdl, DEFAULT_FREQUENCY);
        }

        // this has to be called after setting sample rate
        int status = 0;
        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d",
                           card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "System Timestamp Freq: %llu", this->sys_freq);

        return RX_STREAM;
    }
    else if (direction == SOAPY_SDR_TX)
    {
        //  check the channel configuration
        if (channels.size() > 1)
        {
            throw std::runtime_error("only one TX channel is supported simultaneously");
        }

        tx_hdl = (skiq_tx_hdl_t)channels.at(0);

        SoapySDR_logf(SOAPY_SDR_INFO, "The TX handle is: %u", tx_hdl);

        if (format == "CS16")
        {
            txUseShort = true;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CS16");
        }
        else if (format == "CF32")
        {
            txUseShort = false;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32");
        }
        else
        {
            throw std::runtime_error(
                "setupStream invalid format '" + format +
                "' -- Only CS16 is supported by SoapySidekiq TX module.");
        }

        // Allocate buffers
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            p_tx_block[i] = skiq_tx_block_allocate(current_tx_block_size);
        }
        currTXBuffIndex = 0;

        tx_hdl = (skiq_tx_hdl_t)channels.at(0);

        // We cannot assume the caller will set all default parameters
        // So set them here first
        if (tx_sample_rate == 0)
        {
            setSampleRate(SOAPY_SDR_TX, rx_hdl, DEFAULT_SAMPLE_RATE);
        }

        if (tx_bandwidth == 0)
        {
            setBandwidth(SOAPY_SDR_TX, rx_hdl, DEFAULT_BANDWIDTH);
        }

        if (tx_center_frequency == 0)
        {
            setFrequency(SOAPY_SDR_TX, rx_hdl, DEFAULT_FREQUENCY);
        }

        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d",
                           card, status);
            throw std::runtime_error("");
        }
        
        return TX_STREAM;
    }
    else
    {
        throw std::runtime_error("invalid direction");
    }
}

void SoapySidekiq::closeStream(SoapySDR::Stream *stream)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "closeStream");

    if (stream == RX_STREAM)
    {
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            free(p_rx_block[i]);
        }
    }
    else if (stream == TX_STREAM)
    {
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            skiq_tx_block_free(p_tx_block[i]);
        }
    }
}

size_t SoapySidekiq::getStreamMTU(SoapySDR::Stream *stream) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStremMTU");

    if (stream == RX_STREAM)
    {
        return (rx_payload_size_in_words);
    }
    else if (stream == TX_STREAM)
    {
        return current_tx_block_size;
    }
    else
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    return 0;
}

int SoapySidekiq::activateStream(SoapySDR::Stream *stream,
                                 const int flags,
                                 const long long timeNs,
                                 const size_t numElems)
{
    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "activateStream");

    if (stream == RX_STREAM)
    {
        /* set rx source as iq data */
        if (iq_swap == true)
        {
            status = skiq_write_iq_order_mode(card, skiq_iq_order_iq);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                             "skiq_write_rx_data_src failed (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "RX is set to I then Q order");
        }

        /* set a modest rx timeout to make skiq_receive blocking*/
        status = skiq_set_rx_transfer_timeout(card, 100000);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_set_rx_transfer_timeout failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }

        //  start the receive thread
        if (!_rx_receive_thread.joinable())
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "Start RX thread");

            rx_running = true;
            start_signal = false;

            _rx_receive_thread =
                std::thread(&SoapySidekiq::rx_receive_operation, this);
        }

        std::lock_guard<std::mutex> lock(rx_mutex);

        // Notify the thread to run
        start_signal = true;
        _cv.notify_one();  

        /* start rx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            status = skiq_start_rx_streaming_on_1pps(card, rx_hdl, 0);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_rx_streaming_on_1pps failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
        }
        else
        {
            status = skiq_start_rx_streaming(card, rx_hdl);
            if (status !=0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_rx_streaming failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
        }
    }
    else if (stream == TX_STREAM)
    {
        /* set as iq data */
        if (iq_swap == true)
        {
            status = skiq_write_iq_order_mode(card, skiq_iq_order_iq);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                             "skiq_write_rx_data_src failed (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "TX is set to I then Q order");
        }

        p_tx_block_index = 0;

        //  tx block size
        status =
            skiq_write_tx_block_size(card, tx_hdl, current_tx_block_size);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                         "skiq_write_tx_block_size failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "TX block size is: %u", current_tx_block_size);

        //  tx data flow mode
        status = skiq_write_tx_data_flow_mode(card, tx_hdl,
                                              skiq_tx_immediate_data_flow_mode);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "skiq_write_tx_data_flow_mode failed (card %u) status %d",
                card, status);
            throw std::runtime_error("");
        }

        // running in aync mode
        status = skiq_write_tx_transfer_mode(card, tx_hdl,
                                             skiq_tx_transfer_mode_async);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_tx_transfer_mode failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }

        // configure 4 threads to be safe, too many and it may consume resources
        status = skiq_write_num_tx_threads(card, 4);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_num_tx_threads failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }

        /* start tx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            status = skiq_start_tx_streaming_on_1pps(card, tx_hdl, 0);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_tx_streaming_on_1pps failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "TX start streaming on 1pps");
        }
        else
        {
            status = skiq_start_tx_streaming(card, tx_hdl);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_tx_streaming failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "TX start streaming");
        }
    }

    return 0;
}

int SoapySidekiq::deactivateStream(SoapySDR::Stream *stream, const int flags,
                                   const long long timeNs)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "deactivateStream");

    if (stream == RX_STREAM)
    {
        // stop receive thread
        rx_running = false;

        /* stop rx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            status = skiq_stop_rx_streaming_on_1pps(card, rx_hdl, 0);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_rx_streaming_on_1pps failed, (card %u) handle "
                        "%d, status %d",
                        card, rx_hdl, status);
                throw std::runtime_error("");
            }
        }
        else
        {
            status = skiq_stop_rx_streaming(card, rx_hdl);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_rx_streaming failed, (card %u) handle "
                        "%d, status %d",
                        card, rx_hdl, status);
                throw std::runtime_error("");
            }
        }

        /* wait till the rx thread is done */
        if (_rx_receive_thread.joinable())
        {
            _rx_receive_thread.join();
        }
    }
    else if (stream == TX_STREAM)
    {
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            /* stop tx streaming */
            status = skiq_stop_tx_streaming_on_1pps(card, tx_hdl, 0);
            if (status != 0)
            {
                SoapySDR_logf(
                        SOAPY_SDR_ERROR,
                        "skiq_stop_tx_streaming_on_1pps failed (card %u), status %d",
                        card, status);
                throw std::runtime_error("");
            }
        }
        else
        {
            /* stop tx streaming */
            status = skiq_stop_tx_streaming(card, tx_hdl);
            if (status != 0)
            {
                SoapySDR_logf(
                        SOAPY_SDR_ERROR,
                        "skiq_stop_tx_streaming failed (card %u), status %d",
                        card, status);
                throw std::runtime_error("");
            }
        }
    }

    return 0;
}

int SoapySidekiq::readStream(SoapySDR::Stream *stream, void *const *buffs,
                             const size_t numElems, int &flags,
                             long long &timeNs, const long timeoutUs)
{
    if (stream != RX_STREAM)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    if (numElems % rx_payload_size_in_words != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "numElems must be a multiple of the RX block size "
                     " numElems %d, block size %u",
                     numElems, rx_payload_size_in_words);
        throw std::runtime_error("");
        return 0;
    }

    long waitTime = timeoutUs;

    // if the user didn't give a waittime then wait a LONG time
    if (waitTime == 0)
    {
        waitTime = SLEEP_1SEC;
    }

    // see if we have any receive buffers to give
    while ((rxReadIndex == rxWriteIndex) && (waitTime > 0))
    {
        // wait
        usleep(DEFAULT_SLEEP_US);
        waitTime -= DEFAULT_SLEEP_US;
    }

    if (waitTime <= 0)
    {
        SoapySDR_log(SOAPY_SDR_DEBUG, "readStream timed out");
        return SOAPY_SDR_TIMEOUT;
    }

    char *buff_ptr = (char *)buffs[0];
    skiq_rx_block_t *block_ptr = p_rx_block[rxReadIndex];
    char *ringbuffer_ptr = (char *)((char *)block_ptr->data);

    // move to the next buffer in the ring
    rxReadIndex = (rxReadIndex + 1) % DEFAULT_NUM_BUFFERS;

    if (this->rfTimeSource == true)
    {
        timeNs = convert_timestamp_to_nanos(block_ptr->rf_timestamp, rx_sample_rate);
    }
    else
    {
        timeNs = convert_timestamp_to_nanos(block_ptr->sys_timestamp, sys_freq);
    }

    flags = SOAPY_SDR_HAS_TIME;

    uint32_t block_num = 0;
    uint32_t num_blocks = numElems / rx_payload_size_in_words;

    while (block_num < num_blocks)
    {
        // copy in the amount of data we have in the ring block
        if (rxUseShort == true)
        {
            // CS16
            memcpy(buff_ptr, ringbuffer_ptr, rx_payload_size_in_bytes);
        }
        else
        {
            // CF32 float
            float *  dbuff_ptr = (float *)buff_ptr;
            int16_t *source = (int16_t *)ringbuffer_ptr;
            int short_ctr = 0;
            uint32_t i;

            for (i = 0; i < rx_payload_size_in_words; i++)
            {
                *dbuff_ptr++ = (float)source[short_ctr + 1] / this->maxValue;
                *dbuff_ptr++ = (float)source[short_ctr] / this->maxValue;
                short_ctr += 2;
            }
        }

        // a block is done, so move counter.
        block_num++;

        // if we need more blocks get the next one
        if (block_num < num_blocks)
        {
            // get next block if available otherwise wait
            while ((rxReadIndex == rxWriteIndex) && (waitTime > 0))
            {
                // wait
                usleep(DEFAULT_SLEEP_US);
                waitTime -= DEFAULT_SLEEP_US;
            }

            if (waitTime <= 0)
            {
                SoapySDR_log(SOAPY_SDR_DEBUG, "readStream timed out");
                return SOAPY_SDR_TIMEOUT;
            }

            buff_ptr +=  rx_payload_size_in_bytes;
            block_ptr = p_rx_block[rxReadIndex];
            ringbuffer_ptr = (char *)((char *)block_ptr->data);

            // move to the next buffer in the ring
            rxReadIndex = (rxReadIndex + 1) % DEFAULT_NUM_BUFFERS;
        }
    }

    // if we are here then we have put NumElems into the buffer
    return numElems;
}

int SoapySidekiq::writeStream(SoapySDR::Stream * stream,
                              const void *const *buffs, const size_t numElems,
                              int &flags, const long long timeNs,
                              const long timeoutUs)
{
    int      status = 0;

    if (stream != TX_STREAM)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    if (numElems % current_tx_block_size != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "numElems must be a multiple of the TX MTU size "
                     " numElems %d, block size %u",
                     numElems, current_tx_block_size);
        throw std::runtime_error("");
    }

    // Pointer to the location in the input buffer to transmit from
    char *inbuff_ptr = (char *)(buffs[0]);

    uint32_t num_blocks = numElems / current_tx_block_size;

    uint32_t curr_block = 0;

    // total number of bytes that need to be transmitted in this call
    uint32_t tx_block_bytes = current_tx_block_size * 4;

    while (curr_block < num_blocks)
    {
        // Pointer to the location in the output buffer to copy to.
        char *outbuff_ptr =
                    (char *)p_tx_block[currTXBuffIndex]->data;

        // determine if we received short or float
        if (txUseShort == true)
        {
            // CS16
            memcpy(outbuff_ptr, inbuff_ptr, tx_block_bytes);
        }
        else
        {
            // float
            float *  float_inbuff = (float *)inbuff_ptr;
            uint32_t words_left = current_tx_block_size;
            uint16_t * new_outbuff = (uint16_t *)outbuff_ptr;

            int short_ctr = 0;
            for (uint32_t i = 0; i < words_left; i++)
            {
                new_outbuff[short_ctr + 1] = (uint16_t)(float_inbuff[short_ctr + 1] *
                                              this->maxValue);

                new_outbuff[short_ctr] = (uint16_t)(float_inbuff[short_ctr] *
                                          this->maxValue);
                short_ctr += 2;
            }
        }


        // need to make sure that we don't update the timestamp of a packet
        // that is already in use
        tx_buf_mutex.lock();
        if (p_tx_status[currTXBuffIndex] == 0)
        {
            p_tx_status[currTXBuffIndex] = 1;
        }
        else
        {
            tx_buf_mutex.unlock();
            pthread_mutex_lock(&space_avail_mutex);
            // wait for a packet to complete
            ready = false;
            pthread_cond_wait(&space_avail_cond, &space_avail_mutex);
            pthread_mutex_unlock(&space_avail_mutex);

            // space available so try again
            continue;
        }
        tx_buf_mutex.unlock();

        // Create the structure that is passed in p_user
        passedStructInstance = new passedStruct;
        passedStructInstance->classAddr = this;
        passedStructInstance->txIndex = currTXBuffIndex;

        // transmit the buffer
        status = skiq_transmit(this->card,
                               this->tx_hdl,
                               this->p_tx_block[currTXBuffIndex],
                               passedStructInstance);
        if (status == SKIQ_TX_ASYNC_SEND_QUEUE_FULL)
        {
            // update the in use status since we didn't actually send it yet
            tx_buf_mutex.lock();
            p_tx_status[currTXBuffIndex] = 0;
            tx_buf_mutex.unlock();

            // if there's no space left to send, wait until there should be space available
            pthread_mutex_lock(&space_avail_mutex);

            // wait for a packet to complete
            while (!ready)
            {
                pthread_cond_wait(&space_avail_cond, &space_avail_mutex);
            }
            ready = false;
            pthread_mutex_unlock(&space_avail_mutex);
        }
        else if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_transmit failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }
        else
        {
            curr_block++;

            // move the index into the transmit block array
            currTXBuffIndex = (currTXBuffIndex + 1) % DEFAULT_NUM_BUFFERS;

            // move the pointer to the next block in the writeStream buffer
            inbuff_ptr += (current_tx_block_size * 4);
 
        }

    }

    return numElems;
}

int SoapySidekiq::readStreamStatus(SoapySDR::Stream *stream,
                                  size_t &chanMask,
                                  int &flags,
                                  long long &timeNs,
                                  const long timeoutUs)
{
    int status = 0;
    uint32_t errors = 0;

    if (stream != TX_STREAM)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    /* This call will return a cumulative number of underruns since start
     * streaming */
    status = skiq_read_tx_num_underruns(this->card, this->tx_hdl, &errors);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_tx_num_underruns failed, (card %u) status %d",
                      this->card, status);
        throw std::runtime_error("");
    }

    // if the total changed since last call indicate UNDERFLOW
    if (errors > this->tx_underruns)
    {
        SoapySDR_logf(SOAPY_SDR_INFO,
                      "Number of underruns %u",
                      errors);
        this->tx_underruns = errors;
        return SOAPY_SDR_UNDERFLOW;
    }
    else
    {
        return 0;
    }

}


