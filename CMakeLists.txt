cmake_minimum_required(VERSION 3.16)
project(galay-rpc VERSION 1.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 构建选项 / 平台选项
include(CTest)
include(cmake/option.cmake)

# 配置 IO 后端
configure_io_backend()

# 优先使用 sibling 仓库下的本地安装前缀，避免误命中 /usr/local 旧包。
set(GALAY_RPC_SIBLING_KERNEL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../galay-kernel/install-local")
set(GALAY_RPC_SIBLING_KERNEL_CONFIG_DIR "${GALAY_RPC_SIBLING_KERNEL_PREFIX}/lib/cmake/galay-kernel")
if(EXISTS "${GALAY_RPC_SIBLING_KERNEL_CONFIG_DIR}/galay-kernel-config.cmake")
    list(PREPEND CMAKE_PREFIX_PATH "${GALAY_RPC_SIBLING_KERNEL_PREFIX}")
    if(NOT DEFINED galay-kernel_DIR OR galay-kernel_DIR STREQUAL "")
        set(galay-kernel_DIR "${GALAY_RPC_SIBLING_KERNEL_CONFIG_DIR}" CACHE PATH
            "Preferred galay-kernel package config directory" FORCE)
    endif()
endif()

# 查找依赖
find_package(galay-kernel 3.4.4 CONFIG REQUIRED)

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

set(GALAY_RPC_MODULE_COMPILE_OPTIONS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND GALAY_RPC_MODULE_COMPILE_OPTIONS -Xclang -fskip-odr-check-in-gmf)
    if(APPLE)
        if(CMAKE_OSX_SYSROOT)
            list(APPEND GALAY_RPC_MODULE_COMPILE_OPTIONS -isysroot "${CMAKE_OSX_SYSROOT}")
        else()
            execute_process(
                COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE GALAY_RPC_MACOS_SDK_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(GALAY_RPC_MACOS_SDK_PATH)
                list(APPEND GALAY_RPC_MODULE_COMPILE_OPTIONS -isysroot "${GALAY_RPC_MACOS_SDK_PATH}")
            endif()
        endif()
    endif()
endif()

if(BUILD_MODULE_EXAMPLES AND GALAY_RPC_MODULES_SUPPORTED AND GALAY_RPC_MODULES_GENERATOR_SUPPORTED)
    add_library(galay-rpc-modules STATIC)
    target_link_libraries(galay-rpc-modules PUBLIC galay-rpc)
    target_compile_features(galay-rpc-modules PUBLIC cxx_std_23)
    if(GALAY_RPC_MODULE_COMPILE_OPTIONS)
        target_compile_options(galay-rpc-modules PRIVATE ${GALAY_RPC_MODULE_COMPILE_OPTIONS})
    endif()
    target_sources(galay-rpc-modules
        PUBLIC
            FILE_SET CXX_MODULES
            FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/galay-rpc/module/galay.rpc.cppm"
    )
endif()

# 测试
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# 压测
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

# 示例
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 安装
install(TARGETS galay-rpc
    EXPORT galay-rpc-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

if(TARGET galay-rpc-modules AND GALAY_RPC_INSTALL_MODULE_INTERFACE)
    install(TARGETS galay-rpc-modules
        EXPORT galay-rpc-targets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        FILE_SET CXX_MODULES DESTINATION include/galay-rpc/module
    )
endif()

if(TARGET galay-rpc-modules AND GALAY_RPC_INSTALL_MODULE_INTERFACE)
    install(DIRECTORY galay-rpc/
        DESTINATION include/galay-rpc
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )
else()
    install(DIRECTORY galay-rpc/
        DESTINATION include/galay-rpc
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.cppm"
    )
endif()

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
