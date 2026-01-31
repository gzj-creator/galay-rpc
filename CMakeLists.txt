cmake_minimum_required(VERSION 3.16)
project(galay-rpc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 选项
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_BENCHMARKS "Build benchmark programs" ON)
option(BUILD_EXAMPLES "Build example programs" ON)

# 查找依赖
find_package(spdlog REQUIRED)
find_package(galay-kernel REQUIRED)

# 收集头文件
file(GLOB_RECURSE RPC_HEADERS
    "galay-rpc/protoc/*.h"
    "galay-rpc/kernel/*.h"
    "galay-rpc/utils/*.h"
)

# 创建INTERFACE库（header-only）
add_library(galay-rpc INTERFACE)

target_include_directories(galay-rpc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-rpc INTERFACE galay-kernel::galay-kernel)

# 测试
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# 压测
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

# 示例
if(BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

# 安装
install(TARGETS galay-rpc
    EXPORT galay-rpc-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY galay-rpc/
    DESTINATION include/galay-rpc
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

install(EXPORT galay-rpc-targets
    FILE galay-rpc-targets.cmake
    NAMESPACE galay-rpc::
    DESTINATION lib/cmake/galay-rpc
)

# 配置文件
include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/galay-rpc-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/galay-rpc-config.cmake"
    INSTALL_DESTINATION lib/cmake/galay-rpc
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/galay-rpc-config.cmake"
    DESTINATION lib/cmake/galay-rpc
)
