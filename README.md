# Galay-RPC

基于 C++20/23 协程的高性能异步 RPC 框架，构建于 [galay-kernel](https://github.com/galay) 异步运行时之上。

## 性能数据

| 指标 | 数值 |
|------|------|
| QPS | **125,000+** |
| 吞吐量 | **61 MB/s** |
| 错误率 | **0%** |
| 测试配置 | 50连接, 256字节payload, 4个IO调度器 |

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
- **超时控制**: 支持请求级别的超时设置

### 服务发现（可扩展）
- **Concept约束**: 使用 C++20 concept 定义服务发现接口
- **本地注册**: 内置 LocalServiceRegistry 用于单机部署
- **可扩展**: 支持 etcd、consul 等注册中心（实现 ServiceRegistry concept 即可）
- **负载均衡**: 内置轮询、随机、加权轮询选择器

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

    // 调用远程方法
    RpcResponse response;
    std::expected<void, RpcError> result;
    co_await client.call("EchoService", "echo", "Hello", response, result).wait();

    if (result.has_value() && response.isOk()) {
        std::string data(response.payload().begin(), response.payload().end());
        std::cout << "Response: " << data << "\n";
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
    Coroutine call(const std::string& service, const std::string& method,
                   const char* payload, size_t len,
                   RpcResponse& response, std::expected<void, RpcError>& result);
    CloseAwaitable close();
};
```

### RpcService

```cpp
class RpcService {
    explicit RpcService(std::string_view name);
    void registerMethod(std::string_view name, RpcMethodHandler handler);
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

```cpp
template<typename T>
concept ServiceSelector = requires(T selector, const std::vector<ServiceEndpoint>& endpoints) {
    { selector.select(endpoints) } -> std::same_as<const ServiceEndpoint*>;
    { selector.update(endpoints) } -> std::same_as<void>;
};
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

# 启动客户端压测
./benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 100 -d 10 -s 256 -i 4

# 参数说明:
#   -h: 服务器地址
#   -p: 服务器端口
#   -c: 并发连接数
#   -d: 测试持续时间（秒）
#   -s: payload大小（字节）
#   -i: IO调度器数量
```

## 许可证

MIT License
