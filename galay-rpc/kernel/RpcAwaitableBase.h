/**
 * @file RpcAwaitableBase.h
 * @brief RPC builder/state-machine awaitable 辅助骨架
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 为 RPC 仓库内的读写 facade 提供共享的 state-machine helper，
 *          让上层 awaitable 通过 `AwaitableBuilder::fromStateMachine(...)`
 *          暴露，而不是继续继承手写 IO awaitable。
 */

#ifndef GALAY_RPC_AWAITABLE_BASE_H
#define GALAY_RPC_AWAITABLE_BASE_H

#include "galay-rpc/protoc/RpcError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/common/Buffer.h"

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace galay::rpc
{

using namespace galay::kernel;

namespace detail {

inline void consumeWritevIovecs(std::vector<iovec>& iovecs, size_t consumed)
{
    if (consumed == 0 || iovecs.empty()) {
        return;
    }

    size_t idx = 0;
    while (idx < iovecs.size() && consumed >= iovecs[idx].iov_len) {
        consumed -= iovecs[idx].iov_len;
        ++idx;
    }

    if (idx >= iovecs.size()) {
        iovecs.clear();
        return;
    }

    if (consumed > 0) {
        auto* base = static_cast<char*>(iovecs[idx].iov_base);
        iovecs[idx].iov_base = base + consumed;
        iovecs[idx].iov_len -= consumed;
    }

    if (idx > 0) {
        iovecs.erase(iovecs.begin(),
                     iovecs.begin() + static_cast<std::ptrdiff_t>(idx));
    }
}

inline bool prepareRingBufferReadWindow(RingBuffer& ring_buffer,
                                        std::array<struct iovec, 2>& read_iovecs,
                                        size_t& read_iov_count)
{
    read_iov_count = ring_buffer.getWriteIovecs(read_iovecs);
    size_t writable = 0;
    for (size_t i = 0; i < read_iov_count; ++i) {
        writable += read_iovecs[i].iov_len;
    }
    return writable > 0;
}

inline RpcError mapRpcReadError(const IOError& io_error,
                                RpcErrorCode default_code = RpcErrorCode::INTERNAL_ERROR)
{
    if (IOError::contains(io_error.code(), kDisconnectError)) {
        return RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed");
    }
    return RpcError::from(io_error, default_code);
}

template<typename ResultT = std::expected<bool, RpcError>>
class RpcRingBufferReadStateBase
{
public:
    using ResultType = ResultT;

    explicit RpcRingBufferReadStateBase(RingBuffer& ring_buffer)
        : m_ring_buffer(&ring_buffer)
    {
    }

    bool prepareReadWindow()
    {
        if (!prepareRingBufferReadWindow(*m_ring_buffer, m_read_iovecs, m_read_iov_count)) {
            m_error.emplace(RpcErrorCode::INTERNAL_ERROR, "No writable ring buffer space");
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_read_iovecs.data(); }
    size_t recvIovecsCount() const { return m_read_iov_count; }

    void setRecvError(const IOError& io_error)
    {
        m_error = mapRpcReadError(io_error);
    }

    void onPeerClosed()
    {
        m_error.emplace(RpcErrorCode::CONNECTION_CLOSED, "Connection closed");
    }

    void onBytesReceived(size_t bytes_read)
    {
        m_ring_buffer->produce(bytes_read);
    }

    ResultType takeResult()
    {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        return true;
    }

protected:
    RingBuffer& ringBuffer() { return *m_ring_buffer; }

    void setReadError(RpcError error)
    {
        m_error = std::move(error);
    }

private:
    RingBuffer* m_ring_buffer = nullptr;
    std::array<struct iovec, 2> m_read_iovecs{};
    size_t m_read_iov_count = 0;
    std::optional<RpcError> m_error;
};

template<typename ResultT = std::expected<bool, RpcError>>
class RpcWriteStateBase
{
public:
    using ResultType = ResultT;

    bool isComplete() const
    {
        return m_error.has_value() || m_iovecs.empty();
    }

    bool prepareWriteIovecs()
    {
        return true;
    }

    const struct iovec* writeIovecsData() const { return m_iovecs.data(); }
    size_t writeIovecsCount() const { return m_iovecs.size(); }

    void onBytesWritten(size_t bytes_written)
    {
        consumeWritevIovecs(m_iovecs, bytes_written);
    }

    void setSendError(const IOError& io_error)
    {
        m_error = RpcError::from(io_error, RpcErrorCode::INTERNAL_ERROR);
    }

    void onZeroWrite()
    {
        m_error = RpcError::from(IOError(kSendFailed, 0), RpcErrorCode::INTERNAL_ERROR);
    }

    ResultType takeResult()
    {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        return true;
    }

protected:
    std::vector<struct iovec>& mutableIovecs() { return m_iovecs; }

    void setWriteError(RpcError error)
    {
        m_error = std::move(error);
    }

private:
    std::vector<struct iovec> m_iovecs;
    std::optional<RpcError> m_error;
};

class RpcVectorWriteState : public RpcWriteStateBase<>
{
public:
    explicit RpcVectorWriteState(std::vector<char>&& data)
        : m_data(std::move(data))
    {
        if (!m_data.empty()) {
            mutableIovecs().push_back(iovec{m_data.data(), m_data.size()});
        }
    }

private:
    std::vector<char> m_data;
};

template<typename StateT>
struct RpcRingBufferReadMachine
{
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit RpcRingBufferReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state))
    {
    }

    MachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareReadWindow()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result)
    {
        if (!result.has_value()) {
            m_state->setRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError>)
    {
    }

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};

template<typename StateT>
struct RpcWritevMachine
{
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit RpcWritevMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state))
    {
    }

    MachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->isComplete()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareWriteIovecs()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitWritev(
            m_state->writeIovecsData(),
            m_state->writeIovecsCount());
    }

    void onRead(std::expected<size_t, IOError>)
    {
    }

    void onWrite(std::expected<size_t, IOError> result)
    {
        if (!result.has_value()) {
            m_state->setSendError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onZeroWrite();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesWritten(result.value());
        if (m_state->isComplete()) {
            m_result = m_state->takeResult();
        }
    }

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};

} // namespace detail

} // namespace galay::rpc

#endif // GALAY_RPC_AWAITABLE_BASE_H
