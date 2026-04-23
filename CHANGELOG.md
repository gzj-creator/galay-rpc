# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `## [Unreleased]`。
- 需要发版时，从 `Unreleased` 或“自上次 tag 以来”的累计变更整理出新的版本节。
- 版本号遵循 `major/minor/patch` 规则：大改动升主版本，新功能升次版本，修复与非破坏性维护升修订版本。
- 推荐标题格式为 `## [vX.Y.Z] - YYYY-MM-DD`，正文按 `Added` / `Changed` / `Fixed` / `Docs` / `Chore` 归纳。

## [Unreleased]

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
