/**
 * @file RpcClient.h
 * @brief RPC客户端
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC客户端功能，支持异步调用和超时控制。
 *
 * @example
 * @code
 * Coroutine callEcho(Runtime& runtime) {
 *     RpcClient client;
 *     auto connect_result = co_await client.connect("127.0.0.1", 9000);
 *     if (!connect_result) {
 *         co_return;
 *     }
 *
 *     auto result = co_await client.call("EchoService", "echo", "Hello").timeout(std::chrono::milliseconds(5000));
 *     if (result && result.value()) {
 *         auto& response = result.value().value();
 *         // 处理响应
 *     }
 *
 *     co_await client.close();
 * }
 * @endcode
 */

#ifndef GALAY_RPC_CLIENT_H
#define GALAY_RPC_CLIENT_H

#include "RpcConn.h"
#include "RpcStream.h"
#include "galay-rpc/protoc/RpcError.h"
#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <array>
#include <atomic>
#include <expected>
#include <memory>
#include <optional>

namespace galay::rpc
{

using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class RpcClientImpl;

template<typename SocketType>
class RecvRpcResponseChainAwaitable : public ReadvIOContext {
public:
    RecvRpcResponseChainAwaitable(RingBuffer& ring_buffer,
                                  const RpcReaderSetting& setting,
                                  uint32_t expected_request_id,
                                  RpcResponse& response)
        : ReadvIOContext(emptyIovecs(), 0)
        , m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_expected_request_id(expected_request_id)
        , m_response(response)
    {
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        return handleCompleteImpl([&]() {
            if (cqe == nullptr) {
                return false;
            }
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

    const std::expected<void, RpcError>& result() const {
        return m_rpc_result;
    }

private:
    static std::array<struct iovec, 1>& emptyIovecs() {
        static std::array<struct iovec, 1> empty{};
        return empty;
    }

    template<typename CompleteFn>
    bool handleCompleteImpl(CompleteFn&& complete_fn) {
        if (m_terminal) {
            return true;
        }

        auto parsed = tryParseFromRingBuffer();
        if (!parsed.has_value()) {
            m_rpc_result = std::unexpected(parsed.error());
            m_terminal = true;
            return true;
        }
        if (parsed.value()) {
            m_rpc_result = {};
            return true;
        }

        if (!prepareReadIovecs()) {
            m_terminal = true;
            return true;
        }

        if (!complete_fn()) {
            return false;
        }

        if (!m_result.has_value()) {
            m_rpc_result = std::unexpected(RpcError::from(m_result.error()));
            m_terminal = true;
            return true;
        }

        const size_t bytes_read = m_result.value();
        if (bytes_read == 0) {
            m_rpc_result = std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
            m_terminal = true;
            return true;
        }

        m_ring_buffer.produce(bytes_read);

        parsed = tryParseFromRingBuffer();
        if (!parsed.has_value()) {
            m_rpc_result = std::unexpected(parsed.error());
            m_terminal = true;
            return true;
        }

        if (parsed.value()) {
            m_rpc_result = {};
            return true;
        }

        return false;
    }

    bool prepareReadIovecs() {
        m_read_iovec_count = m_ring_buffer.getWriteIovecs(m_read_iovecs);
        ReadvIOContext::m_iovecs = std::span<const struct iovec>(m_read_iovecs.data(), m_read_iovec_count);
        if (m_read_iovec_count == 0 || m_read_iovecs[0].iov_len == 0) {
            m_rpc_result = std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                                    "No writable ring buffer space while receiving response"));
            return false;
        }
        return true;
    }

    std::expected<bool, RpcError> tryParseFromRingBuffer() {
        if (m_ring_buffer.readable() == 0) {
            return false;
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovec_count = m_ring_buffer.getReadIovecs(read_iovecs);
        if (read_iovec_count == 0) {
            return false;
        }

        const std::span<const struct iovec> read_span(read_iovecs.data(), read_iovec_count);
        const size_t total_readable = detail::iovecsReadableBytes(read_span);
        auto parse_result = detail::tryParseResponseMessage(read_span,
                                                            total_readable,
                                                            m_setting.max_message_size,
                                                            m_response);
        if (!parse_result.has_value()) {
            return std::unexpected(parse_result.error());
        }

        if (parse_result.value() == 0) {
            return false;
        }

        if (m_response.requestId() != m_expected_request_id) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                            "Mismatched response request id"));
        }

        m_ring_buffer.consume(parse_result.value());
        return true;
    }

private:
    RingBuffer& m_ring_buffer;
    const RpcReaderSetting& m_setting;
    uint32_t m_expected_request_id;
    RpcResponse& m_response;
    std::array<struct iovec, 2> m_read_iovecs{};
    size_t m_read_iovec_count = 0;
    std::expected<void, RpcError> m_rpc_result;
    bool m_terminal = false;
};

