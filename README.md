# Galay-RPC

基于 C++20/23 协程的高性能异步 RPC 框架，构建于 [galay-kernel](https://github.com/galay) 异步运行时之上。

## 性能数据

> 测试环境: Apple M4, 24GB RAM, macOS 15.7.3, AppleClang, Release 构建

### RPC 压测

| 连接数 | Payload | IO调度器 | QPS | 吞吐量 | P50 | P99 | 错误率 |
|--------|---------|----------|-----|--------|-----|-----|--------|
| 50 | 256B | 2 | **170,629** | 83 MB/s | 223 us | 1,165 us | 0% |
| 100 | 256B | 2 | **183,338** | 90 MB/s | 453 us | 1,824 us | 0% |
| 200 | 256B | 2 | **169,673** | 83 MB/s | 973 us | 3,812 us | 0% |
| 100 | 1KB | 2 | **178,606** | 349 MB/s | 464 us | 1,933 us | 0% |
| 100 | 4KB | 4 | **155,697** | 1,216 MB/s | 392 us | 4,010 us | 0% |

### RPC 四模式 Echo（2026-02-14）

47B（200连接，`-i 0`，`-l 4`，5秒）：

| 模式 | QPS | 吞吐量 | P99 |
|------|-----|--------|-----|
| unary | **345,965** | 31.01 MB/s | 40,811 us |
| client_stream | **277,020** | 24.83 MB/s | 53,104 us |
| server_stream | **293,926** | 26.35 MB/s | 50,927 us |
| bidi | **283,218** | 25.39 MB/s | 71,229 us |

64KB（100连接，`-i 0`，`-l 1`，5秒）：

| 模式 | QPS | 吞吐量 | P99 |
|------|-----|--------|-----|
| unary | **87,055** | 10,881.85 MB/s | 15,522 us |
| client_stream | **85,434** | 10,679.27 MB/s | 14,911 us |
| server_stream | **83,592** | 10,449.03 MB/s | 15,271 us |
| bidi | **84,188** | 10,523.46 MB/s | 14,649 us |

### ServiceDiscovery 压测

| 指标 | 数值 |
|------|------|
| OPS | **5,430,000+** |
| 错误率 | **0%** |
| 测试配置 | 100 workers, 2个IO调度器 |

详细压测报告见 [docs/B1-RPC压测报告.md](docs/B1-RPC压测报告.md) 和 [docs/B3-服务发现压测报告.md](docs/B3-服务发现压测报告.md)。

## 核心特性

### 协程驱动的异步架构
- **完全异步**: 基于 C++20/23 协程的非阻塞 I/O 操作
- **高效IO**: 使用 RingBuffer 配合 readv/writev 实现高效数据传输
- **超时支持**: 所有异步操作均支持超时设置

### RPC 服务器
- **内置 Runtime**: 服务器自带运行时管理，无需手动配置调度器
- **服务注册**: 支持多服务、多方法注册
- **请求分发**: 自动解析请求并分发到对应服务方法

### RPC 客户端
- **简洁API**: 协程风格的 RPC 调用
- **连接复用**: 支持长连接复用
- **四种调用模式**: 支持 unary / client_stream / server_stream / bidi
- **超时控制**: 支持请求级别的超时设置

### 服务发现（可扩展）
- **Concept约束**: 使用 C++20 concept 定义服务发现接口
- **本地注册**: 内置 LocalServiceRegistry 用于单机部署
- **可扩展**: 支持 etcd、consul 等注册中心（实现 ServiceRegistry concept 即可）
- **负载均衡**: 使用 galay-kernel 内置的轮询、随机、加权轮询、加权随机选择器

### 协议设计
- **二进制协议**: 高效的二进制消息格式
- **消息头**: 16字节固定头部，包含魔数、版本、类型、请求ID、消息长度
- **错误码**: 完善的错误码定义

## 快速开始

### 构建要求

- CMake 3.16+
- C++23 兼容编译器 (GCC 11+, Clang 14+, AppleClang 15+)
- galay-kernel 库
- spdlog 库

### 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
```

## 使用示例

### 定义服务

```cpp
#include "galay-rpc/kernel/RpcService.h"

using namespace galay::rpc;
using namespace galay::kernel;

class EchoService : public RpcService {
public:
    EchoService() : RpcService("EchoService") {
        registerMethod("echo", &EchoService::echo);
        registerMethod("uppercase", &EchoService::uppercase);
    }

    Coroutine echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.setPayload(req.payload().data(), req.payload().size());
        co_return;
    }

    Coroutine uppercase(RpcContext& ctx) {
        auto& payload = ctx.request().payload();
        std::string data(payload.begin(), payload.end());
        for (auto& c : data) {
            c = std::toupper(c);
        }
        ctx.setPayload(data);
        co_return;
    }
};
```

### RPC 服务器

```cpp
#include "galay-rpc/kernel/RpcServer.h"

