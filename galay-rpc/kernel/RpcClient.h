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
 *     // 带超时的调用
 *     while (true) {
 *         auto result = co_await client.call("EchoService", "echo", "Hello").setTimeout(5000);
 *         if (!result) {
 *             // 错误处理
 *             break;
 *         }
 *         if (result.value()) {
 *             auto& response = result.value().value();
 *             // 处理响应
 *             break;
 *         }
 *         // 继续等待
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

/**
 * @brief RPC客户端调用等待体
 *
 * @details 封装完整的RPC调用流程：发送请求 -> 接收响应
 * 继承TimeoutSupport以支持超时控制。
 *
 * 返回值说明：
 * - std::expected<std::optional<RpcResponse>, RpcError>
 * - 错误时返回 unexpected(RpcError)
 * - 成功但未完成时返回 std::nullopt（需要继续co_await）
 * - 完成时返回 RpcResponse
 */
template<typename SocketType>
class RpcCallAwaitableImpl : public TimeoutSupport<RpcCallAwaitableImpl<SocketType>>
{
public:
    using SendAwaitableType = SendRpcRequestAwaitable<SocketType>;
    using RecvAwaitableType = GetRpcResponseAwaitable<SocketType>;

    RpcCallAwaitableImpl(RpcClientImpl<SocketType>& client, RpcRequest&& request)
        : m_client(client)
        , m_request(std::move(request))
        , m_response()
        , m_state(State::Invalid)
        , m_send_awaitable(std::nullopt)
        , m_recv_awaitable(std::nullopt)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (m_state == State::Invalid) {
            m_state = State::Sending;
            m_send_awaitable.emplace(m_client.getWriter().sendRequest(m_request));
            return m_send_awaitable->await_suspend(handle);
        } else if (m_state == State::Sending) {
            m_send_awaitable.emplace(m_client.getWriter().sendRequest(m_request));
            return m_send_awaitable->await_suspend(handle);
        } else {
            m_recv_awaitable.emplace(m_client.getReader().getResponse(m_response));
            return m_recv_awaitable->await_suspend(handle);
        }
    }

    std::expected<std::optional<RpcResponse>, RpcError> await_resume() {
        // 检查超时结果
        if (!m_result.has_value()) {
            auto& io_error = m_result.error();

            RpcErrorCode rpc_error_code;
            if (io_error.code() == kTimeout) {
                rpc_error_code = RpcErrorCode::REQUEST_TIMEOUT;
            } else if (IOError::contains(io_error.code(), kDisconnectError)) {
                rpc_error_code = RpcErrorCode::CONNECTION_CLOSED;
            } else {
                rpc_error_code = RpcErrorCode::INTERNAL_ERROR;
            }

            reset();
            return std::unexpected(RpcError(rpc_error_code, io_error.message()));
        }

        if (m_state == State::Sending) {
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                reset();
                return std::unexpected(send_result.error());
            }

            if (!send_result.value()) {
                // 发送未完成，继续
                return std::nullopt;
            }

            // 发送完成，切换到接收状态
            m_state = State::Receiving;
            m_send_awaitable.reset();
            return std::nullopt;
        } else if (m_state == State::Receiving) {
            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                reset();
                return std::unexpected(recv_result.error());
            }

            if (!recv_result.value()) {
                // 接收未完成，继续
                return std::nullopt;
            }

            // 接收完成
            auto response = std::move(m_response);
            reset();
            return response;
        } else {
            reset();
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, "RpcCallAwaitable in Invalid state"));
        }
    }

    bool isInvalid() const {
        return m_state == State::Invalid;
    }

    void reset() {
        m_state = State::Invalid;
        m_send_awaitable.reset();
        m_recv_awaitable.reset();
        m_response = RpcResponse();
        m_result = std::nullopt;
    }

private:
    enum class State {
        Invalid,
        Sending,
        Receiving
    };

    RpcClientImpl<SocketType>& m_client;
    RpcRequest m_request;
    RpcResponse m_response;
    State m_state;

    std::optional<SendAwaitableType> m_send_awaitable;
    std::optional<RecvAwaitableType> m_recv_awaitable;

public:
    // TimeoutSupport需要访问此成员
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
     *
     * @example
     * @code
     * // 基本调用
     * while (true) {
     *     auto result = co_await client.call("Service", "method", data, len);
     *     if (!result) break;  // 错误
     *     if (result.value()) {
     *         auto& response = result.value().value();
     *         break;  // 完成
     *     }
     * }
     *
     * // 带超时调用
     * while (true) {
     *     auto result = co_await client.call("Service", "method", data, len).setTimeout(5000);
     *     // ...
     * }
     * @endcode
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
