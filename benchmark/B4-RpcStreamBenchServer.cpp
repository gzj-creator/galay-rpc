/**
 * @file B4-RpcStreamBenchServer.cpp
 * @brief 真实流式 RPC 压测服务端
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

class StreamBenchService : public RpcService {
public:
    StreamBenchService()
        : RpcService("StreamBenchService") {
        registerStreamMethod("echo", &StreamBenchService::echo);
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
                    "frames=" + std::to_string(frames) +
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

    auto server = RpcStreamServerBuilder()
        .host("0.0.0.0")
        .port(port)
        .ioSchedulerCount(io_count)
        .ringBufferSize(ring_buffer_size)
        .backlog(1024)
        .build();
    server.registerService(std::make_shared<StreamBenchService>());
    server.start();

    std::cout << "=== RPC Stream Benchmark Server ===\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Schedulers: "
              << (io_count == 0 ? "auto" : std::to_string(io_count))
              << "\n";
    std::cout << "RingBuffer size: " << ring_buffer_size << " bytes\n";
    std::cout << "Stream benchmark server started. Press Ctrl+C to stop.\n";

    while (g_running.load(std::memory_order_acquire) && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::cout << "Stream benchmark server stopped.\n";
    return 0;
}