int main() {
    // 创建服务
    auto echoService = std::make_shared<EchoService>();

    // 配置服务器
    RpcServerConfig config;
    config.host = "0.0.0.0";
    config.port = 9000;

    // 启动服务器
    RpcServer server(config);
    server.registerService(echoService);
    server.start();

    // 保持运行
    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
```

### RPC 客户端

```cpp
#include "galay-rpc/kernel/RpcClient.h"
#include "galay-kernel/kernel/Runtime.h"

Coroutine callEcho(Runtime& runtime) {
    RpcClient client;

    // 连接服务器
    auto connect_result = co_await client.connect("127.0.0.1", 9000);
    if (!connect_result) {
        co_return;
    }

    // 一元调用
    auto unary_result = co_await client.call("EchoService", "echo", "Hello");
    if (unary_result && unary_result.value() && unary_result.value()->isOk()) {
        std::string data(unary_result.value()->payload().begin(),
                         unary_result.value()->payload().end());
        std::cout << "Unary response: " << data << "\n";
    }

    // 双向流模式（当前示例为单帧）
    auto bidi_result = co_await client.callBidiStreamFrame("EchoService", "echo", "Hello", 5, true);
    if (bidi_result && bidi_result.value() && bidi_result.value()->isOk()) {
        std::string data(bidi_result.value()->payload().begin(),
                         bidi_result.value()->payload().end());
        std::cout << "Bidi response: " << data << "\n";
    }

    co_await client.close();
}

int main() {
    Runtime runtime(1, 1);
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(callEcho(runtime));

    std::this_thread::sleep_for(std::chrono::seconds(3));
    runtime.stop();

    return 0;
}
```

### 服务发现

```cpp
#include "galay-rpc/kernel/ServiceDiscovery.h"

using namespace galay::rpc;

// 使用本地注册中心
LocalServiceRegistry registry;

// 注册服务
ServiceEndpoint endpoint;
endpoint.service_name = "EchoService";
endpoint.host = "127.0.0.1";
endpoint.port = 9000;
endpoint.instance_id = "instance-1";
endpoint.weight = 100;

registry.registerService(endpoint);

// 创建服务发现客户端（使用轮询负载均衡）
ServiceDiscoveryClient<LocalServiceRegistry, RoundRobinSelector> client(registry);

// 获取服务实例
auto result = client.getServiceEndpoint("EchoService");
if (result) {
    auto& ep = result.value();
    std::cout << "Selected: " << ep.address() << "\n";
}

// 监听服务变更
client.watch("EchoService", [](const ServiceEvent& event) {
    if (event.type == ServiceEventType::ADDED) {
        std::cout << "Service added: " << event.endpoint.address() << "\n";
    }
});
```

### 自定义注册中心（如 etcd）

```cpp
// 实现 ServiceRegistry concept
class EtcdServiceRegistry {
public:
    std::expected<void, DiscoveryError> registerService(const ServiceEndpoint& endpoint);
    std::expected<void, DiscoveryError> deregisterService(const ServiceEndpoint& endpoint);
    std::expected<std::vector<ServiceEndpoint>, DiscoveryError> discoverService(const std::string& service_name);
    std::expected<void, DiscoveryError> watchService(const std::string& service_name, ServiceWatchCallback callback);
    void unwatchService(const std::string& service_name);
};

// 编译时验证
static_assert(ServiceRegistry<EtcdServiceRegistry>, "Must satisfy ServiceRegistry concept");

// 使用
EtcdServiceRegistry etcdRegistry("http://localhost:2379");
ServiceDiscoveryClient<EtcdServiceRegistry> client(etcdRegistry);
```

## 项目结构

```
galay-rpc/
├── galay-rpc/              # 核心库源码
│   ├── protoc/             # 协议层
│   │   ├── RpcBase.h       # 基础定义
│   │   ├── RpcMessage.h    # 消息定义
│   │   ├── RpcCodec.h      # 编解码器
│   │   └── RpcError.h      # 错误处理
│   └── kernel/             # 内核层
│       ├── RpcService.h    # 服务定义
│       ├── RpcConn.h       # 连接封装（RingBuffer + readv/writev）
│       ├── RpcServer.h     # 服务器
│       ├── RpcClient.h     # 客户端
│       └── ServiceDiscovery.h  # 服务发现
├── test/                   # 测试代码
├── benchmark/              # 压测工具
├── example/                # 示例代码
├── scripts/                # 脚本
├── docs/                   # 文档
└── CMakeLists.txt          # 构建配置
```

## 协议格式

### 消息头 (16 bytes)

```
+--------+--------+--------+--------+--------+--------+--------+--------+
|              Magic (4 bytes)              |  Ver   |  Type  | Flags  |
+--------+--------+--------+--------+--------+--------+--------+--------+
|           Request ID (4 bytes)            |       Body Length (4 bytes)
+--------+--------+--------+--------+--------+--------+--------+--------+
```

### 请求体

```
+--------+--------+--------+--------+--------+--------+--------+--------+
|  Service Name Len (2)   |         Service Name (variable)            |
+--------+--------+--------+--------+--------+--------+--------+--------+
|  Method Name Len (2)    |         Method Name (variable)             |
+--------+--------+--------+--------+--------+--------+--------+--------+
|                         Payload (variable)                           |
+--------+--------+--------+--------+--------+--------+--------+--------+
```

### 响应体

```
+--------+--------+--------+--------+--------+--------+--------+--------+
|    Error Code (2)       |         Payload (variable)                 |
+--------+--------+--------+--------+--------+--------+--------+--------+
```

## API 参考

### RpcServer

```cpp
class RpcServer {
    explicit RpcServer(const RpcServerConfig& config);
    void registerService(std::shared_ptr<RpcService> service);
    void start();
    void stop();
    bool isRunning() const;
};
```

### RpcClient

```cpp
class RpcClient {
    ConnectAwaitable connect(const std::string& host, uint16_t port);
    RpcCallAwaitable call(const std::string& service, const std::string& method);
    RpcCallAwaitable call(const std::string& service, const std::string& method, const std::string& payload);
    RpcCallAwaitable call(const std::string& service, const std::string& method,
                          const char* payload, size_t payload_len);
    RpcCallAwaitable callClientStreamFrame(const std::string& service, const std::string& method,
                                           const char* payload, size_t payload_len, bool end_of_stream);
    RpcCallAwaitable callServerStreamRequest(const std::string& service, const std::string& method,
                                             const char* payload, size_t payload_len);
    RpcCallAwaitable callBidiStreamFrame(const std::string& service, const std::string& method,
                                         const char* payload, size_t payload_len, bool end_of_stream);
    CloseAwaitable close();
};
```

### RpcService

```cpp
class RpcService {
    explicit RpcService(std::string_view name);
    void registerMethod(std::string_view name, RpcMethodHandler handler);  // unary
    void registerClientStreamingMethod(std::string_view name, RpcMethodHandler handler);
    void registerServerStreamingMethod(std::string_view name, RpcMethodHandler handler);
    void registerBidiStreamingMethod(std::string_view name, RpcMethodHandler handler);
    template<typename T>
    void registerMethod(std::string_view name, Coroutine (T::*method)(RpcContext&));
};
```

### ServiceRegistry Concept

```cpp
template<typename T>
concept ServiceRegistry = requires(T registry, ...) {
    { registry.registerService(endpoint) } -> std::same_as<std::expected<void, DiscoveryError>>;
    { registry.deregisterService(endpoint) } -> std::same_as<std::expected<void, DiscoveryError>>;
    { registry.discoverService(service_name) } -> std::same_as<std::expected<std::vector<ServiceEndpoint>, DiscoveryError>>;
    { registry.watchService(service_name, callback) } -> std::same_as<std::expected<void, DiscoveryError>>;
    { registry.unwatchService(service_name) } -> std::same_as<void>;
};
```

### ServiceSelector Concept

已移除，现在直接使用 galay-kernel 的负载均衡器：

```cpp
// 可用的选择器类型
using RoundRobinSelector = details::RoundRobinLoadBalancer<ServiceEndpoint>;
using RandomSelector = details::RandomLoadBalancer<ServiceEndpoint>;
using WeightedRoundRobinSelector = details::WeightRoundRobinLoadBalancer<ServiceEndpoint>;
using WeightedRandomSelector = details::WeightedRandomLoadBalancer<ServiceEndpoint>;
```

## 错误码

| 错误码 | 说明 |
|-------|------|
| OK | 成功 |
| SERVICE_NOT_FOUND | 服务未找到 |
| METHOD_NOT_FOUND | 方法未找到 |
| INVALID_REQUEST | 无效请求 |
| INVALID_RESPONSE | 无效响应 |
| TIMEOUT | 超时 |
| CONNECTION_CLOSED | 连接关闭 |
| SERIALIZATION_ERROR | 序列化错误 |
| DESERIALIZATION_ERROR | 反序列化错误 |
| INTERNAL_ERROR | 内部错误 |

## 压测命令

```bash
# 启动服务端
./benchmark/B1-RpcBenchServer 9000
# 参数: [port] [io_scheduler_count] [ring_buffer_size]

# 启动客户端压测
./benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 100 -d 10 -s 256 -i 4
# 新增: -l 指定单连接 pipeline 深度（默认 1），-m 指定 RPC 模式

# 参数说明:
#   -h: 服务器地址
#   -p: 服务器端口
#   -c: 并发连接数
#   -d: 测试持续时间（秒）
#   -s: payload大小（字节）
#   -i: IO调度器数量
#   -l: 单连接 pipeline 深度
#   -m: RPC 模式（unary|client_stream|server_stream|bidi）

# 四模式 Echo（47B）示例
./benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m unary
./benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m client_stream
./benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m server_stream
./benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m bidi
```

## 许可证

MIT License
