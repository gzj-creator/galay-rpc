/**
 * @file E3-StreamServer.cpp
 * @brief 真实流式 RPC 服务端示例
 *
 * @details STREAM_INIT 到达后自动按 service/method 路由，
 *          回调入参为 RpcStream，业务层只处理流读写。
 *
 * Usage:
 *   ./E3-StreamServer [port] [io_count] [ring_buffer_size]
 */

#include "galay-rpc/kernel/RpcService.h"
#include "galay-rpc/kernel/RpcStreamServer.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {
constexpr uint16_t kDefaultPort = 9100;
constexpr size_t kDefaultRingBufferSize = 128 * 1024;

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

class StreamExampleService : public RpcService {
public:
    StreamExampleService()
        : RpcService("StreamExampleService") {
        registerStreamMethod("echo", &StreamExampleService::echo);
    }

    Coroutine echo(RpcStream& stream) {
        uint64_t frames = 0;
        uint64_t bytes = 0;

        while (true) {
            StreamMessage msg;
            auto recv_result = co_await stream.read(msg);
            if (!recv_result.has_value()) {
                co_return;
            }

            if (msg.messageType() == RpcMessageType::STREAM_DATA) {
                frames += 1;
                bytes += msg.payload().size();

                auto send_result = co_await stream.sendData(msg.payload().data(), msg.payload().size());
                if (!send_result.has_value()) {
                    co_return;
                }
                continue;
            }

            if (msg.messageType() == RpcMessageType::STREAM_END) {
                const std::string summary =
                    "service=" + stream.serviceName() +
                    ", method=" + stream.methodName() +
                    ", frames=" + std::to_string(frames) +
                    ", bytes=" + std::to_string(bytes);

                auto send_result = co_await stream.sendData(summary.data(), summary.size());
                if (!send_result.has_value()) {
                    co_return;
                }

                send_result = co_await stream.sendEnd();
                if (!send_result.has_value()) {
                    co_return;
                }

                co_return;
            }

            if (msg.messageType() == RpcMessageType::STREAM_CANCEL) {
                co_return;
            }
            (void)co_await stream.sendCancel();
            co_return;
        }
    }
};
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    uint16_t port = kDefaultPort;
    size_t io_count = 0;
    size_t ring_buffer_size = kDefaultRingBufferSize;

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        io_count = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    }
    if (argc > 3) {
        ring_buffer_size = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }

    RpcStreamServerConfig config;
    config.host = "0.0.0.0";
    config.port = port;
    config.io_scheduler_count = io_count;
    config.ring_buffer_size = ring_buffer_size;

    RpcStreamServer server(config);
    server.registerService(std::make_shared<StreamExampleService>());
    server.start();

    std::cout << "=== Stream RPC Server Example ===\n";
    std::cout << "listen: " << config.host << ":" << config.port << "\n";
    std::cout << "io_schedulers: " << (config.io_scheduler_count == 0 ? "auto" : std::to_string(config.io_scheduler_count)) << "\n";
    std::cout << "ring_buffer: " << config.ring_buffer_size << " bytes\n";

    while (g_running.load(std::memory_order_acquire) && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "Stream server stopped.\n";
    return 0;
}
