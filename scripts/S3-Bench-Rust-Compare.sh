#!/bin/bash
# scripts/S3-Bench-Rust-Compare.sh - 同机 C++ vs tonic 基线（B1/B2 + B4/B5）对照脚本

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

CPP_B1_SERVER_BIN="$BUILD_DIR/benchmark/B1-RpcBenchServer"
CPP_B2_CLIENT_BIN="$BUILD_DIR/benchmark/B2-RpcBenchClient"
CPP_B4_SERVER_BIN="$BUILD_DIR/benchmark/B4-RpcStreamBenchServer"
CPP_B5_CLIENT_BIN="$BUILD_DIR/benchmark/B5-RpcStreamBenchClient"

RR_SERVER_PORT="${SERVER_PORT:-9000}"
RR_SERVER_IO_COUNT="${SERVER_IO_COUNT:-0}"
RR_SERVER_RING="${SERVER_RING:-131072}"
RR_CLIENT_CONNECTIONS="${CLIENT_CONNECTIONS:-200}"
RR_CLIENT_DURATION="${CLIENT_DURATION:-5}"
RR_CLIENT_SIZE="${CLIENT_SIZE:-47}"
RR_CLIENT_PIPELINE="${CLIENT_PIPELINE:-4}"
RR_CLIENT_IO_COUNT="${CLIENT_IO_COUNT:-0}"
WORKLOAD_MODES_STRING="${WORKLOAD_MODES:-unary client_stream server_stream bidi}"

RUN_STREAM_COMPARE="${RUN_STREAM_COMPARE:-1}"
STREAM_SERVER_PORT="${STREAM_SERVER_PORT:-9100}"
STREAM_SERVER_IO_COUNT="${STREAM_SERVER_IO_COUNT:-1}"
STREAM_SERVER_RING="${STREAM_SERVER_RING:-131072}"
STREAM_CLIENT_CONNECTIONS="${STREAM_CLIENT_CONNECTIONS:-100}"
STREAM_CLIENT_DURATION="${STREAM_CLIENT_DURATION:-5}"
STREAM_CLIENT_SIZE="${STREAM_CLIENT_SIZE:-128}"
STREAM_CLIENT_FRAMES="${STREAM_CLIENT_FRAMES:-16}"
STREAM_CLIENT_WINDOW="${STREAM_CLIENT_WINDOW:-8}"
STREAM_CLIENT_IO_COUNT="${STREAM_CLIENT_IO_COUNT:-1}"

RUST_COMPARE_DIR="$REPO_ROOT/benchmark/compare/rust/tonic"
RUST_TARGET_DIR="${RUST_TARGET_DIR:-$RUST_COMPARE_DIR/target}"
CARGO_BIN="${CARGO_BIN:-/Users/gongzhijie/.rustup/toolchains/stable-aarch64-apple-darwin/bin/cargo}"
RUSTC_BIN="${RUSTC_BIN:-/Users/gongzhijie/.rustup/toolchains/stable-aarch64-apple-darwin/bin/rustc}"
RUST_HOST="${RUST_HOST:-127.0.0.1}"

RUST_CONNECTIONS="${RUST_CONNECTIONS:-$RR_CLIENT_CONNECTIONS}"
RUST_DURATION="${RUST_DURATION:-$RR_CLIENT_DURATION}"
RUST_PAYLOAD_SIZE="${RUST_PAYLOAD_SIZE:-$RR_CLIENT_SIZE}"
RUST_PIPELINE_DEPTH="${RUST_PIPELINE_DEPTH:-$RR_CLIENT_PIPELINE}"
RUST_IO_COUNT="${RUST_IO_COUNT:-$RR_CLIENT_IO_COUNT}"
RUST_WORKLOAD_MODES_STRING="${RUST_WORKLOAD_MODES:-$WORKLOAD_MODES_STRING}"

