#!/bin/bash
# S2-run_tests.sh - 运行测试脚本

set -e

BUILD_DIR="build"
RESULT_DIR="test_results"

echo "=== Running galay-rpc Tests ==="

mkdir -p "$RESULT_DIR"

# 运行协议测试
echo "Running protocol tests..."
"$BUILD_DIR/T1-RpcProtocolTest"
cp T1-RpcProtocolTest.result "$RESULT_DIR/" 2>/dev/null || true

echo ""
echo "=== Test Results ==="
cat "$RESULT_DIR/T1-RpcProtocolTest.result" 2>/dev/null || echo "No results found"
