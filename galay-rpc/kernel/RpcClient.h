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
#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcCodec.h"
#include "galay-rpc/protoc/RpcError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <atomic>
#include <expected>
#include <optional>

namespace galay::rpc
{

using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class RpcClientImpl;

inline RpcError ioErrorToRpcError(const IOError& io_error) {
    RpcErrorCode rpc_error_code = RpcErrorCode::INTERNAL_ERROR;
    if (io_error.code() == kTimeout) {
        rpc_error_code = RpcErrorCode::REQUEST_TIMEOUT;
    } else if (IOError::contains(io_error.code(), kDisconnectError)) {
        rpc_error_code = RpcErrorCode::CONNECTION_CLOSED;
    }
    return RpcError(rpc_error_code, io_error.message());
}

/**
 * @brief 发送完整RPC请求才完成的等待体
 */
class SendRpcRequestChainAwaitable : public SendAwaitable {
public:
    SendRpcRequestChainAwaitable(IOController* controller, RpcRequest&& request)
        : SendAwaitable(controller, nullptr, 0)
        , m_data(request.serialize())
        , m_total_sent(0)
    {
        m_buffer = m_data.data();
        m_length = m_data.size();
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        if (cqe == nullptr) {
            // io_uring 的 CustomAwaitable 首次会先探测一次，再提交SQE
            return m_length == 0;
        }
        if (!SendIOContext::handleComplete(cqe, handle)) {
            return false;
        }
        return handleSendResult();
    }
#else
    bool handleComplete(GHandle handle) override {
        if (!SendIOContext::handleComplete(handle)) {
            return false;
        }
        return handleSendResult();
    }
#endif

private:
    bool handleSendResult() {
        if (!m_result.has_value()) {
            return true;
        }

        const size_t sent_once = m_result.value();
        m_total_sent += sent_once;

        if (m_total_sent >= m_data.size()) {
            m_result = m_total_sent;
            return true;
        }

        if (sent_once == 0) {
            m_result = std::unexpected(IOError(kSendFailed, 0));
            return true;
        }

        m_buffer = m_data.data() + m_total_sent;
        m_length = m_data.size() - m_total_sent;
        return false;
    }

private:
    std::vector<char> m_data;
    size_t m_total_sent;
};

/**
 * @brief 协议解析完成才完成的响应接收等待体
 */
class RecvRpcResponseChainAwaitable : public RecvAwaitable {
public:
    RecvRpcResponseChainAwaitable(IOController* controller,
                                  RingBuffer& ring_buffer,
                                  const RpcReaderSetting& setting,
                                  RpcResponse& response)
        : RecvAwaitable(controller, nullptr, 0)
        , m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_response(response)
    {
        if (!prepareReadTarget()) {
            m_terminal = true;
        }
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        return handleCompleteImpl([&]() {
            if (cqe == nullptr) {
                return false;
            }
            return RecvIOContext::handleComplete(cqe, handle);
        });
    }
#else
    bool handleComplete(GHandle handle) override {
        return handleCompleteImpl([&]() {
            return RecvIOContext::handleComplete(handle);
        });
    }
#endif

    const std::expected<void, RpcError>& result() const {
        return m_rpc_result;
    }

private:
    template<typename CompleteFn>
    bool handleCompleteImpl(CompleteFn&& complete_fn) {
        if (m_terminal) {
            return true;
        }

        auto parsed = parseResponseFromRingBuffer();
        if (!parsed.has_value()) {
            m_rpc_result = std::unexpected(parsed.error());
            m_terminal = true;
            return true;
        }
        if (parsed.value()) {
            m_rpc_result = {};
            return true;
        }

        if (!prepareReadTarget()) {
            m_terminal = true;
            return true;
        }

        if (!complete_fn()) {
            return false;
        }

        if (!m_result.has_value()) {
            m_rpc_result = std::unexpected(ioErrorToRpcError(m_result.error()));
            m_terminal = true;
            return true;
        }

        const size_t bytes_read = m_result.value().size();
        if (bytes_read == 0) {
            m_rpc_result = std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
            m_terminal = true;
            return true;
        }

        m_ring_buffer.produce(bytes_read);

        parsed = parseResponseFromRingBuffer();
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

    bool prepareReadTarget() {
        auto iovecs = m_ring_buffer.getWriteIovecs();
        if (iovecs.empty() || iovecs.front().iov_len == 0) {
            m_rpc_result = std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                                    "No writable ring buffer space while receiving response"));
            return false;
        }

        m_buffer = static_cast<char*>(iovecs.front().iov_base);
        m_length = iovecs.front().iov_len;
        return true;
    }