/**
 * @brief RPC客户端调用等待体
 *
 * @details 继承 CustomAwaitable，串联 WRITEV -> READV 两个任务。
 */
template<typename SocketType>
class RpcCallAwaitableImpl : public CustomAwaitable, public TimeoutSupport<RpcCallAwaitableImpl<SocketType>>
{
public:
    RpcCallAwaitableImpl(RpcClientImpl<SocketType>& client, RpcRequest&& request)
        : CustomAwaitable(client.socket().controller())
        , m_request(std::move(request))
        , m_response()
        , m_send_awaitable(m_request, client.socket())
        , m_recv_awaitable(client.ringBuffer(),
                           client.readerSetting(),
                           m_request.requestId(),
                           m_response)
    {
        addTask(IOEventType::WRITEV, &m_send_awaitable);
        addTask(IOEventType::READV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    std::expected<std::optional<RpcResponse>, RpcError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            return std::unexpected(RpcError::from(m_result.error()));
        }

        if (!m_send_awaitable.m_result.has_value()) {
            return std::unexpected(RpcError::from(m_send_awaitable.m_result.error()));
        }

        if (!m_recv_awaitable.result().has_value()) {
            return std::unexpected(m_recv_awaitable.result().error());
        }

        if (m_response.callMode() != m_request.callMode()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Mismatched response call mode"));
        }

        return std::optional<RpcResponse>(std::move(m_response));
    }

public:
    // TimeoutSupport 需要访问此成员
    std::expected<std::optional<RpcResponse>, IOError> m_result;

private:
    RpcRequest m_request;
    RpcResponse m_response;
    SendRpcRequestAwaitable<SocketType> m_send_awaitable;
    RecvRpcResponseChainAwaitable<SocketType> m_recv_awaitable;
};

/**
 * @brief RPC客户端配置
 */
struct RpcClientConfig {
    RpcReaderSetting reader_setting;
    RpcWriterSetting writer_setting;
    size_t ring_buffer_size = kDefaultRpcRingBufferSize;
};

class RpcClientBuilder {
public:
    RpcClientBuilder& readerSetting(RpcReaderSetting setting) { m_config.reader_setting = std::move(setting); return *this; }
    RpcClientBuilder& writerSetting(RpcWriterSetting setting) { m_config.writer_setting = std::move(setting); return *this; }
    RpcClientBuilder& ringBufferSize(size_t size)             { m_config.ring_buffer_size = size; return *this; }
    RpcClientImpl<TcpSocket> build() const;
    RpcClientConfig buildConfig() const                       { return m_config; }

private:
    RpcClientConfig m_config;
};

/**
 * @brief RPC客户端模板类
 */
template<typename SocketType>
class RpcClientImpl {
public:
    /**
     * @brief 构造函数
     */
    explicit RpcClientImpl(const RpcClientConfig& config = RpcClientConfig())
        : m_socket(nullptr)
        , m_ring_buffer(nullptr)
        , m_config(config)
        , m_request_id(0)
        , m_stream_id(1)
    {
    }

    ~RpcClientImpl() = default;

    // 禁止拷贝和移动
    RpcClientImpl(const RpcClientImpl&) = delete;
    RpcClientImpl& operator=(const RpcClientImpl&) = delete;
    RpcClientImpl(RpcClientImpl&&) = delete;
    RpcClientImpl& operator=(RpcClientImpl&&) = delete;

    /**
     * @brief 连接到服务器
     * @param host 服务器地址
     * @param port 服务器端口
     * @return 连接等待体
     */
    ConnectAwaitable connect(const std::string& host, uint16_t port) {
        m_socket = std::make_unique<SocketType>(IPType::IPV4);

        const size_t ring_buffer_size = m_config.ring_buffer_size == 0
            ? kDefaultRpcRingBufferSize
            : m_config.ring_buffer_size;
        m_ring_buffer = std::make_unique<RingBuffer>(ring_buffer_size);

        m_socket->option().handleNonBlock();

        Host server_host(IPType::IPV4, host, port);
        return m_socket->connect(server_host);
    }

