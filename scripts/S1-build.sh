#!/bin/bash
# S1-build.sh - 构建脚本

set -e

BUILD_DIR="build"
BUILD_TYPE="${1:-Release}"

echo "=== Building galay-rpc ==="
echo "Build type: $BUILD_TYPE"

# 创建构建目录
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 配置
cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTING=ON \
    -DBUILD_BENCHMARKS=ON \
    -DBUILD_EXAMPLES=ON

# 构建
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

echo "=== Build completed ==="
echo "Binaries are in: $BUILD_DIR/bin"
