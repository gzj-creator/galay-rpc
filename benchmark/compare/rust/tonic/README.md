# tonic Benchmark Baseline

本目录提供 `galay-rpc` 本地压测对照用的 Rust 基线，覆盖 `B1/B2` 的四种调用模式，以及 `B4/B5` 的最小 stream benchmark 映射。

## 范围

- Rust popular baseline: `tonic`
- 当前支持：`unary`、`client_stream`、`server_stream`、`bidi`、`stream_bench`
- `stream_bench` 用于映射 `B4/B5` 的公开 stream benchmark 路径
- `B3-ServiceDiscoveryBench` 仍缺公平 Rust 对照，必须标记为 `internal-only` / `historical`

## 产物

- `tonic-bench-server`: RPC benchmark server（含 unary + stream handlers）
- `tonic-bench-client`: benchmark client（含 `unary/client_stream/server_stream/bidi/stream_bench`）

## 运行

```bash
RUSTC=/Users/gongzhijie/.rustup/toolchains/stable-aarch64-apple-darwin/bin/rustc \
CARGO_HOME=/tmp/cargo-rpc \
/Users/gongzhijie/.rustup/toolchains/stable-aarch64-apple-darwin/bin/cargo build --release \
  --manifest-path galay-rpc/benchmark/compare/rust/tonic/Cargo.toml

galay-rpc/benchmark/compare/rust/tonic/target/release/tonic-bench-server --port 9000

galay-rpc/benchmark/compare/rust/tonic/target/release/tonic-bench-client \
  --host 127.0.0.1 \
  --port 9000 \
  --connections 200 \
  --duration 5 \
  --payload-size 47 \
  --pipeline-depth 4 \
  --mode unary

galay-rpc/benchmark/compare/rust/tonic/target/release/tonic-bench-client \
  --host 127.0.0.1 \
  --port 9100 \
  --connections 100 \
  --duration 5 \
  --payload-size 128 \
  --mode stream_bench \
  --frames-per-stream 16 \
  --frame-window 8
```

`scripts/S3-Bench-Rust-Compare.sh` 会自动构建并运行 `B1/B2` + `B4/B5` 对照路径。
