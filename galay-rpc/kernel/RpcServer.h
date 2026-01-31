/**
 * @file RpcServer.h
 * @brief RPC服务器
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC服务器功能，支持服务注册和请求分发。
 *
 * @example
 * @code
 * // 创建服务
 * auto echoService = std::make_shared<EchoService>();
 *
 * // 配置服务器
 * RpcServerConfig config;
 * config.host = "0.0.0.0";
 * config.port = 9000;
 *
 * // 启动服务器
 * RpcServer server(config);
 * server.registerService(echoService);
 * server.start();
 * @endcode
 */

#ifndef GALAY_RPC_SERVER_H
#define GALAY_RPC_SERVER_H

#include "RpcService.h"
#include "RpcConn.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/async/TcpSocket.h"
#include <memory>
#include <unordered_map>
#include <atomic>

namespace galay::rpc
{

using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief RPC服务器配置
 */
struct RpcServerConfig {
    std::string host = "0.0.0.0";       ///< 监听地址
    uint16_t port = 9000;               ///< 监听端口
    int backlog = 128;                  ///< 监听队列长度
    size_t io_scheduler_count = 0;      ///< IO调度器数量，0表示自动
    size_t compute_scheduler_count = 0; ///< 计算调度器数量，0表示自动
    size_t ring_buffer_size = 8192;     ///< RingBuffer大小
};

/**
 * @brief RPC服务器
 */
class RpcServer {
public:
    /**
     * @brief 构造函数
     * @param config 服务器配置
     */
    explicit RpcServer(const RpcServerConfig& config)
        : m_config(config)
        , m_runtime(config.io_scheduler_count,
                    config.compute_scheduler_count) {}

    ~RpcServer() {
        stop();
    }

    /**
     * @brief 注册服务
     * @param service 服务实例
     */
    void registerService(std::shared_ptr<RpcService> service) {
        m_services[service->name()] = std::move(service);
    }

    /**
     * @brief 启动服务器
     */
    void start() {
        m_running.store(true, std::memory_order_release);
        m_runtime.start();

        auto* scheduler = m_runtime.getNextIOScheduler();
        scheduler->spawn(acceptLoop());
    }

    /**
     * @brief 停止服务器
     */
    void stop() {
        if (m_running.exchange(false, std::memory_order_acq_rel)) {
            m_runtime.stop();
        }
    }

    /**
     * @brief 检查是否运行中
     */
    bool isRunning() const {
        return m_running.load(std::memory_order_acquire);
    }

    /**
     * @brief 获取Runtime
     */
    Runtime& runtime() { return m_runtime; }

private:
    /**
     * @brief 接受连接循环
     */
    Coroutine acceptLoop() {
        TcpSocket listener(IPType::IPV4);
        listener.option().handleReuseAddr();
        listener.option().handleNonBlock();

        Host host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(host);
        if (!bind_result) {
            co_return;
        }

        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            co_return;
        }

        while (m_running.load(std::memory_order_acquire)) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);
            if (!accept_result) {
                continue;
            }

            // 分发到下一个IO调度器处理
            auto* scheduler = m_runtime.getNextIOScheduler();
            scheduler->spawn(handleConnection(accept_result.value()));
        }

        co_await listener.close();
        co_return;
    }

    /**
     * @brief 处理连接
     */
    Coroutine handleConnection(GHandle handle) {
        RpcConn conn(handle);

        while (m_running.load(std::memory_order_acquire)) {
            // 读取请求（循环等待完整消息）
            RpcRequest request;
            auto reader = conn.getReader();

            while (true) {
                auto result = co_await reader.getRequest(request);
                if (!result) {
                    // 错误，关闭连接
                    co_await conn.close();
                    co_return;
                }
                if (result.value()) {
                    break;  // 解析完成
                }
                // result.value() == false，继续读取
            }

            // 处理请求
            RpcResponse response(request.requestId());
            co_await processRequest(request, response).wait();

            // 发送响应（循环等待发送完成）
            auto writer = conn.getWriter();
            while (true) {
                auto result = co_await writer.sendResponse(response);
                if (!result) {
                    co_await conn.close();
                    co_return;
                }
                if (result.value()) {
                    break;  // 发送完成
                }
            }
        }

        co_await conn.close();
        co_return;
    }

    /**
     * @brief 处理请求
     */
    Coroutine processRequest(RpcRequest& request, RpcResponse& response) {
        // 查找服务
        auto service_it = m_services.find(request.serviceName());
        if (service_it == m_services.end()) {
            response.errorCode(RpcErrorCode::SERVICE_NOT_FOUND);
            co_return;
        }

        auto& service = service_it->second;

        // 查找方法
        auto* handler = service->findMethod(request.methodName());
        if (!handler) {
            response.errorCode(RpcErrorCode::METHOD_NOT_FOUND);
            co_return;
        }

        // 调用方法
        RpcContext ctx(request, response);
        co_await (*handler)(ctx).wait();
        co_return;
    }

private:
    RpcServerConfig m_config;
    Runtime m_runtime;
    std::unordered_map<std::string, std::shared_ptr<RpcService>> m_services;
    std::atomic<bool> m_running{false};
};

} // namespace galay::rpc

#endif // GALAY_RPC_SERVER_H
