/**
 * @file B2-RpcBenchClient.cpp
 * @brief RPC压测客户端（含P99延迟统计）
 */

#include "galay-rpc/kernel/RpcClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>
#include <csignal>
#include <iomanip>
#include <algorithm>
#include <mutex>

using namespace galay::rpc;
using namespace galay::kernel;

struct BenchConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    size_t connections = 100;
    size_t payload_size = 256;
    size_t duration_sec = 10;
    size_t io_schedulers = 2;
};

std::atomic<uint64_t> g_total_requests{0};
std::atomic<uint64_t> g_total_errors{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<bool> g_running{true};

// 延迟统计
std::mutex g_latency_mutex;
std::vector<uint64_t> g_latencies;  // 微秒

void signalHandler(int) {
    g_running.store(false);
}

Coroutine benchWorker(Runtime& runtime, const BenchConfig& config, size_t worker_id) {
    (void)runtime;
    (void)worker_id;

    RpcClientConfig client_config;
    // echo场景下响应体与请求体同量级，按payload放大客户端缓冲，避免大包响应解析失败。
    client_config.ring_buffer_size = std::max<size_t>(8192, config.payload_size + RPC_HEADER_SIZE + 256);

    RpcClient client(client_config);

    const int max_retries = 15;
    const auto retry_interval = std::chrono::seconds(1);

    bool connected = false;
    for (int retry_count = 0; retry_count < max_retries && g_running.load(std::memory_order_relaxed); ++retry_count) {
        auto connect_result = co_await client.connect(config.host, config.port);
        if (connect_result) {
            connected = true;
            break;
        }

        g_total_errors.fetch_add(1, std::memory_order_relaxed);
        (void)co_await client.close();

        if (retry_count + 1 < max_retries && g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(retry_interval);
        }
    }

    if (!connected) {
        co_return;
    }

    std::string payload(config.payload_size, 'X');
    std::vector<uint64_t> local_latencies;
    local_latencies.reserve(10000);
    bool stop_worker = false;

    while (g_running.load(std::memory_order_relaxed) && !stop_worker) {
        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto result = co_await client.call("BenchEchoService", "echo", payload);
            if (!result) {
                g_total_errors.fetch_add(1, std::memory_order_relaxed);

                (void)co_await client.close();

                bool reconnected = false;
                for (int retry_count = 0; retry_count < max_retries && g_running.load(std::memory_order_relaxed); ++retry_count) {
                    auto reconnect_result = co_await client.connect(config.host, config.port);
                    if (reconnect_result) {
                        reconnected = true;
                        break;
                    }

                    g_total_errors.fetch_add(1, std::memory_order_relaxed);
                    (void)co_await client.close();

                    if (retry_count + 1 < max_retries && g_running.load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(retry_interval);
                    }
                }

                if (!reconnected) {
                    stop_worker = true;
                }
                break;
            }

            if (result.value()) {
                auto& response = result.value().value();
                if (response.isOk()) {
                    auto end = std::chrono::steady_clock::now();
                    auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                    local_latencies.push_back(latency_us);

                    g_total_requests.fetch_add(1, std::memory_order_relaxed);
                    g_total_bytes.fetch_add(payload.size() * 2, std::memory_order_relaxed);
                } else {
                    g_total_errors.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        }
    }

    // 合并本地延迟数据
    if (!local_latencies.empty()) {
        std::lock_guard<std::mutex> lock(g_latency_mutex);
        g_latencies.insert(g_latencies.end(), local_latencies.begin(), local_latencies.end());
    }

    co_await client.close();
    co_return;
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -h <host>        Server host (default: 127.0.0.1)\n"
              << "  -p <port>        Server port (default: 9000)\n"
              << "  -c <connections> Number of connections (default: 100)\n"
              << "  -s <size>        Payload size in bytes (default: 256)\n"
              << "  -d <duration>    Test duration in seconds (default: 10)\n"
              << "  -i <io_count>    IO scheduler count (default: 2)\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    BenchConfig config;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) break;
        std::string opt = argv[i];
        std::string val = argv[i + 1];

        if (opt == "-h") config.host = val;
        else if (opt == "-p") config.port = static_cast<uint16_t>(std::stoi(val));
        else if (opt == "-c") config.connections = std::stoul(val);
        else if (opt == "-s") config.payload_size = std::stoul(val);
        else if (opt == "-d") config.duration_sec = std::stoul(val);
        else if (opt == "-i") config.io_schedulers = std::stoul(val);
        else if (opt == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "=== RPC Benchmark Client ===\n";
    std::cout << "Target: " << config.host << ":" << config.port << "\n";
    std::cout << "Connections: " << config.connections << "\n";
    std::cout << "Payload size: " << config.payload_size << " bytes\n";
    std::cout << "Duration: " << config.duration_sec << " seconds\n";
    std::cout << "IO Schedulers: " << config.io_schedulers << "\n";
    std::cout << "\n";

    Runtime runtime(config.io_schedulers, 1);
    runtime.start();

    // 启动所有连接
    std::cout << "Starting " << config.connections << " connections...\n";
    for (size_t i = 0; i < config.connections; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        scheduler->spawn(benchWorker(runtime, config, i));
    }

    std::cout << "Benchmark running...\n\n";

    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_requests = 0;
    uint64_t last_bytes = 0;

    for (size_t sec = 0; sec < config.duration_sec && g_running.load(); ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t current_requests = g_total_requests.load();
        uint64_t current_bytes = g_total_bytes.load();
        uint64_t current_errors = g_total_errors.load();

        uint64_t qps = current_requests - last_requests;
        double throughput_mb = static_cast<double>(current_bytes - last_bytes) / (1024.0 * 1024.0);

        std::cout << "[" << std::setw(3) << (sec + 1) << "s] "
                  << "QPS: " << std::setw(8) << qps
                  << " | Throughput: " << std::fixed << std::setprecision(2) << throughput_mb << " MB/s"
                  << " | Errors: " << current_errors
                  << "\n";

        last_requests = current_requests;
        last_bytes = current_bytes;
    }

    g_running.store(false);
    auto end_time = std::chrono::steady_clock::now();

    // 等待连接关闭
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    runtime.stop();

    // 计算统计
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double duration_sec = duration.count() / 1000.0;

    uint64_t total_requests = g_total_requests.load();
    uint64_t total_errors = g_total_errors.load();
    uint64_t total_bytes = g_total_bytes.load();

    double avg_qps = total_requests / duration_sec;
    double avg_throughput = (total_bytes / (1024.0 * 1024.0)) / duration_sec;
    double error_rate = total_requests > 0 ? (100.0 * total_errors / (total_requests + total_errors)) : 0;

    std::cout << "\n=== Benchmark Results ===\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << duration_sec << " seconds\n";
    std::cout << "Total Requests: " << total_requests << "\n";
    std::cout << "Total Errors: " << total_errors << "\n";
    std::cout << "Average QPS: " << std::fixed << std::setprecision(0) << avg_qps << "\n";
    std::cout << "Average Throughput: " << std::fixed << std::setprecision(2) << avg_throughput << " MB/s\n";
    std::cout << "Error Rate: " << std::fixed << std::setprecision(2) << error_rate << "%\n";

    // 计算延迟百分位数
    if (!g_latencies.empty()) {
        std::sort(g_latencies.begin(), g_latencies.end());
        size_t n = g_latencies.size();

        auto p50 = g_latencies[n * 50 / 100];
        auto p90 = g_latencies[n * 90 / 100];
        auto p95 = g_latencies[n * 95 / 100];
        auto p99 = g_latencies[n * 99 / 100];
        auto p999 = g_latencies[std::min(n * 999 / 1000, n - 1)];
        auto max_lat = g_latencies[n - 1];

        uint64_t sum = 0;
        for (auto lat : g_latencies) sum += lat;
        double avg_lat = static_cast<double>(sum) / n;

        std::cout << "\n=== Latency (microseconds) ===\n";
        std::cout << "Samples: " << n << "\n";
        std::cout << "Avg:  " << std::fixed << std::setprecision(0) << avg_lat << " us\n";
        std::cout << "P50:  " << p50 << " us\n";
        std::cout << "P90:  " << p90 << " us\n";
        std::cout << "P95:  " << p95 << " us\n";
        std::cout << "P99:  " << p99 << " us\n";
        std::cout << "P99.9: " << p999 << " us\n";
        std::cout << "Max:  " << max_lat << " us\n";
    }

    return 0;
}
