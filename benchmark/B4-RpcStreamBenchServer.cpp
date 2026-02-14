/**
 * @file B4-RpcStreamBenchServer.cpp
 * @brief 真实流式 RPC 压测服务端
 */

#include "galay-rpc/kernel/RpcStream.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unordered_map>

using namespace galay::rpc;
using namespace galay::kernel;
using namespace galay::async;

namespace {
constexpr uint16_t kDefaultPort = 9100;
constexpr size_t kDefaultRingBufferSize = 128 * 1024;
constexpr int kDefaultBacklog = 1024;

struct StreamSession {
    uint64_t frames = 0;
    uint64_t bytes = 0;
};

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

Coroutine handleStreamConnection(GHandle handle, size_t ring_buffer_size) {
    TcpSocket socket(handle);
    if (!socket.option().handleNonBlock().has_value()) {
        co_return;
    }

    RingBuffer ring_buffer(ring_buffer_size);
    StreamReader reader(ring_buffer, socket);
    std::unordered_map<uint32_t, StreamSession> sessions;

    while (g_running.load(std::memory_order_acquire)) {
        StreamMessage message;
        auto recv_awaitable = reader.getMessage(message);
        while (true) {
            auto recv_result = co_await recv_awaitable;
            if (!recv_result.has_value()) {
                co_await socket.close();
                co_return;
            }
            if (recv_result.value()) {
                break;
            }
        }

        const RpcMessageType type = message.messageType();
        const uint32_t stream_id = message.streamId();
        StreamWriter writer(socket, stream_id);

        if (type == RpcMessageType::STREAM_INIT) {
            StreamInitRequest init_req;
            if (!init_req.deserializeBody(message.payload().data(), message.payload().size())) {
                auto cancel_awaitable = writer.sendCancel();
                while (true) {
                    auto send_result = co_await cancel_awaitable;
                    if (!send_result.has_value() || send_result.value()) {
                        break;
                    }
                }
                continue;
            }

            sessions[stream_id] = StreamSession{};

            auto ack_awaitable = writer.sendInitAck();
            while (true) {
                auto send_result = co_await ack_awaitable;
                if (!send_result.has_value()) {
                    co_await socket.close();
                    co_return;
                }
                if (send_result.value()) {
                    break;
                }
            }
            continue;
        }

        if (type == RpcMessageType::STREAM_DATA) {
            auto session_it = sessions.find(stream_id);
            if (session_it == sessions.end()) {
                auto cancel_awaitable = writer.sendCancel();
                while (true) {
                    auto send_result = co_await cancel_awaitable;
                    if (!send_result.has_value() || send_result.value()) {
                        break;
                    }
                }
                continue;
            }

            session_it->second.frames += 1;
            session_it->second.bytes += message.payload().size();

            auto echo_awaitable = writer.sendData(message.payload().data(), message.payload().size());
            while (true) {
                auto send_result = co_await echo_awaitable;
                if (!send_result.has_value()) {
                    co_await socket.close();
                    co_return;
                }
                if (send_result.value()) {
                    break;
                }
            }
            continue;
        }

        if (type == RpcMessageType::STREAM_END) {
            auto session_it = sessions.find(stream_id);
            if (session_it != sessions.end()) {
                const auto& session = session_it->second;
                const std::string summary =
                    "frames=" + std::to_string(session.frames) +
                    ", bytes=" + std::to_string(session.bytes);

                auto summary_awaitable = writer.sendData(summary.data(), summary.size());
                while (true) {
                    auto send_result = co_await summary_awaitable;
                    if (!send_result.has_value()) {
                        co_await socket.close();
                        co_return;
                    }
                    if (send_result.value()) {
                        break;
                    }
                }

                sessions.erase(session_it);
            }

            auto end_awaitable = writer.sendEnd();
            while (true) {
                auto send_result = co_await end_awaitable;
                if (!send_result.has_value()) {
                    co_await socket.close();
                    co_return;
                }
                if (send_result.value()) {
                    break;
                }
            }
            continue;
        }

        if (type == RpcMessageType::STREAM_CANCEL) {
            sessions.erase(stream_id);
            auto cancel_awaitable = writer.sendCancel();
            while (true) {
                auto send_result = co_await cancel_awaitable;
                if (!send_result.has_value()) {
                    co_await socket.close();
                    co_return;
                }
                if (send_result.value()) {
                    break;
                }
            }
        }
    }

    co_await socket.close();
    co_return;
}

Coroutine acceptLoop(Runtime& runtime, const std::string& host, uint16_t port, size_t ring_buffer_size) {
    TcpSocket listener(IPType::IPV4);

    auto reuse_addr_result = listener.option().handleReuseAddr();
    if (!reuse_addr_result.has_value()) {
        co_return;
    }

    auto non_block_result = listener.option().handleNonBlock();
    if (!non_block_result.has_value()) {
        co_return;
    }

    Host listen_host(IPType::IPV4, host, port);
    if (!listener.bind(listen_host).has_value()) {
        co_return;
    }

    if (!listener.listen(kDefaultBacklog).has_value()) {
        co_return;
    }

    while (g_running.load(std::memory_order_acquire)) {
        Host client_host;
        auto accept_result = co_await listener.accept(&client_host);
        if (!accept_result.has_value()) {
            continue;
        }

        runtime.getNextIOScheduler()->spawn(handleStreamConnection(accept_result.value(), ring_buffer_size));
    }

    co_await listener.close();
    co_return;
}
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

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

    std::cout << "=== RPC Stream Benchmark Server ===\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Schedulers: " << (io_count == 0 ? "auto" : std::to_string(io_count)) << "\n";
    std::cout << "RingBuffer size: " << ring_buffer_size << " bytes\n";

    Runtime runtime(io_count, 1);
    runtime.start();

    runtime.getNextIOScheduler()->spawn(acceptLoop(runtime, "0.0.0.0", port, ring_buffer_size));

    std::cout << "Stream benchmark server started. Press Ctrl+C to stop.\n";
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    runtime.stop();
    std::cout << "Stream benchmark server stopped.\n";
    return 0;
}
