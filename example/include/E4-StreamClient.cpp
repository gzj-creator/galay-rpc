/**
 * @file E4-StreamClient.cpp
 * @brief 真实流式 RPC 客户端示例
 *
 * @details 模拟完整流生命周期：INIT -> 多帧 DATA 双向收发 -> END。
 *
 * Usage:
 *   ./E4-StreamClient [host] [port] [frame_count] [payload_size]
 */

#include "galay-rpc/kernel/RpcClient.h"
#include "galay-rpc/kernel/RpcStream.h"
#include "galay-kernel/kernel/Runtime.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {
constexpr uint16_t kDefaultPort = 9100;
constexpr size_t kDefaultFrameCount = 1000;
constexpr size_t kDefaultPayloadSize = 128;

Coroutine runStreamClient(const std::string& host,
                          uint16_t port,
                          size_t frame_count,
                          size_t payload_size) {
    RpcClientConfig config;
    config.ring_buffer_size = std::max<size_t>(kDefaultRpcRingBufferSize,
                                                payload_size * 4 + RPC_HEADER_SIZE * 8);
    RpcClient client(config);

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result.has_value()) {
        std::cerr << "connect failed: " << connect_result.error().message() << "\n";
        co_return;
    }

    const uint32_t stream_id = 1;
    RpcStreamConn stream(client.socket(), client.ringBuffer(), stream_id);
    auto& writer = stream.getWriter();
    auto& reader = stream.getReader();

    auto init_awaitable = writer.sendInit("StreamExampleService", "echo");
    while (true) {
        auto send_result = co_await init_awaitable;
        if (!send_result.has_value()) {
            std::cerr << "send init failed: " << send_result.error().message() << "\n";
            co_await client.close();
            co_return;
        }
        if (send_result.value()) {
            break;
        }
    }

    StreamMessage init_ack;
    auto init_ack_awaitable = reader.getMessage(init_ack);
    while (true) {
        auto recv_result = co_await init_ack_awaitable;
        if (!recv_result.has_value()) {
            std::cerr << "recv init ack failed: " << recv_result.error().message() << "\n";
            co_await client.close();
            co_return;
        }
        if (recv_result.value()) {
            break;
        }
    }

    if (init_ack.messageType() != RpcMessageType::STREAM_INIT_ACK) {
        std::cerr << "unexpected init response type: " << static_cast<int>(init_ack.messageType()) << "\n";
        co_await client.close();
        co_return;
    }

    std::string payload(payload_size, 'x');
    uint64_t total_echo_bytes = 0;

    const auto start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < frame_count; ++i) {
        if (payload_size >= sizeof(uint64_t)) {
            const uint64_t frame_id = static_cast<uint64_t>(i);
            std::memcpy(payload.data(), &frame_id, sizeof(frame_id));
        }

        auto send_data_awaitable = writer.sendData(payload.data(), payload.size());
        while (true) {
            auto send_result = co_await send_data_awaitable;
            if (!send_result.has_value()) {
                std::cerr << "send frame failed: " << send_result.error().message() << "\n";
                co_await client.close();
                co_return;
            }
            if (send_result.value()) {
                break;
            }
        }

        StreamMessage echo_frame;
        auto recv_data_awaitable = reader.getMessage(echo_frame);
        while (true) {
            auto recv_result = co_await recv_data_awaitable;
            if (!recv_result.has_value()) {
                std::cerr << "recv echo frame failed: " << recv_result.error().message() << "\n";
                co_await client.close();
                co_return;
            }
            if (recv_result.value()) {
                break;
            }
        }

        if (echo_frame.messageType() != RpcMessageType::STREAM_DATA) {
            std::cerr << "unexpected frame type while streaming: "
                      << static_cast<int>(echo_frame.messageType()) << "\n";
            co_await client.close();
            co_return;
        }

        total_echo_bytes += echo_frame.payload().size();
    }

    auto send_end_awaitable = writer.sendEnd();
    while (true) {
        auto send_result = co_await send_end_awaitable;
        if (!send_result.has_value()) {
            std::cerr << "send end failed: " << send_result.error().message() << "\n";
            co_await client.close();
            co_return;
        }
        if (send_result.value()) {
            break;
        }
    }

    std::string summary;
    bool got_end = false;
    while (!got_end) {
        StreamMessage msg;
        auto recv_awaitable = reader.getMessage(msg);
        while (true) {
            auto recv_result = co_await recv_awaitable;
            if (!recv_result.has_value()) {
                std::cerr << "recv tail frame failed: " << recv_result.error().message() << "\n";
                co_await client.close();
                co_return;
            }
            if (recv_result.value()) {
                break;
            }
        }

        if (msg.messageType() == RpcMessageType::STREAM_DATA) {
            summary = msg.payloadStr();
        } else if (msg.messageType() == RpcMessageType::STREAM_END) {
            got_end = true;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0;
    const double frame_rate = elapsed_sec > 0 ? static_cast<double>(frame_count) / elapsed_sec : 0.0;

    std::cout << "stream_id=" << stream_id
              << ", frames=" << frame_count
              << ", payload=" << payload_size
              << " bytes\n";
    std::cout << "elapsed=" << elapsed_sec << " s"
              << ", frame_rate=" << frame_rate << " frames/s"
              << ", echoed_bytes=" << total_echo_bytes << "\n";
    std::cout << "server summary: " << summary << "\n";

    co_await client.close();
    co_return;
}
} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    size_t frame_count = kDefaultFrameCount;
    size_t payload_size = kDefaultPayloadSize;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }
    if (argc > 3) {
        frame_count = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }
    if (argc > 4) {
        payload_size = static_cast<size_t>(std::strtoull(argv[4], nullptr, 10));
    }

    std::cout << "=== Stream RPC Client Example ===\n";
    std::cout << "target=" << host << ":" << port
              << ", frames=" << frame_count
              << ", payload=" << payload_size << " bytes\n";

    Runtime runtime(1, 1);
    runtime.start();
    runtime.getNextIOScheduler()->spawn(runStreamClient(host, port, frame_count, payload_size));

    // 示例程序给一个宽松等待窗口
    std::this_thread::sleep_for(std::chrono::seconds(5));
    runtime.stop();
    return 0;
}
