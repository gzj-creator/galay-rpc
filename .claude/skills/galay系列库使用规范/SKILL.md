# Galay 系列库使用指南

本文档汇总了 galay 系列各个库的功能和使用方法，方便快速查阅。

---

## 目录

1. [galay-kernel](#galay-kernel) - 协程网络核心库
2. [galay-http](#galay-http) - HTTP/WebSocket 库
3. [galay-ssl](#galay-ssl) - SSL/TLS 加密库
4. [galay-redis](#galay-redis) - Redis 客户端库
5. [galay-mcp](#galay-mcp) - MCP 协议库

---

## galay-kernel

高性能 C++20 协程网络库，是整个 galay 系列的基础。

### 核心特性

- 单线程 26-28万 QPS，130+ MB/s 吞吐量
- 跨平台：macOS (kqueue)、Linux (epoll/io_uring)
- 异步文件 IO 支持
- MpscChannel 多生产者单消费者通道（1100-1600万 msg/s）

### Runtime 使用

```cpp
#include "galay-kernel/kernel/Runtime.h"

using namespace galay::kernel;

// 零配置启动（推荐）- 自动创建 2*CPU 核心数的 IO 调度器
Runtime runtime;
runtime.start();

// 或指定调度器数量
Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 4, 8);  // 4 IO + 8 计算

// 获取调度器（负载均衡）
auto* io_scheduler = runtime.getNextIOScheduler();
auto* compute_scheduler = runtime.getNextComputeScheduler();

// 提交协程任务
io_scheduler->spawn(myCoroutine());

runtime.stop();
```

### TcpSocket 服务器

```cpp
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/KqueueScheduler.h"  // macOS
// #include "galay-kernel/kernel/EpollScheduler.h"  // Linux

using namespace galay::async;
using namespace galay::kernel;

Coroutine handleClient(IOScheduler* scheduler, GHandle handle) {
    TcpSocket client(scheduler, handle);
    client.option().handleNonBlock();

    char buffer[1024];
    while (true) {
        auto result = co_await client.recv(buffer, sizeof(buffer));
        if (!result || result.value().size() == 0) break;
        co_await client.send(result.value().c_str(), result.value().size());
    }
    co_await client.close();
}

Coroutine server(IOScheduler* scheduler) {
    TcpSocket listener(scheduler);
    listener.create(IPType::IPV4);
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();
    listener.bind(Host(IPType::IPV4, "0.0.0.0", 8080));
    listener.listen(1024);

    while (true) {
        Host clientHost;
        auto result = co_await listener.accept(&clientHost);
        if (result) {
            scheduler->spawn(handleClient(scheduler, result.value()));
        }
    }
}
```

### TcpSocket 客户端

```cpp
Coroutine client(IOScheduler* scheduler) {
    TcpSocket socket(scheduler);
    socket.create(IPType::IPV4);
    socket.option().handleNonBlock();

    co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", 8080));
    co_await socket.send("Hello", 5);

    char buffer[1024];
    auto result = co_await socket.recv(buffer, sizeof(buffer));
    co_await socket.close();
}
```

### AsyncFile 异步文件操作

```cpp
#include "galay-kernel/async/AsyncFile.h"

Coroutine fileOps(IOScheduler* scheduler) {
    AsyncFile file(scheduler);
    file.open("/path/to/file", FileOpenMode::ReadWrite);

    char buffer[4096];
    co_await file.read(buffer, sizeof(buffer), 0);  // offset=0
    co_await file.write("data", 4, 0);
    co_await file.close();
}
```

---

## galay-http

基于 galay-kernel 的 HTTP/WebSocket 库。

### 核心特性

- HTTP 服务器内置 Runtime，无需手动配置
- 路由系统：精确匹配 O(1) + Trie 树模糊匹配 O(k)
- 静态文件服务：支持 Range 请求、ETag、断点续传
- 四种文件传输模式：MEMORY、CHUNK、SENDFILE、AUTO
- 完整 WebSocket 支持

### HTTP 服务器

```cpp
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"

using namespace galay::http;
using namespace galay::kernel;

Coroutine echoHandler(HttpConn& conn, HttpRequest req) {
    std::string body = req.getBodyStr();

    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP/1.0")
        .text("Echo: " + body)
        .build();

    auto writer = conn.getWriter();
    co_await writer.sendResponse(response);
    co_await conn.close();
}

int main() {
    HttpRouter router;
    router.addHandler<HttpMethod::POST>("/echo", echoHandler);

    // 路由模式示例
    router.addHandler<HttpMethod::GET>("/api/users", handleUsers);      // 精确
    router.addHandler<HttpMethod::GET>("/user/{id}", handleUser);       // 路径参数
    router.addHandler<HttpMethod::GET>("/static/*", handleStatic);      // 通配符
    router.addHandler<HttpMethod::GET>("/files/**", handleFiles);       // 贪婪通配符

    // 静态文件挂载
    router.mount("/static", "./public");
    router.mountHardly("/assets", "./assets");  // 预加载到内存

    HttpServerConfig config;
    config.host = "0.0.0.0";
    config.port = 8080;

    HttpServer server(config);
    server.start(std::move(router));

    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}
```

### HTTP 客户端

```cpp
#include "galay-http/kernel/http/HttpClient.h"

using namespace galay::http;

Coroutine sendRequest(Runtime& runtime) {
    HttpClient client;
    auto connect_result = co_await client.connect("http://127.0.0.1:8080/echo");
    if (!connect_result) co_return;

    // 发送请求并循环等待响应
    while (true) {
        auto result = co_await client.post(
            client.url().path,
            "Hello, Server!",
            "text/plain"
        );
        if (!result) co_return;
        if (!result.value()) continue;  // 继续等待

        auto response = result.value().value();
        std::cout << "Response: " << response.getBodyStr() << "\n";
        break;
    }

    co_await client.close();
}
```

### Response Builder

```cpp
// 常用响应
auto ok = Http1_1ResponseBuilder::ok().text("Hello").build();
auto json = Http1_1ResponseBuilder::ok().json(R"({"status":"ok"})").build();
auto html = Http1_1ResponseBuilder::ok().html("<h1>Welcome</h1>").build();
auto notFound = Http1_1ResponseBuilder::notFound().text("Not Found").build();

// 自定义状态码
auto custom = Http1_1ResponseBuilder()
    .status(201)
    .header("Location", "/users/123")
    .json(R"({"id": 123})")
    .build();
```

### Request Builder

```cpp
auto getReq = Http1_1RequestBuilder::get("/api/users")
    .host("example.com")
    .header("User-Agent", "Galay-HTTP-Client/1.0")
    .build();

auto postReq = Http1_1RequestBuilder::post("/api/users")
    .host("example.com")
    .json(R"({"name": "John"})")
    .build();
```

### Chunked 传输

```cpp
// 服务端发送 Chunked
Coroutine handleChunked(HttpConn& conn, HttpRequest req) {
    auto writer = conn.getWriter();

    HttpResponseHeader header;
    header.version() = HttpVersion::HttpVersion_1_1;
    header.code() = HttpStatusCode::OK_200;
    header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
    co_await writer.sendHeader(std::move(header));

    co_await writer.sendChunk("First chunk\n", false);
    co_await writer.sendChunk("Second chunk\n", false);
    co_await writer.sendChunk("", true);  // 最后一个 chunk

    co_await conn.close();
}
```

### WebSocket 服务器

```cpp
#include "galay-http/kernel/websocket/WsConn.h"

Coroutine wsHandler(HttpConn& conn, HttpRequest req) {
    auto wsConn = co_await galay::websocket::upgradeToWebSocket(conn, req);
    if (!wsConn) co_return;

    while (true) {
        WebSocketFrame frame;
        auto result = co_await wsConn->receive(frame);
        if (!result || frame.opcode == WebSocketOpcode::CLOSE) break;

        co_await wsConn->send(frame.payload, WebSocketOpcode::TEXT);
    }

    co_await wsConn->close();
}
```

### WebSocket 客户端

```cpp
#include "galay-http/kernel/websocket/WsClient.h"

Coroutine wsClient() {
    WsClient client;
    co_await client.connect("ws://127.0.0.1:8080/ws");

    co_await client.send("Hello!", WebSocketOpcode::TEXT);

    WebSocketFrame frame;
    co_await client.receive(frame);
    std::cout << "Received: " << frame.payload << "\n";

    co_await client.close();
}
```

---

## galay-ssl

基于 galay-kernel 的异步 SSL/TLS 库。

### 核心特性

- 基于 OpenSSL
- 支持 TLS 1.2/1.3
- 支持证书验证和 SNI
- 支持 epoll 和 io_uring 两种 IO 后端

### SSL 服务端

```cpp
#include "galay-ssl/async/SslSocket.h"
#include "galay-kernel/kernel/EpollScheduler.h"

using namespace galay::ssl;
using namespace galay::kernel;

Coroutine handleClient(SslContext* ctx, GHandle handle) {
    SslSocket client(ctx, handle);
    client.option().handleNonBlock();

    // SSL 握手 - 需要循环直到完成
    while (!client.isHandshakeCompleted()) {
        auto result = co_await client.handshake();
        if (!result) {
            auto& err = result.error();
            if (err.code() == SslErrorCode::kHandshakeWantRead ||
                err.code() == SslErrorCode::kHandshakeWantWrite) {
                continue;
            }
            co_await client.close();
            co_return;
        }
        break;
    }

    // 接收数据
    char buffer[4096];
    while (true) {
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            auto& err = recvResult.error();
            if (err.sslError() == SSL_ERROR_WANT_READ ||
                err.sslError() == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            break;
        }
        if (recvResult.value().size() == 0) break;

        co_await client.send(recvResult.value().c_str(), recvResult.value().size());
    }

    co_await client.shutdown();
    co_await client.close();
}

Coroutine sslServer(IOScheduler* scheduler, SslContext* ctx, uint16_t port) {
    SslSocket listener(ctx);
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();
    listener.bind(Host(IPType::IPV4, "0.0.0.0", port));
    listener.listen(1024);

    while (true) {
        Host clientHost;
        auto result = co_await listener.accept(&clientHost);
        if (result) {
            scheduler->spawn(handleClient(ctx, result.value()));
        }
    }
}

int main() {
    SslContext ctx(SslMethod::TLS_Server);
    ctx.loadCertificate("server.crt");
    ctx.loadPrivateKey("server.key");

    EpollScheduler scheduler;
    scheduler.start();
    scheduler.spawn(sslServer(&scheduler, &ctx, 8443));

    // 等待...
    scheduler.stop();
}
```

### SSL 客户端

```cpp
Coroutine sslClient(SslContext* ctx, const std::string& host, uint16_t port) {
    SslSocket socket(ctx);
    socket.option().handleNonBlock();
    socket.setHostname(host);  // SNI

    auto connectResult = co_await socket.connect(Host(IPType::IPV4, host, port));
    if (!connectResult) co_return;

    // SSL 握手
    while (!socket.isHandshakeCompleted()) {
        auto result = co_await socket.handshake();
        if (!result) {
            auto& err = result.error();
            if (err.code() == SslErrorCode::kHandshakeWantRead ||
                err.code() == SslErrorCode::kHandshakeWantWrite) {
                continue;
            }
            co_await socket.close();
            co_return;
        }
        break;
    }

    co_await socket.send("Hello, SSL!", 11);

    char buffer[4096];
    while (true) {
        auto recvResult = co_await socket.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            if (recvResult.error().sslError() == SSL_ERROR_WANT_READ ||
                recvResult.error().sslError() == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            break;
        }
        break;
    }

    co_await socket.shutdown();
    co_await socket.close();
}
```

---

## galay-redis

基于 C++20 协程的高性能异步 Redis 客户端。

### 核心特性

- 完整的超时支持（`.timeout()` 方法）
- Pipeline 批处理（性能提升 100x）
- 单命令 ~14,000 QPS，Pipeline 可达 1,400,000 QPS

### 基本使用

```cpp
#include "galay-redis/async/RedisClient.h"
#include <galay-kernel/kernel/Runtime.h>

using namespace galay::redis;
using namespace galay::kernel;

Coroutine example(IOScheduler* scheduler) {
    RedisClient client(scheduler);

    // 连接
    co_await client.connect("127.0.0.1", 6379);
    // 或使用 URL: co_await client.connect("redis://:password@127.0.0.1:6379/0");

    // 执行命令（支持超时）
    auto result = co_await client.set("key", "value").timeout(std::chrono::seconds(5));

    if (result && result.value()) {
        std::cout << "SET succeeded" << std::endl;
    }

    // 获取数据
    auto get_result = co_await client.get("key");
    if (get_result && get_result.value()) {
        auto& values = get_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "Value: " << values[0].toString() << std::endl;
        }
    }

    co_await client.close();
}

int main() {
    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(example(scheduler));

    std::this_thread::sleep_for(std::chrono::seconds(5));
    runtime.stop();
}
```

### Pipeline 批处理

```cpp
// 构建批量命令
std::vector<std::vector<std::string>> commands = {
    {"SET", "key1", "value1"},
    {"SET", "key2", "value2"},
    {"GET", "key1"},
    {"GET", "key2"}
};

// 执行 Pipeline（性能提升 100x）
auto result = co_await client.pipeline(commands).timeout(std::chrono::seconds(30));
```

### 常用命令

```cpp
// String
co_await client.set("key", "value");
co_await client.get("key");
co_await client.del("key");
co_await client.incr("counter");

// Hash
co_await client.hset("hash", "field", "value");
co_await client.hget("hash", "field");
co_await client.hgetAll("hash");

// List
co_await client.lpush("list", "value");
co_await client.rpush("list", "value");
co_await client.lrange("list", 0, -1);

// Set
co_await client.sadd("set", "member");
co_await client.smembers("set");

// Sorted Set
co_await client.zadd("zset", 100.0, "member");
co_await client.zrange("zset", 0, -1);
```

### 错误处理

```cpp
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

if (!result) {
    if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
        // 处理超时
    } else {
        // 处理其他错误
    }
}
```

---

## galay-mcp

基于 Galay-Kernel 的 MCP (Model Context Protocol) 协议库。

### 核心特性

- 标准输入输出传输
- JSON-RPC 2.0 协议
- 遵循 MCP 2024-11-05 规范

### MCP 服务器

```cpp
#include "galay-mcp/server/McpStdioServer.h"

McpStdioServer server;

// 添加工具
server.addTool("add", "Add two numbers",
    [](const nlohmann::json& args) -> std::expected<nlohmann::json, McpError> {
        int a = args["a"];
        int b = args["b"];
        return nlohmann::json{{"result", a + b}};
    }
);

// 添加资源
server.addResource("file:///path/to/file", "My File", "text/plain");

// 添加提示
server.addPrompt("greeting", "A greeting prompt");

// 启动服务器（从 stdin 读取，向 stdout 写入）
server.run();
```

### MCP 客户端

```cpp
#include "galay-mcp/client/McpStdioClient.h"

McpStdioClient client;

// 初始化连接
auto init_result = client.initialize("MyClient", "1.0.0");
if (!init_result) {
    // 处理错误
}

// 调用工具
nlohmann::json args = {{"a", 10}, {"b", 20}};
auto result = client.callTool("add", args);
if (result) {
    std::cout << "Result: " << result.value() << std::endl;
}

// 获取工具列表
auto tools = client.listTools();

// 获取资源列表
auto resources = client.listResources();

// 断开连接
client.disconnect();
```

### 协议格式

请求：
```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"add","arguments":{"a":10,"b":20}}}
```

响应：
```json
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"30"}]}}
```

---

## 依赖关系

```
galay-kernel (核心)
    ↓
    ├── galay-http (HTTP/WebSocket)
    ├── galay-ssl (SSL/TLS)
    ├── galay-redis (Redis 客户端)
    └── galay-mcp (MCP 协议)
```

## 构建要求

- C++20/23 编译器 (GCC 11+/13+, Clang 14+/16+)
- CMake 3.16+
- OpenSSL 1.1.1+ (galay-ssl)
- nlohmann/json (galay-mcp)
- Linux: liburing (可选), libaio