RUST_STREAM_CONNECTIONS="${RUST_STREAM_CONNECTIONS:-$STREAM_CLIENT_CONNECTIONS}"
RUST_STREAM_DURATION="${RUST_STREAM_DURATION:-$STREAM_CLIENT_DURATION}"
RUST_STREAM_PAYLOAD_SIZE="${RUST_STREAM_PAYLOAD_SIZE:-$STREAM_CLIENT_SIZE}"
RUST_STREAM_FRAMES="${RUST_STREAM_FRAMES:-$STREAM_CLIENT_FRAMES}"
RUST_STREAM_WINDOW="${RUST_STREAM_WINDOW:-$STREAM_CLIENT_WINDOW}"
RUST_STREAM_IO_COUNT="${RUST_STREAM_IO_COUNT:-$STREAM_CLIENT_IO_COUNT}"

function ensure_binary() {
    local path="$1"
    if [[ ! -x "$path" ]]; then
        echo "Missing benchmark binary: $path" >&2
        return 1
    fi
    return 0
}

function wait_for_port() {
    local host="$1"
    local port="$2"
    local attempts="${3:-30}"

    for _ in $(seq 1 "$attempts"); do
        if command -v nc >/dev/null 2>&1 && nc -z "$host" "$port" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

server_pid=""

function stop_server() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" >/dev/null 2>&1 || true
        server_pid=""
    fi
}

function cleanup() {
    stop_server
}

trap cleanup EXIT

function start_server() {
    local cmd="$1"
    local host="$2"
    local port="$3"
    local log_file="$4"

    bash -lc "$cmd" >"$log_file" 2>&1 &
    server_pid=$!
    if ! wait_for_port "$host" "$port" 30; then
        echo "Server did not become ready on $host:$port, log: $log_file" >&2
        return 1
    fi
    return 0
}

function run_cpp_request_response() {
    ensure_binary "$CPP_B1_SERVER_BIN"
    ensure_binary "$CPP_B2_CLIENT_BIN"

    echo "Starting Galay-C++ request/response benchmark server (B1, port $RR_SERVER_PORT)..."
    start_server "\"$CPP_B1_SERVER_BIN\" \"$RR_SERVER_PORT\" \"$RR_SERVER_IO_COUNT\" \"$RR_SERVER_RING\"" \
        "127.0.0.1" "$RR_SERVER_PORT" "/tmp/galay-rpc-cpp-b1-server.log"

    for mode in $WORKLOAD_MODES_STRING; do
        echo
        echo "========== C++ workload (B2): $mode =========="
        "$CPP_B2_CLIENT_BIN" \
            -h 127.0.0.1 \
            -p "$RR_SERVER_PORT" \
            -c "$RR_CLIENT_CONNECTIONS" \
            -d "$RR_CLIENT_DURATION" \
            -s "$RR_CLIENT_SIZE" \
            -i "$RR_CLIENT_IO_COUNT" \
            -l "$RR_CLIENT_PIPELINE" \
            -m "$mode"
    done

    stop_server
    echo
    echo "Galay-C++ request/response workloads completed."
}

function run_cpp_stream_path() {
    if [[ "$RUN_STREAM_COMPARE" != "1" ]]; then
        echo "RUN_STREAM_COMPARE=$RUN_STREAM_COMPARE, skip C++ B4/B5 stream compare path."
        return 0
    fi

    if ! ensure_binary "$CPP_B4_SERVER_BIN" || ! ensure_binary "$CPP_B5_CLIENT_BIN"; then
        echo "C++ B4/B5 stream binaries not found, stream path stays internal-only in this run."
        return 0
    fi

    echo
    echo "Starting Galay-C++ stream benchmark server (B4, port $STREAM_SERVER_PORT)..."
    start_server "\"$CPP_B4_SERVER_BIN\" \"$STREAM_SERVER_PORT\" \"$STREAM_SERVER_IO_COUNT\" \"$STREAM_SERVER_RING\"" \
        "127.0.0.1" "$STREAM_SERVER_PORT" "/tmp/galay-rpc-cpp-b4-server.log"

    echo
    echo "========== C++ workload (B5): stream =========="
    "$CPP_B5_CLIENT_BIN" \
        -h 127.0.0.1 \
        -p "$STREAM_SERVER_PORT" \
        -c "$STREAM_CLIENT_CONNECTIONS" \
        -d "$STREAM_CLIENT_DURATION" \
        -s "$STREAM_CLIENT_SIZE" \
        -f "$STREAM_CLIENT_FRAMES" \
        -w "$STREAM_CLIENT_WINDOW" \
        -i "$STREAM_CLIENT_IO_COUNT"

    stop_server
    echo
    echo "Galay-C++ stream workloads completed."
}

