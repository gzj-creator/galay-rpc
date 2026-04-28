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
