/**
 * @file RpcStreamServer.h
 * @brief 真实流式 RPC 服务器
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 基于 STREAM_INIT/STREAM_DATA/STREAM_END 协议，
 *          按 service/method 路由到 RpcService::registerStreamMethod 注册的回调。
 */

#ifndef GALAY_RPC_STREAM_SERVER_H
#define GALAY_RPC_STREAM_SERVER_H

#include "RpcService.h"
#include "RpcStream.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace galay::rpc
{

using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 流式 RPC 服务器配置
 */
struct RpcStreamServerConfig {
    std::string host = "0.0.0.0";       ///< 监听地址
    uint16_t port = 9100;               ///< 监听端口
    int backlog = 1024;                 ///< 监听队列长度
    size_t io_scheduler_count = 0;      ///< IO 调度器数量，0 表示自动
    size_t compute_scheduler_count = 0; ///< 计算调度器数量，0 表示自动
    size_t ring_buffer_size = 128 * 1024; ///< 每连接 RingBuffer 大小
};

/**
 * @brief 真实流式 RPC 服务器
 *
 * @note 当前每条连接同一时刻只处理一个活跃流会话（顺序处理）。
 */
class RpcStreamServer {
public:
    explicit RpcStreamServer(const RpcStreamServerConfig& config)
        : m_config(config)
        , m_runtime(config.io_scheduler_count,
                    config.compute_scheduler_count) {}

    ~RpcStreamServer() {
        stop();
    }

    void registerService(std::shared_ptr<RpcService> service) {
        m_services[service->name()] = std::move(service);
    }

    void start() {
        m_running.store(true, std::memory_order_release);
        m_runtime.start();
        m_runtime.getNextIOScheduler()->spawn(acceptLoop());
    }

    void stop() {
        if (m_running.exchange(false, std::memory_order_acq_rel)) {
            m_runtime.stop();
        }
    }

    bool isRunning() const {
        return m_running.load(std::memory_order_acquire);
    }

    Runtime& runtime() { return m_runtime; }

    std::optional<RpcError> lastError() const {
        return m_last_error;
    }

private:
    std::expected<RpcStreamHandler*, RpcErrorCode> resolveStreamHandler(const StreamInitRequest& init_req) {
        auto service_it = m_services.find(init_req.serviceName());
        if (service_it == m_services.end()) {
            return std::unexpected(RpcErrorCode::SERVICE_NOT_FOUND);
        }

        auto* handler = service_it->second->findStreamMethod(init_req.methodName());
        if (handler == nullptr) {
            return std::unexpected(RpcErrorCode::METHOD_NOT_FOUND);
        }

        return handler;
    }

    Coroutine acceptLoop() {
        m_last_error.reset();

        TcpSocket listener(IPType::IPV4);
        auto reuse_addr_result = listener.option().handleReuseAddr();
        if (!reuse_addr_result) {
            m_last_error = RpcError::from(reuse_addr_result.error());
            co_return;
        }

        auto non_block_result = listener.option().handleNonBlock();
        if (!non_block_result) {
            m_last_error = RpcError::from(non_block_result.error());
            co_return;
        }

        Host host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(host);
        if (!bind_result) {
            m_last_error = RpcError::from(bind_result.error());
            co_return;
        }

        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            m_last_error = RpcError::from(listen_result.error());
            co_return;
        }

        while (m_running.load(std::memory_order_acquire)) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);
            if (!accept_result) {
                m_last_error = RpcError::from(accept_result.error());
                continue;
            }

            m_runtime.getNextIOScheduler()->spawn(handleConnection(accept_result.value()));
        }

        auto close_result = co_await listener.close();
        if (!close_result) {
            m_last_error = RpcError::from(close_result.error());
        }
        co_return;
    }

    Coroutine handleConnection(GHandle handle) {
        TcpSocket socket(handle);
        auto non_block_result = socket.option().handleNonBlock();
        if (!non_block_result) {
            m_last_error = RpcError::from(non_block_result.error());
            auto close_result = co_await socket.close();
            if (!close_result) {
                m_last_error = RpcError::from(close_result.error());
            }
            co_return;
        }

        RingBuffer ring_buffer(m_config.ring_buffer_size == 0 ? 128 * 1024 : m_config.ring_buffer_size);
        StreamReader reader(ring_buffer, socket);

        while (m_running.load(std::memory_order_acquire)) {
            StreamMessage init_frame;
            auto recv_result = co_await reader.getMessage(init_frame);
            if (!recv_result.has_value()) {
                m_last_error = recv_result.error();
                auto close_result = co_await socket.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                }
                co_return;
            }

            const uint32_t stream_id = init_frame.streamId();
            RpcStream stream(socket, ring_buffer, stream_id);

            if (init_frame.messageType() != RpcMessageType::STREAM_INIT) {
                m_last_error = RpcError(RpcErrorCode::INVALID_REQUEST,
                                        "Expected STREAM_INIT as first frame");
                auto cancel_result = co_await stream.sendCancel();
                if (!cancel_result.has_value()) {
                    m_last_error = cancel_result.error();
                    auto close_result = co_await socket.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                    }
                    co_return;
                }
                continue;
            }

            StreamInitRequest init_req;
            if (!init_req.deserializeBody(init_frame.payload().data(), init_frame.payload().size())) {
                m_last_error = RpcError(RpcErrorCode::INVALID_REQUEST,
                                        "Failed to parse stream init body");
                auto cancel_result = co_await stream.sendCancel();
                if (!cancel_result.has_value()) {
                    m_last_error = cancel_result.error();
                    auto close_result = co_await socket.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                    }
                    co_return;
                }
                continue;
            }

            auto handler_result = resolveStreamHandler(init_req);
            if (!handler_result.has_value()) {
                m_last_error = RpcError(handler_result.error());
                auto cancel_result = co_await stream.sendCancel();
                if (!cancel_result.has_value()) {
                    m_last_error = cancel_result.error();
                    auto close_result = co_await socket.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                    }
                    co_return;
                }
                continue;
            }

            stream.setRoute(init_req.serviceName(), init_req.methodName());

            auto send_result = co_await stream.sendInitAck();
            if (!send_result.has_value()) {
                m_last_error = send_result.error();
                auto close_result = co_await socket.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                }
                co_return;
            }

            auto* handler = handler_result.value();
            co_await (*handler)(stream).wait();
        }

        auto close_result = co_await socket.close();
        if (!close_result) {
            m_last_error = RpcError::from(close_result.error());
        }
        co_return;
    }

private:
    RpcStreamServerConfig m_config;
    Runtime m_runtime;
    std::unordered_map<std::string, std::shared_ptr<RpcService>> m_services;
    std::atomic<bool> m_running{false};
    std::optional<RpcError> m_last_error;
};

} // namespace galay::rpc

#endif // GALAY_RPC_STREAM_SERVER_H
