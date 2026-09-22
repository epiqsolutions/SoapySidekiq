#include <SoapySidekiq/SidekiqTxBackend.hpp>

#include <sidekiq_api.h>

#include <cstring>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <utility>

namespace
{

struct CompletionContext
{
    soapy_sidekiq::TxStreamBackend::Completion completion;
};

void completionCallback(
    const int32_t status,
    skiq_tx_block_t *block,
    void *user)
{
    std::unique_ptr<CompletionContext> context(
        static_cast<CompletionContext *>(user));
    if (context != nullptr && context->completion)
    {
        context->completion(status);
    }
    skiq_tx_block_free(block);
}

} // namespace

namespace soapy_sidekiq
{

SidekiqTxBackend::SidekiqTxBackend(const std::uint8_t card)
    : card_(card)
{
    const int status = skiq_register_tx_complete_callback(card_, completionCallback);
    if (status != 0)
    {
        throw std::runtime_error("failed to register Sidekiq TX completion callback");
    }
    registered_ = true;
}

SidekiqTxBackend::~SidekiqTxBackend()
{
    shutdown();
}

TxSendResult SidekiqTxBackend::transmit(
    const std::uint32_t handle,
    const std::vector<std::int16_t> &samples,
    Completion completion)
{
    if (samples.empty() || samples.size() % 2 != 0)
    {
        return {TxSendStatus::error, -EINVAL};
    }
    auto *block = skiq_tx_block_allocate(
        static_cast<std::uint32_t>(samples.size() / 2));
    if (block == nullptr)
    {
        return {TxSendStatus::error, -ENOMEM};
    }
    std::memcpy(
        block->data,
        samples.data(),
        samples.size() * sizeof(std::int16_t));
    auto context = std::make_unique<CompletionContext>();
    context->completion = std::move(completion);
    const int status = skiq_transmit(
        card_,
        static_cast<skiq_tx_hdl_t>(handle),
        block,
        context.get());
    if (status == 0)
    {
        context.release();
        return {TxSendStatus::accepted, 0};
    }

    skiq_tx_block_free(block);
    return {
        status == SKIQ_TX_ASYNC_SEND_QUEUE_FULL
            ? TxSendStatus::queue_full
            : TxSendStatus::error,
        status};
}

int SidekiqTxBackend::readUnderruns(
    const std::uint32_t handle,
    std::uint32_t &count)
{
    return skiq_read_tx_num_underruns(
        card_, static_cast<skiq_tx_hdl_t>(handle), &count);
}

void SidekiqTxBackend::shutdown() noexcept
{
    if (registered_)
    {
        (void)skiq_register_tx_complete_callback(card_, nullptr);
        registered_ = false;
    }
}

} // namespace soapy_sidekiq
