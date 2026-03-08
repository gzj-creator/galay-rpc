/**
 * @file RpcAwaitableBase.h
 * @brief RPC等待体CRTP基类
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供读/写等待体的公共骨架，消除重复代码。
 *          Derived类只需实现特定的解析/构建逻辑。
 */

#ifndef GALAY_RPC_AWAITABLE_BASE_H
#define GALAY_RPC_AWAITABLE_BASE_H

#include "galay-rpc/protoc/RpcError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/common/Buffer.h"
#include <array>
#include <expected>
#include <optional>
#include <vector>

namespace galay::rpc
{

using namespace galay::kernel;

namespace detail {

inline std::array<struct iovec, 1>& emptyIovecs() {
    static std::array<struct iovec, 1> empty{};
    return empty;
}

inline void consumeWritevIovecs(std::vector<iovec>& iovecs, size_t consumed) {
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

} // namespace detail

/**
 * @brief 基于RingBuffer的读等待体CRTP基类
 *
 * @details Derived必须实现:
 *   - std::expected<bool, RpcError> parseFromRingBuffer()
 *     返回 true=解析完成, false=数据不足, unexpected=解析错误
 */
template<typename Derived, typename SocketType>
class RingBufferReadAwaitable : public ReadvAwaitable
{
public:
    RingBufferReadAwaitable(RingBuffer& ring_buffer, SocketType& socket)
        : ReadvAwaitable(socket.controller(), detail::emptyIovecs(), 0)
        , m_ring_buffer(ring_buffer)
    {}

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        auto parse_result = derived().parseFromRingBuffer();
        if (!parse_result.has_value() || parse_result.value()) {
            m_cached_result = std::move(parse_result);
            return false;
        }
        if (!prepareReadIovecs()) {
            m_cached_result = std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, "No writable ring buffer space"));
            return false;
        }
        return ReadvAwaitable::await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        if (m_cached_result.has_value()) {
            auto result = std::move(*m_cached_result);
            m_cached_result.reset();
            return result;
        }
        auto readv_result = ReadvAwaitable::await_resume();
        if (!readv_result) {
            if (IOError::contains(readv_result.error().code(), kDisconnectError)) {
                return std::unexpected(
                    RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
            }
            return std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, readv_result.error().message()));
        }
        if (m_parse_error.has_value()) {
            return std::unexpected(std::move(*m_parse_error));
        }
        return true;
    }

protected:
    RingBuffer& ringBuffer() { return m_ring_buffer; }

private:
    Derived& derived() { return static_cast<Derived&>(*this); }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        return handleCompleteImpl([&]() {
            if (cqe == nullptr) return false;
            return ReadvIOContext::handleComplete(cqe, handle);
        });
    }
#else
    bool handleComplete(GHandle handle) override {
        return handleCompleteImpl([&]() {
            return ReadvIOContext::handleComplete(handle);
        });
    }
#endif

    template<typename CompleteFn>
    bool handleCompleteImpl(CompleteFn&& complete_fn) {
        if (!complete_fn()) return false;
        if (!m_result.has_value()) return true;

        const size_t bytes_read = m_result.value();
        if (bytes_read == 0) {
            m_parse_error = RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed");
            return true;
        }
        m_ring_buffer.produce(bytes_read);

        auto parse_result = derived().parseFromRingBuffer();
        if (!parse_result.has_value()) {
            m_parse_error = parse_result.error();
            return true;
        }
        if (parse_result.value()) return true;

        if (!prepareReadIovecs()) {
            m_parse_error = RpcError(RpcErrorCode::INTERNAL_ERROR,
                                     "No writable ring buffer space");
            return true;
        }
        return false;
    }

    bool prepareReadIovecs() {
        m_read_iovec_count = m_ring_buffer.getWriteIovecs(m_read_iovecs);
        ReadvIOContext::m_iovecs = std::span<const struct iovec>(m_read_iovecs.data(), m_read_iovec_count);
        size_t writable = 0;
        for (size_t i = 0; i < m_read_iovec_count; ++i) {
            writable += m_read_iovecs[i].iov_len;
        }
        return writable > 0;
    }

private:
    RingBuffer& m_ring_buffer;
    std::array<struct iovec, 2> m_read_iovecs{};
    size_t m_read_iovec_count = 0;
    std::optional<std::expected<bool, RpcError>> m_cached_result;
    std::optional<RpcError> m_parse_error;
};

/**
 * @brief 可续写的写等待体CRTP基类
 *
 * @details Derived必须:
 *   1. 在构造函数中填充 m_iovecs 和设置 m_total_bytes
 *   2. 可选重写 await_ready() / await_resume()
 */
template<typename Derived, typename SocketType>
class ResumableWriteAwaitable : public WritevAwaitable
{
public:
    explicit ResumableWriteAwaitable(SocketType& socket)
        : WritevAwaitable(socket.controller(), detail::emptyIovecs(), 0)
    {}

    template<typename Handle>
    auto await_suspend(Handle handle) {
        syncWriteIovecs();
        return WritevAwaitable::await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto writev_result = WritevAwaitable::await_resume();
        if (!writev_result.has_value()) {
            return std::unexpected(
                RpcError::from(writev_result.error(), RpcErrorCode::INTERNAL_ERROR));
        }
        return true;
    }

private:
#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        if (cqe == nullptr) {
            return m_iovecs.empty();
        }
        if (!WritevIOContext::handleComplete(cqe, handle)) {
            return false;
        }
        return handleWriteResult();
    }
#else
    bool handleComplete(GHandle handle) override {
        if (!WritevIOContext::handleComplete(handle)) {
            return false;
        }
        return handleWriteResult();
    }
#endif

    bool handleWriteResult() {
        if (!m_result.has_value()) {
            return true;
        }

        const size_t sent_once = m_result.value();
        if (sent_once == 0) {
            m_result = std::unexpected(IOError(kSendFailed, 0));
            return true;
        }

        m_total_sent += sent_once;
        if (m_total_sent >= m_total_bytes) {
            m_result = m_total_sent;
            syncWriteIovecs();
            return true;
        }

        detail::consumeWritevIovecs(m_iovecs, sent_once);
        syncWriteIovecs();
        return false;
    }

protected:
    void syncWriteIovecs() {
        WritevIOContext::m_iovecs = std::span<const struct iovec>(m_iovecs.data(), m_iovecs.size());
    }

    std::vector<iovec> m_iovecs;
    size_t m_total_bytes = 0;
    size_t m_total_sent = 0;
};

} // namespace galay::rpc

#endif // GALAY_RPC_AWAITABLE_BASE_H
