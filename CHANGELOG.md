# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `

## [Unreleased]

## [v2.0.2] - 2026-05-18

### Changed
- 将安装导出的 CMake targets 文件改为 `galayRpcConfigTargets.cmake`，同步 package config 的 include 路径。
- packaging 回归脚本中的 fake `galay-kernel` targets 文件同步改为 `galayKernelConfigTargets.cmake`。
- 将 CMake project 版本提升到 `2.0.2`，对齐本次发布 tag。


## [v2.0.1] - 2026-05-11

### Chore
- 移除 `benchmark/compare` 目录，避免误提交对比基准测试代码与构建产物。

## [v2.0.0] - 2026-04-29

### Changed
- 统一源码、头文件、测试、示例与 benchmark 文件命名为 `lower_snake_case`，编号前缀同步使用 `t<number>_`、`e<number>_` 与 `b<number>_` 风格。
- 同步更新构建脚本、模块入口、示例、测试、文档与脚本中的文件路径引用。
- 将项目内头文件包含调整为基于公开 include 根或模块根的非相对路径。

### Release
- 按大版本发布要求提升版本到 `v2.0.0`。

## [v1.1.3] - 2026-04-23

### Fixed
- 在 `include(CTest)` 之前显式钳制 `BUILD_TESTING=OFF`，并保留 `BUILD_TESTS` 兼容别名，避免 `galay-rpc` 默认无意构建测试目标。
- 补齐 `galay-rpc-config-version.cmake` 的生成与安装，修复带版本约束的 `find_package(galay-rpc ...)` 无法通过包版本匹配的问题。

### Added
- 新增 `scripts/tests/test_cmake_packaging.sh`，覆盖默认测试开关、兼容别名与安装后包版本探测回归。

## [v1.1.2] - 2026-04-21

### Changed
- 锁定 `galay-kernel 3.4.4` 的源码构建依赖版本，避免误命中旧的本地安装前缀。
- 同步更新导出包配置中的 `find_dependency(galay-kernel ...)` 版本约束，保持源码构建与下游消费一致。