    /**
     * @brief 调用远程方法
     * @param service 服务名
     * @param method 方法名
     * @param payload 请求数据
     * @param payload_len 数据长度
     * @return RPC调用等待体（支持超时）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method,
                                          const char* payload,
                                          size_t payload_len) {
        return callWithMode(service, method, RpcCallMode::UNARY, true, payload, payload_len);
    }

    /**
     * @brief 按调用模式发送RPC帧（为流式RPC预留）
     *
     * @note 当前仍走一次请求对应一次响应链路；后续流式模式会复用该元信息扩展多帧流程。
     */
    RpcCallAwaitableImpl<SocketType> callWithMode(const std::string& service,
                                                  const std::string& method,
                                                  RpcCallMode mode,
                                                  bool end_of_stream,
                                                  const char* payload,
                                                  size_t payload_len) {
        uint32_t req_id = m_request_id.fetch_add(1, std::memory_order_relaxed);
        RpcRequest request(req_id, service, method);
        request.callMode(mode);
        request.endOfStream(end_of_stream);
        if (payload && payload_len > 0) {
            request.payload(payload, payload_len);
        }
        return RpcCallAwaitableImpl<SocketType>(*this, std::move(request));
    }

    /**
     * @brief 调用远程方法（字符串payload）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method,
                                          const std::string& payload) {
        return call(service, method, payload.data(), payload.size());
    }

    /**
     * @brief 调用远程方法（无payload）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method) {
        return call(service, method, nullptr, 0);
    }

    /**
     * @brief 客户端流帧发送（N frame -> 1 response）
     */
    RpcCallAwaitableImpl<SocketType> callClientStreamFrame(const std::string& service,
                                                           const std::string& method,
                                                           const char* payload,
                                                           size_t payload_len,
                                                           bool end_of_stream) {
        return callWithMode(service, method, RpcCallMode::CLIENT_STREAMING, end_of_stream, payload, payload_len);
    }

    /**
     * @brief 服务端流请求（1 request -> N response frame）
     */
    RpcCallAwaitableImpl<SocketType> callServerStreamRequest(const std::string& service,
                                                             const std::string& method,
                                                             const char* payload,
                                                             size_t payload_len) {
        return callWithMode(service, method, RpcCallMode::SERVER_STREAMING, true, payload, payload_len);
    }

    /**
     * @brief 双向流帧发送（N frame <-> N frame）
     */
    RpcCallAwaitableImpl<SocketType> callBidiStreamFrame(const std::string& service,
                                                         const std::string& method,
                                                         const char* payload,
                                                         size_t payload_len,
                                                         bool end_of_stream) {
        return callWithMode(service, method, RpcCallMode::BIDI_STREAMING, end_of_stream, payload, payload_len);
    }

    /**
     * @brief 创建流会话（自动分配 stream_id）
     *
     * @note 仅创建会话对象，不会自动执行 STREAM_INIT。
     */
    std::expected<RpcStreamImpl<SocketType>, RpcError> createStream(const std::string& service,
                                                                     const std::string& method) {
        const uint32_t stream_id = m_stream_id.fetch_add(1, std::memory_order_relaxed);
        return createStream(stream_id, service, method);
    }

    /**
     * @brief 创建流会话（显式指定 stream_id）
     *
     * @note 仅创建会话对象，不会自动执行 STREAM_INIT。
     */
    std::expected<RpcStreamImpl<SocketType>, RpcError> createStream(uint32_t stream_id,
                                                                     const std::string& service = {},
                                                                     const std::string& method = {}) {
        if (!m_socket || !m_ring_buffer) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED,
                                            "Client is not connected"));
        }
        return RpcStreamImpl<SocketType>(*m_socket, *m_ring_buffer, stream_id, service, method);
    }

    /**
     * @brief 关闭连接
     */
    CloseAwaitable close() {
        return m_socket->close();
    }

    /**
     * @brief 获取读取器
     */
    RpcReaderImpl<SocketType> getReader() {
        return RpcReaderImpl<SocketType>(*m_ring_buffer, m_config.reader_setting, *m_socket);
    }

    /**
     * @brief 获取写入器
     */
    RpcWriterImpl<SocketType> getWriter() {
        return RpcWriterImpl<SocketType>(m_config.writer_setting, *m_socket);
    }

    /**
     * @brief 获取底层socket
     */
    SocketType& socket() { return *m_socket; }

    /**
     * @brief 获取RingBuffer
     */
    RingBuffer& ringBuffer() { return *m_ring_buffer; }

    /**
     * @brief 获取读取配置
     */
    const RpcReaderSetting& readerSetting() const { return m_config.reader_setting; }

private:
    std::unique_ptr<SocketType> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;
    RpcClientConfig m_config;
    std::atomic<uint32_t> m_request_id;
    std::atomic<uint32_t> m_stream_id;
};

// 类型别名
using RpcCallAwaitable = RpcCallAwaitableImpl<TcpSocket>;
using RpcClient = RpcClientImpl<TcpSocket>;
inline RpcClient RpcClientBuilder::build() const { return RpcClient(m_config); }

} // namespace galay::rpc

#endif // GALAY_RPC_CLIENT_H