    std::expected<bool, RpcError> parseResponseFromRingBuffer() {
        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        size_t total_readable = 0;
        for (const auto& iov : read_iovecs) {
            total_readable += iov.iov_len;
        }

        if (total_readable < RPC_HEADER_SIZE) {
            return false;
        }

        std::vector<char> linear_buffer(total_readable);
        size_t offset = 0;
        for (const auto& iov : read_iovecs) {
            std::memcpy(linear_buffer.data() + offset, iov.iov_base, iov.iov_len);
            offset += iov.iov_len;
        }

        size_t msg_len = RpcCodec::messageLength(linear_buffer.data(), linear_buffer.size());
        if (msg_len == 0) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Invalid message header"));
        }

        if (msg_len > m_setting.max_message_size + RPC_HEADER_SIZE) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Message too large"));
        }

        if (total_readable < msg_len) {
            return false;
        }

        auto result = RpcCodec::decodeResponse(linear_buffer.data(), msg_len);
        if (!result) {
            return std::unexpected(result.error());
        }

        m_response = std::move(result.value());
        m_ring_buffer.consume(msg_len);
        return true;
    }

private:
    RingBuffer& m_ring_buffer;
    const RpcReaderSetting& m_setting;
    RpcResponse& m_response;
    std::expected<void, RpcError> m_rpc_result;
    bool m_terminal = false;
};

/**
 * @brief RPC客户端调用等待体
 *
 * @details 继承 CustomAwaitable，组合 SEND -> RECV 两个任务：
 * - SEND：请求全部发送完成后推进到下一任务
 * - RECV：响应协议完整解析完成后唤醒协程
 */
template<typename SocketType>
class RpcCallAwaitableImpl : public CustomAwaitable, public TimeoutSupport<RpcCallAwaitableImpl<SocketType>>
{
public:
    RpcCallAwaitableImpl(RpcClientImpl<SocketType>& client, RpcRequest&& request)
        : CustomAwaitable(client.socket().controller())
        , m_response()
        , m_send_awaitable(client.socket().controller(), std::move(request))
        , m_recv_awaitable(client.socket().controller(),
                           client.ringBuffer(),
                           client.readerSetting(),
                           m_response)
    {
        addTask(IOEventType::SEND, &m_send_awaitable);
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    std::expected<std::optional<RpcResponse>, RpcError> await_resume() {
        onCompleted();
        m_completed = true;

        if (!m_result.has_value()) {
            return std::unexpected(ioErrorToRpcError(m_result.error()));
        }

        if (!m_send_awaitable.m_result.has_value()) {
            return std::unexpected(ioErrorToRpcError(m_send_awaitable.m_result.error()));
        }

        if (!m_recv_awaitable.result().has_value()) {
            return std::unexpected(m_recv_awaitable.result().error());
        }

        return std::optional<RpcResponse>(std::move(m_response));
    }

    bool isInvalid() const {
        return m_completed;
    }

private:
    RpcResponse m_response;
    SendRpcRequestChainAwaitable m_send_awaitable;
    RecvRpcResponseChainAwaitable m_recv_awaitable;
    bool m_completed = false;

public:
    // TimeoutSupport 需要访问此成员
    std::expected<std::optional<RpcResponse>, IOError> m_result;
};

/**
 * @brief RPC客户端配置
 */
struct RpcClientConfig {
    RpcReaderSetting reader_setting;
    RpcWriterSetting writer_setting;
    size_t ring_buffer_size = 8192;
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
        m_ring_buffer = std::make_unique<RingBuffer>(m_config.ring_buffer_size);

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
    RpcCallAwaitableImpl<SocketType>& call(const std::string& service, const std::string& method,
                                           const char* payload, size_t payload_len) {
        if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
            uint32_t req_id = m_request_id.fetch_add(1, std::memory_order_relaxed);
            RpcRequest request(req_id, service, method);
            if (payload && payload_len > 0) {
                request.payload(payload, payload_len);
            }
            m_awaitable.emplace(*this, std::move(request));
        }
        return *m_awaitable;
    }

    /**
     * @brief 调用远程方法（字符串payload）
     */
    RpcCallAwaitableImpl<SocketType>& call(const std::string& service, const std::string& method,
                                           const std::string& payload) {
        return call(service, method, payload.data(), payload.size());
    }

    /**
     * @brief 调用远程方法（无payload）
     */
    RpcCallAwaitableImpl<SocketType>& call(const std::string& service, const std::string& method) {
        return call(service, method, nullptr, 0);
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
    std::optional<RpcCallAwaitableImpl<SocketType>> m_awaitable;
};

// 类型别名
using RpcCallAwaitable = RpcCallAwaitableImpl<TcpSocket>;
using RpcClient = RpcClientImpl<TcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_CLIENT_H
