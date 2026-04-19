# Galay-RPC

高性能 **C++23** 协程 RPC 框架，构建于 [galay-kernel](https://github.com/gzj-creator/galay-kernel) 异步运行时之上。

## 特性

- C++23 协程：统一 `co_await` 异步调用模型
- C++23 模块：支持 `export module` / `import` 使用方式
- 四种 RPC 模式：`unary` / `client_stream` / `server_stream` / `bidi`
- 真实流协议：支持 `STREAM_INIT` / `STREAM_DATA` / `STREAM_END` 生命周期
- 高效 IO：`RingBuffer + readv/writev`，支持 pipeline 与窗口化收发
- 服务发现：基于 C++23 Concept 约束的可扩展注册中心接口
- 工程完整：内置 `examples/`、`test/`、`benchmark/`

## 文档导航

建议先看 `docs/README.md`，再从 `docs/00-快速开始.md` 开始：

0. [文档总览](docs/README.md) - 规范化阅读顺序、真相来源与页面约定
1. [快速开始](docs/00-快速开始.md) - 环境搭建、编译、运行示例
2. [架构设计](docs/01-架构设计.md) - 整体架构、协议设计、模块划分
3. [API参考](docs/02-API参考.md) - 完整 API 文档
4. [使用指南](docs/03-使用指南.md) - 服务端、客户端、流式传输、服务发现
5. [示例代码](docs/04-示例代码.md) - 与真实 `examples/` / `test/` 文件逐项对齐的示例清单
6. [性能测试](docs/05-性能测试.md) - benchmark target、压测命令、复现要求与历史说明
7. [高级主题](docs/06-高级主题.md) - 负载均衡、超时重试、性能优化、安全性
8. [常见问题](docs/07-常见问题.md) - 编译、连接、调用、性能等问题解答

## 文档真相来源

当文档与仓库内容冲突时，以以下顺序为准：

1. 公开头文件与导出 target
2. 实现行为
3. `examples/`
4. `test/`
5. `benchmark/`
6. Markdown 文档

## 构建要求

- CMake 3.16+
- C++23 编译器（GCC 11+ / Clang 14+ / AppleClang 15+）
- Galay 内部依赖（统一联调推荐）：
  - `galay-kernel`（构建必需）
  - `galay-utils`（推荐）
  - `galay-http`（推荐）

## 依赖安装（macOS / Homebrew）

```bash
brew install cmake
```

## 依赖安装（Ubuntu / Debian）

```bash
sudo apt-get update
sudo apt-get install -y cmake g++
```

## 拉取源码（统一联调推荐）

```bash
git clone https://github.com/gzj-creator/galay-kernel.git
git clone https://github.com/gzj-creator/galay-utils.git
git clone https://github.com/gzj-creator/galay-http.git
git clone https://github.com/gzj-creator/galay-rpc.git
cd galay-rpc
```

仅单独构建 `galay-rpc` 时，最小内部依赖为 `galay-kernel`。本仓库现在只走标准 `find_package(galay-kernel CONFIG REQUIRED)`；依赖必须通过 `galay-kernel_DIR`、`CMAKE_PREFIX_PATH` 或系统安装前缀提供。

## 构建

```bash
# 若刚刚按上面的顺序并排 clone，先为 galay-kernel 生成 package config
cmake -S ../galay-kernel -B ../galay-kernel/build -DCMAKE_BUILD_TYPE=Release
cmake --build ../galay-kernel/build --parallel

# 再在 galay-rpc 仓库根目录构建本项目
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

如果 `galay-kernel` 不在默认安装前缀，可显式指定安装前缀或 package config 目录：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/galay-kernel/install
```

也可直接指定 package config 所在目录：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -Dgalay-kernel_DIR=/path/to/galay-kernel/lib/cmake/galay-kernel
```

## 常用 CMake 选项

```cmake
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_BENCHMARKS "Build benchmark programs" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_MODULE_EXAMPLES "Build C++23 module(import/export) examples" ON)
option(GALAY_RPC_INSTALL_MODULE_INTERFACE "Install C++ module interface files (*.cppm)" ON)
option(DISABLE_IOURING "Disable io_uring and use epoll on Linux" ON)
```

- `BUILD_MODULE_EXAMPLES` 需要 CMake `>= 3.28` 且使用 `Ninja` / `Visual Studio` 生成器；使用 `Unix Makefiles` 时会在配置阶段自动关闭。
- `galay-rpc::galay-rpc` 是头文件 `INTERFACE` target；当前仓库没有共享库 / 静态库切换开关。
- `GALAY_RPC_INSTALL_MODULE_INTERFACE=ON` 时：
  - 若当前工具链能生成 `galay-rpc-modules`，安装包会导出 `galay-rpc::galay-rpc-modules` 并安装 `galay.rpc.cppm`
  - 若当前工具链不能生成模块 target，仍会安装原始 `galay.rpc.cppm` 文件，便于消费端自行导入
- `GALAY_RPC_INSTALL_MODULE_INTERFACE=OFF` 时，安装包只保留头文件公开面，不安装 `.cppm`
- `DISABLE_IOURING=ON` 是 Linux 默认值；如需尝试 `io_uring`，请显式传入 `-DDISABLE_IOURING=OFF` 并确保系统可找到 `liburing`

## 快速示例

以下命令均在 `galay-rpc` 仓库根目录执行；终端 1 需要保持运行。

### Echo（RPC 四模式）

```bash
# 终端 1
./build/examples/E1-EchoServer 9000

