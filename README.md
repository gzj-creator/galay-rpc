# Galay-RPC

高性能 **C++23** 协程 RPC 框架，构建于 [galay-kernel](https://github.com/gzj-creator/galay-kernel) 异步运行时之上。

## 特性

- C++23 协程：统一 `co_await` 异步调用模型
- C++23 模块：支持 `export module` / `import` 使用方式
- 四种 RPC 模式：`unary` / `client_stream` / `server_stream` / `bidi`
- 真实流协议：支持 `STREAM_INIT` / `STREAM_DATA` / `STREAM_END` 生命周期
- 高效 IO：`RingBuffer + readv/writev`，支持 pipeline 与窗口化收发
- 服务发现：基于 C++23 Concept 约束的可扩展注册中心接口
- 工程完整：内置 `example/`、`test/`、`benchmark/`

## 文档导航

建议从 `docs/03-使用指南.md` 开始：

1. [架构设计](docs/01-架构设计.md)
2. [API参考](docs/02-API参考.md)
3. [使用指南](docs/03-使用指南.md)
4. [性能测试](docs/04-性能测试.md)

## 构建要求

- CMake 3.16+
- C++23 编译器（GCC 11+ / Clang 14+ / AppleClang 15+）
- `spdlog`
- Galay 内部依赖（统一联调推荐）：
  - `galay-kernel`（构建必需）
  - `galay-utils`（推荐）
  - `galay-http`（推荐）

## 依赖安装（macOS / Homebrew）

```bash
brew install cmake spdlog
```

## 依赖安装（Ubuntu / Debian）

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ libspdlog-dev
```

## 拉取源码（统一联调推荐）

```bash
git clone https://github.com/gzj-creator/galay-kernel.git
git clone https://github.com/gzj-creator/galay-utils.git
git clone https://github.com/gzj-creator/galay-http.git
git clone https://github.com/gzj-creator/galay-rpc.git
```

仅单独构建 `galay-rpc` 时，最小内部依赖为 `galay-kernel`。

## 构建

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

## 常用 CMake 选项

```cmake
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_BENCHMARKS "Build benchmark programs" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_MODULE_EXAMPLES "Build C++23 module(import/export) examples" ON)
```

> `BUILD_MODULE_EXAMPLES` 需要 CMake `>= 3.28` 且使用 `Ninja`/`Visual Studio` 生成器。
> 若使用 `Unix Makefiles`，该选项会自动关闭。

## 快速示例

### Echo（RPC 四模式）

```bash
# 终端 1
./build/example/E1-EchoServer 9000

# 终端 2
./build/example/E2-EchoClient 127.0.0.1 9000
```

### 真实 Stream（STREAM_* 协议）

```bash
# 终端 1
./build/example/E3-StreamServer 9100 1 131072

# 终端 2
./build/example/E4-StreamClient 127.0.0.1 9100 200 64
```

### C++23 模块化导入示例（import 版本）

```cpp
import galay.rpc;
```

```bash
# Echo import 版本
./build/example/E1-EchoServerImport 9000
./build/example/E2-EchoClientImport 127.0.0.1 9000

# Stream import 版本
./build/example/E3-StreamServerImport 9100 1 131072
./build/example/E4-StreamClientImport 127.0.0.1 9100 200 64
```

### 模块支持更新（2026-02）

本次模块接口已统一为：

- `module;`
- `#include "galay-rpc/module/ModulePrelude.hpp"`
- `export module galay.rpc;`
- `export { #include ... }`

对应文件：

- `galay-rpc/module/galay.rpc.cppm`
- `galay-rpc/module/ModulePrelude.hpp`

推荐构建（Clang 20 + Ninja）：

```bash
cmake -S . -B build-mod -G Ninja \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@20/bin/clang++
cmake --build build-mod --target galay-rpc-modules --parallel
```

## 运行测试与基准

### 测试

```bash
./build/test/T1-RpcProtocolTest

# 终端 1
./build/test/T2-RpcServerTest 9750

# 终端 2
./build/test/T3-RpcClientTest 127.0.0.1 9750
```

### RPC 压测（请求/响应）

```bash
# 终端 1
./build/benchmark/B1-RpcBenchServer 9000

# 终端 2
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m unary
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m client_stream
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m server_stream
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m bidi
```

### 真实 Stream 压测（窗口化）

```bash
# 终端 1
./build/benchmark/B4-RpcStreamBenchServer 9100 1 131072

# 终端 2
./build/benchmark/B5-RpcStreamBenchClient -h 127.0.0.1 -p 9100 -c 100 -d 5 -s 128 -f 16 -w 8 -i 0
```

`B5-RpcStreamBenchClient` 关键参数：

- `-f`: 每条 stream 的帧数（frames per stream）
- `-w`: 帧级 pipeline 窗口大小（默认 `1`，推荐压测 `8`）

## 性能数据（摘要）

> 测试环境：Apple M4 / 24GB RAM / macOS 15.7.3 / AppleClang / Release

### 四模式 Echo（47B，200连接，`-i 0`，`-l 4`，5秒）

| 模式 | QPS | 吞吐量 | P99 |
|------|-----|--------|-----|
| unary | 345,965 | 31.01 MB/s | 40,811 us |
| client_stream | 277,020 | 24.83 MB/s | 53,104 us |
| server_stream | 293,926 | 26.35 MB/s | 50,927 us |
| bidi | 283,218 | 25.39 MB/s | 71,229 us |

### 真实 Stream（100连接，128B，16帧）

窗口化 `-w` 多轮结果详见 [docs/04-性能测试.md](docs/04-性能测试.md)。

## 项目结构

```text
galay-rpc/
├── galay-rpc/          # 核心库（header-only）
│   ├── kernel/         # RpcServer / RpcClient / RpcService / RpcStream / ServiceDiscovery
│   ├── module/         # C++23 命名模块接口（galay.rpc.cppm）
│   └── protoc/         # RpcMessage / RpcCodec / RpcError / RpcBase
├── example/
│   ├── common/         # 示例公共配置
│   ├── include/        # include 版本示例（E1~E4）
│   └── import/         # import 版本示例（E1~E4）
├── test/               # 测试（T1~T3）
├── benchmark/          # 基准压测（B1~B5）
└── docs/               # 设计、API、使用与压测报告
```

## 许可证

MIT License
