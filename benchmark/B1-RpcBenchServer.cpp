/**
 * @file B1-RpcBenchServer.cpp
 * @brief RPC压测服务端
 */

#include "galay-rpc/kernel/RpcServer.h"
#include "galay-rpc/kernel/RpcService.h"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace galay::rpc;
using namespace galay::kernel;

/**
 * @brief 压测Echo服务
 */
class BenchEchoService : public RpcService {
public:
    BenchEchoService() : RpcService("BenchEchoService") {
        registerMethod("echo", &BenchEchoService::echo);
    }

    Coroutine echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.setPayload(req.payload().data(), req.payload().size());
        co_return;
    }
};

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    uint16_t port = 9000;
    size_t io_count = 0;  // 自动
    size_t ring_buffer_size = 128 * 1024;

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        io_count = static_cast<size_t>(std::atoi(argv[2]));
    }
    if (argc > 3) {
        ring_buffer_size = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }

    std::cout << "=== RPC Benchmark Server ===\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Schedulers: " << (io_count == 0 ? "auto" : std::to_string(io_count)) << "\n";
    std::cout << "RingBuffer size: " << ring_buffer_size << " bytes\n";

    auto service = std::make_shared<BenchEchoService>();

    RpcServerConfig config;
    config.host = "0.0.0.0";
    config.port = port;
    config.io_scheduler_count = io_count;
    config.backlog = 1024;
    config.ring_buffer_size = ring_buffer_size;

    RpcServer server(config);
    server.registerService(service);
    server.start();

    std::cout << "Server started. Press Ctrl+C to stop.\n";

    while (g_running.load() && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "Server stopped.\n";

    return 0;
}