# 终端 2
./build/examples/E2-EchoClient 127.0.0.1 9000
```

> 该链路现已由 `EchoExampleSmokeTest` 自动验证，不再依赖手工肉眼确认。

### 真实 Stream（STREAM_* 协议）

```bash
# 终端 1
./build/examples/E3-StreamServer 9100 1 131072

# 终端 2
./build/examples/E4-StreamClient 127.0.0.1 9100 200 64
```

> 该 include 链路现已由 `StreamExampleSmokeTest` 自动验证。

### C++23 模块化导入示例（import 版本）

```cpp
import galay.rpc;
```

```bash
# Echo import 版本
./build/examples/E1-EchoServerImport 9000
./build/examples/E2-EchoClientImport 127.0.0.1 9000

# Stream import 版本
./build/examples/E3-StreamServerImport 9100 1 131072
./build/examples/E4-StreamClientImport 127.0.0.1 9100 200 64
```

> 当 `BUILD_MODULE_EXAMPLES=ON` 且生成器支持 C++ 模块时，Echo import 链路由 `EchoImportExampleSmokeTest` 自动验证，真实 Stream import 链路由 `StreamImportExampleSmokeTest` 自动验证。

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
./build/benchmark/B1-RpcBenchServer 9000 0 131072

# 终端 2
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m unary
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m client_stream
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m server_stream
./build/benchmark/B2-RpcBenchClient -h 127.0.0.1 -p 9000 -c 200 -d 5 -s 47 -i 0 -l 4 -m bidi
```

### 真实 Stream 压测（窗口化）

```bash
# 终端 1
./build/benchmark/B4-RpcStreamBenchServer 9100 0 131072

# 终端 2
./build/benchmark/B5-RpcStreamBenchClient -h 127.0.0.1 -p 9100 -c 100 -d 5 -s 128 -f 16 -w 8 -i 0
```

> `RpcBenchmarkSmokeTest` 与 `RpcStreamBenchmarkSmokeTest` 会用较轻负载验证上述 benchmark server/client 命令形态；其中 `0` 表示自动选择 IO scheduler 数量。

`B5-RpcStreamBenchClient` 关键参数：

- `-f`: 每条 stream 的帧数（frames per stream）
- `-w`: 帧级 pipeline 窗口大小（默认 `1`，推荐压测 `8`）

## 性能验证状态

- 仓库当前公开了 5 个 benchmark target：`B1-RpcBenchServer`、`B2-RpcBenchClient`、`B3-ServiceDiscoveryBench`、`B4-RpcStreamBenchServer`、`B5-RpcStreamBenchClient`
- 本次文档修复未重新产出新的基准数值，因此 README 不再把旧的 QPS / P99 表当作当前事实陈述
- 真实 target、参数含义、复现实验命令和历史状态说明见 [docs/05-性能测试.md](docs/05-性能测试.md)

## 已知限制与边界

- `docs/06-高级主题.md` 中涉及 etcd / Consul / HTTP / JSON / 熔断 / 限流 / 指标采集的内容是集成思路，不代表仓库内置了这些第三方实现
- `BUILD_MODULE_EXAMPLES` 依赖 CMake `>= 3.28` 且需要支持 C++ 模块的生成器
- benchmark 结果与机器、编译器、payload、pipeline 深度、调度器数量强相关；没有原始输出与环境记录的数字不应被视为当前版本的可审计结论

### Rust 对标入口

- `scripts/S3-Bench-Rust-Compare.sh` 是基于 `B1-RpcBenchServer` / `B2-RpcBenchClient` 的请求/响应压测流程，默认会在同台机器上启动 C++ 服务端，并执行客户端命令。完成 C++ 端实验后脚本会提示或执行 `RUST_BASELINE_CMD`（可指向 `benchmark/compare/rust/tonic` 中的实现），以确保记录 C++ 与 Rust 的同构 workloads。
- `benchmark/compare/rust/tonic/README.md` 说明了当前推荐的 Rust `tonic` 对照实现模板，后续可在该目录继续推进完整 server/client，并在对外发布时一并附上相同的参数与执行环境；没有 Rust 基线的历史数据请标记为 internal-only / historical，并避免对外宣传。

## 项目结构

```text
galay-rpc/
├── galay-rpc/          # 核心库（header-only）
│   ├── kernel/         # RpcServer / RpcClient / RpcService / RpcStream / ServiceDiscovery
│   ├── module/         # C++23 命名模块接口（galay.rpc.cppm）
│   └── protoc/         # RpcMessage / RpcCodec / RpcError / RpcBase
├── examples/
│   ├── common/         # 示例公共配置
│   ├── include/        # include 版本示例（E1~E4）
│   └── import/         # import 版本示例（E1~E4）
├── test/               # 测试（T1~T3）
├── benchmark/          # 基准压测（B1~B5）
└── docs/               # 设计、API、示例、压测与 FAQ
```

## 许可证

MIT License
