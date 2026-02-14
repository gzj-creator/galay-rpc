cmake_minimum_required(VERSION 3.16)
project(galay-rpc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 选项
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_BENCHMARKS "Build benchmark programs" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_MODULE_EXAMPLES "Build C++23 module(import/export) examples" ON)

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

# C++23 命名模块支持（需要 CMake 3.28+）
set(GALAY_RPC_MODULES_SUPPORTED FALSE)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(GALAY_RPC_MODULES_SUPPORTED TRUE)
endif()

if(BUILD_MODULE_EXAMPLES AND NOT GALAY_RPC_MODULES_SUPPORTED)
    message(FATAL_ERROR "BUILD_MODULE_EXAMPLES requires CMake >= 3.28")
endif()

set(GALAY_RPC_MODULES_GENERATOR_SUPPORTED FALSE)
if(CMAKE_GENERATOR MATCHES "Ninja" OR CMAKE_GENERATOR MATCHES "Visual Studio")
    set(GALAY_RPC_MODULES_GENERATOR_SUPPORTED TRUE)
endif()

if(BUILD_MODULE_EXAMPLES AND NOT GALAY_RPC_MODULES_GENERATOR_SUPPORTED)
    message(WARNING "Generator '${CMAKE_GENERATOR}' does not support C++ modules; disabling BUILD_MODULE_EXAMPLES.")
    set(BUILD_MODULE_EXAMPLES OFF)
endif()

if(BUILD_MODULE_EXAMPLES AND GALAY_RPC_MODULES_SUPPORTED AND GALAY_RPC_MODULES_GENERATOR_SUPPORTED)
    add_library(galay-rpc-modules STATIC)
    target_link_libraries(galay-rpc-modules PUBLIC galay-rpc)
    target_sources(galay-rpc-modules
        PUBLIC
            FILE_SET CXX_MODULES
            FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/galay-rpc/module/galay.rpc.cppm"
    )
endif()

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
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.cppm"
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
