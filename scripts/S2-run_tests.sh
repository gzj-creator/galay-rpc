#!/bin/bash
# s2_run_tests.sh - 运行测试脚本

set -e

BUILD_DIR="build"
RESULT_DIR="test_results"

echo "=== Running galay-rpc Tests ==="

mkdir -p "$RESULT_DIR"

# 运行协议测试
echo "Running protocol tests..."
"$BUILD_DIR/t1_proto"
cp t1_proto.result "$RESULT_DIR/" 2>/dev/null || true

echo ""
echo "=== Test Results ==="
cat "$RESULT_DIR/t1_proto.result" 2>/dev/null || echo "No results found"
