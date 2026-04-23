#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/galay-rpc-cmake.XXXXXX")

cleanup() {
    rm -rf "$TMP_ROOT"
}

trap cleanup EXIT INT TERM HUP

fail() {
    printf '%s\n' "FAIL: $*" >&2
    exit 1
}

assert_file_contains() {
    file=$1
    pattern=$2

    if ! grep -Eq "$pattern" "$file"; then
        fail "expected '$file' to contain pattern: $pattern"
    fi
}

FAKE_KERNEL_PREFIX="$TMP_ROOT/fake-kernel-prefix"
DEFAULT_BUILD_DIR="$TMP_ROOT/default"
ALIAS_BUILD_DIR="$TMP_ROOT/alias"
RPC_BUILD_DIR="$TMP_ROOT/rpc-build"
RPC_INSTALL_PREFIX="$TMP_ROOT/rpc-install"
CONSUMER_SOURCE_DIR="$TMP_ROOT/consumer"
CONSUMER_BUILD_DIR="$TMP_ROOT/consumer-build"

mkdir -p "$FAKE_KERNEL_PREFIX/lib/cmake/galay-kernel" "$CONSUMER_SOURCE_DIR"

cat > "$FAKE_KERNEL_PREFIX/lib/cmake/galay-kernel/galay-kernel-targets.cmake" <<'EOF'
if(NOT TARGET galay-kernel::galay-kernel)
    add_library(galay-kernel::galay-kernel INTERFACE IMPORTED)
endif()
EOF

cat > "$FAKE_KERNEL_PREFIX/lib/cmake/galay-kernel/galay-kernel-config.cmake" <<'EOF'
include("${CMAKE_CURRENT_LIST_DIR}/galay-kernel-targets.cmake")
EOF

cat > "$FAKE_KERNEL_PREFIX/lib/cmake/galay-kernel/galay-kernel-config-version.cmake" <<'EOF'
set(PACKAGE_VERSION "3.4.4")

if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
EOF

cmake -S "$REPO_ROOT" -B "$DEFAULT_BUILD_DIR" >/dev/null
assert_file_contains "$DEFAULT_BUILD_DIR/CMakeCache.txt" '^BUILD_TESTING:BOOL=OFF$'

cmake -S "$REPO_ROOT" -B "$ALIAS_BUILD_DIR" -DBUILD_TESTS=ON >/dev/null
if ! cmake --build "$ALIAS_BUILD_DIR" --target help | grep -E 'test|T1-' >/dev/null; then
    fail "expected BUILD_TESTS=ON to expose test targets"
fi

cmake -S "$REPO_ROOT" -B "$RPC_BUILD_DIR" \
    -DCMAKE_PREFIX_PATH="$FAKE_KERNEL_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$RPC_INSTALL_PREFIX" \
    -DBUILD_TESTING=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_MODULE_EXAMPLES=OFF >/dev/null
cmake --install "$RPC_BUILD_DIR" >/dev/null

cat > "$CONSUMER_SOURCE_DIR/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(galay_rpc_consumer LANGUAGES CXX)

find_package(galay-rpc 1.1.2 REQUIRED CONFIG)
EOF

cmake -S "$CONSUMER_SOURCE_DIR" -B "$CONSUMER_BUILD_DIR" \
    -DCMAKE_PREFIX_PATH="$RPC_INSTALL_PREFIX;$FAKE_KERNEL_PREFIX" >/dev/null

printf '%s\n' "ok"
