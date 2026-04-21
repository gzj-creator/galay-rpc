# Release Note

按时间顺序追加版本记录，避免覆盖历史发布说明。

## v1.1.2 - 2026-04-21

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v1.1.2`
- Git Tag：`v1.1.2`
- 自述摘要：
  - 锁定 `galay-kernel 3.4.4` 的依赖版本，确保 `galay-rpc` 在最新基础库前缀下稳定构建。
  - 同步导出包配置的依赖版本约束，减少下游 `find_package(galay-rpc)` 时命中旧 `galay-kernel` 的风险。