function run_default_rust_baseline() {
    if [[ ! -f "$RUST_COMPARE_DIR/Cargo.toml" ]]; then
        echo "Rust tonic baseline not found at $RUST_COMPARE_DIR"
        return 0
    fi

    echo
    echo "Building Rust tonic baseline..."
    pushd "$RUST_COMPARE_DIR" >/dev/null
    RUSTC="$RUSTC_BIN" CARGO_TARGET_DIR="$RUST_TARGET_DIR" "$CARGO_BIN" build --release
    popd >/dev/null

    local rust_server="$RUST_TARGET_DIR/release/tonic-bench-server"
    local rust_client="$RUST_TARGET_DIR/release/tonic-bench-client"
    ensure_binary "$rust_server"
    ensure_binary "$rust_client"

    echo
    echo "Starting Rust tonic benchmark server (request/response, port $RR_SERVER_PORT)..."
    start_server "\"$rust_server\" --host 0.0.0.0 --port \"$RR_SERVER_PORT\"" \
        "$RUST_HOST" "$RR_SERVER_PORT" "/tmp/galay-rpc-rust-b1-server.log"

    for mode in $RUST_WORKLOAD_MODES_STRING; do
        echo
        echo "========== Rust workload (B2 mapping): $mode =========="
        "$rust_client" \
            --host "$RUST_HOST" \
            --port "$RR_SERVER_PORT" \
            --connections "$RUST_CONNECTIONS" \
            --duration "$RUST_DURATION" \
            --payload-size "$RUST_PAYLOAD_SIZE" \
            --pipeline-depth "$RUST_PIPELINE_DEPTH" \
            --io-count "$RUST_IO_COUNT" \
            --mode "$mode"
    done

    stop_server
    echo
    echo "Rust request/response workloads completed."

    if [[ "$RUN_STREAM_COMPARE" != "1" ]]; then
        echo "RUN_STREAM_COMPARE=$RUN_STREAM_COMPARE, skip Rust stream benchmark path."
        return 0
    fi

    if ! ensure_binary "$CPP_B4_SERVER_BIN" || ! ensure_binary "$CPP_B5_CLIENT_BIN"; then
        echo "C++ B4/B5 unavailable in this build dir, skip Rust stream path for this run."
        return 0
    fi

    echo
    echo "Starting Rust tonic benchmark server (stream path, port $STREAM_SERVER_PORT)..."
    start_server "\"$rust_server\" --host 0.0.0.0 --port \"$STREAM_SERVER_PORT\"" \
        "$RUST_HOST" "$STREAM_SERVER_PORT" "/tmp/galay-rpc-rust-b4-server.log"

    echo
    echo "========== Rust workload (B5 mapping): stream_bench =========="
    "$rust_client" \
        --host "$RUST_HOST" \
        --port "$STREAM_SERVER_PORT" \
        --connections "$RUST_STREAM_CONNECTIONS" \
        --duration "$RUST_STREAM_DURATION" \
        --payload-size "$RUST_STREAM_PAYLOAD_SIZE" \
        --io-count "$RUST_STREAM_IO_COUNT" \
        --mode stream_bench \
        --frames-per-stream "$RUST_STREAM_FRAMES" \
        --frame-window "$RUST_STREAM_WINDOW"

    stop_server
    echo
    echo "Rust stream workloads completed."
}

echo "Benchmark compare build dir: $BUILD_DIR"
echo "Note: B3-ServiceDiscoveryBench currently has no fair mainstream Rust mapping; keep it internal-only."

run_cpp_request_response
run_cpp_stream_path

if [[ -n "${RUST_BASELINE_CMD:-}" ]]; then
    echo
    echo "Executing Rust baseline command defined by RUST_BASELINE_CMD..."
    eval "$RUST_BASELINE_CMD"
else
    run_default_rust_baseline
fi
