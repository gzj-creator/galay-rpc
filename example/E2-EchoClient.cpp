/**
 * @file E2-EchoClient.cpp
 * @brief Echo RPC客户端示例
 *
 * @details 演示如何创建一个简单的RPC客户端
 *
 * 使用方法:
 *   ./E2-EchoClient [host] [port]
 *
 * 示例:
 *   ./E2-EchoClient 127.0.0.1 9000
 */

#include "galay-rpc/kernel/RpcClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <thread>

using namespace galay::rpc;
using namespace galay::kernel;

Coroutine runClient(Runtime& runtime, const std::string& host, uint16_t port) {
    std::cout << "Connecting to " << host << ":" << port << "...\n";

    RpcClient client;

    // 连接服务器
    auto connect_result = co_await client.connect(host, port);

    if (!connect_result) {
        std::cerr << "Failed to connect\n";
        co_return;
    }

    std::cout << "Connected!\n\n";

    // 测试 echo 方法
    {
        std::cout << "=== Test: echo ===\n";
        std::string payload = "Hello, RPC World!";
        std::cout << "Input: " << payload << "\n";

        while (true) {
            auto result = co_await client.call("EchoService", "echo", payload);
            if (!result) {
                std::cerr << "Error: " << result.error().message() << "\n";
                break;
            }
            if (result.value()) {
                auto& response = result.value().value();
                if (response.isOk()) {
                    std::string output(response.payload().begin(), response.payload().end());
                    std::cout << "Output: " << output << "\n";
                } else {
                    std::cerr << "Error: " << rpcErrorCodeToString(response.errorCode()) << "\n";
                }
                break;
            }
        }
        std::cout << "\n";
    }

    // 测试 reverse 方法
    {
        std::cout << "=== Test: reverse ===\n";
        std::string payload = "Hello, RPC!";
        std::cout << "Input: " << payload << "\n";

        while (true) {
            auto result = co_await client.call("EchoService", "reverse", payload);
            if (!result) {
                std::cerr << "Error: " << result.error().message() << "\n";
                break;
            }
            if (result.value()) {
                auto& response = result.value().value();
                if (response.isOk()) {
                    std::string output(response.payload().begin(), response.payload().end());
                    std::cout << "Output: " << output << "\n";
                }
                break;
            }
        }
        std::cout << "\n";
    }

    // 测试 length 方法
    {
        std::cout << "=== Test: length ===\n";
        std::string payload = "Test string for length";
        std::cout << "Input: \"" << payload << "\" (expected length: " << payload.size() << ")\n";

        while (true) {
            auto result = co_await client.call("EchoService", "length", payload);
            if (!result) {
                std::cerr << "Error: " << result.error().message() << "\n";
                break;
            }
            if (result.value()) {
                auto& response = result.value().value();
                if (response.isOk() && response.payload().size() >= 4) {
                    uint32_t len;
                    std::memcpy(&len, response.payload().data(), sizeof(len));
                    std::cout << "Output: " << len << "\n";
                }
                break;
            }
        }
        std::cout << "\n";
    }

    // 测试不存在的服务
    {
        std::cout << "=== Test: non-existent service ===\n";

        while (true) {
            auto result = co_await client.call("NonExistentService", "method");
            if (!result) {
                std::cerr << "Error: " << result.error().message() << "\n";
                break;
            }
            if (result.value()) {
                auto& response = result.value().value();
                std::cout << "Error code: " << rpcErrorCodeToString(response.errorCode()) << "\n";
                break;
            }
        }
        std::cout << "\n";
    }

    co_await client.close();
    std::cout << "Client closed.\n";
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    std::cout << "=== Echo RPC Client Example ===\n\n";

    Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 1);
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(runClient(runtime, host, port));

    // 等待完成
    std::this_thread::sleep_for(std::chrono::seconds(3));

    runtime.stop();

    return 0;
}
