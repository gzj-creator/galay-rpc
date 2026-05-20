# Release Note

按时间顺序追加版本记录，避免覆盖历史发布说明。

## v1.1.2 - 2026-04-21

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v1.1.2`
- Git Tag：`v1.1.2`
- 自述摘要：
  - 锁定 `galay-kernel 3.4.4` 的依赖版本，确保 `galay-rpc` 在最新基础库前缀下稳定构建。
  - 同步导出包配置的依赖版本约束，减少下游 `find_package(galay-rpc)` 时命中旧 `galay-kernel` 的风险。

## v1.1.3 - 2026-04-23

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v1.1.3`
- Git Tag：`v1.1.3`
- 自述摘要：
  - 将 `BUILD_TESTING` 的默认值前置钳制为 `OFF`，并保留 `BUILD_TESTS` 兼容别名，统一 `galay-rpc` 默认不编测试的行为。
  - 补齐 `galay-rpc-config-version.cmake` 的生成与安装，修复下游使用带版本约束的 `find_package(galay-rpc 2.0.0 REQUIRED CONFIG)` 时无法通过兼容性判定的问题。
  - 新增 `scripts/tests/test_cmake_packaging.sh`，回归验证默认测试开关与 CMake 包版本导出链路。

## v2.0.0 - 2026-04-29

- 版本级别：大版本（major）
- Git 提交消息：`refactor: 统一源码文件命名规范`
- Git Tag：`v2.0.0`
- 自述摘要：
  - 将源码、头文件、测试、示例与 benchmark 文件统一重命名为 lower_snake_case，编号前缀同步改为小写下划线形式。
  - 同步更新 CMake/Bazel 构建描述、模块入口、README/docs、脚本和所有项目内 include 路径引用。
  - 移除项目内相对 include，统一使用基于公开 include 根或模块根的非相对路径。

## v2.0.1 - 2026-05-11

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 移除 benchmark compare 目录`
- Git Tag：`v2.0.1`
- 自述摘要：
  - 移除 `benchmark/compare` 目录并收紧忽略规则，避免误提交对比基准测试代码与构建产物。

## v2.0.2 - 2026-05-18

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 统一 CMake 导出文件命名`
- Git Tag：`v2.0.2`
- 自述摘要：
  - 将安装导出的 CMake targets 文件改为 `galayRpcConfigTargets.cmake`，并同步 `galay-rpc-config.cmake` 的 include 路径。
  - packaging 回归脚本中的 fake `galay-kernel` targets 文件同步改为 `galayKernelConfigTargets.cmake`，覆盖新的依赖导出命名。
  - 将 CMake project 版本提升到 `2.0.2`，确保源码版本元数据、tag 与发布记录一致。

## v2.1.0 - 2026-05-20

- 版本级别：中版本（minor）
- Git 提交消息：`feat: 增加 rpc 库级 BaseLogger 日志入口`
- Git Tag：`v2.1.0`
- 自述摘要：
  - 新增 `galay::rpc::log::set/get`，用户可只为 `galay-rpc` 设置 `BaseLogger`，不会影响其他 galay 库日志。
  - 新增 `RPC_LOG_*` 与 `RPC_LOG_ENABLED`，并在客户端调用、服务端监听/调度/路由、流式服务错误路径增加按级别过滤的日志埋点。
  - 将 `galay-kernel` 依赖约束同步提升到 `5.0.0`，项目版本提升到 `2.1.0`。
  - 新增日志入口测试，覆盖空 logger 与级别过滤时不执行日志参数构造的行为。
