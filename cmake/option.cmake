# galay-rpc 构建选项配置

# 设置默认安装目录为/usr/local
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX /usr/local CACHE PATH "Install path prefix" FORCE)
endif()

# 构建目标选项
# BUILD_TESTS 兼容别名：向 BUILD_TESTING 迁移
if(DEFINED BUILD_TESTS)
    message(DEPRECATION "BUILD_TESTS is deprecated; use BUILD_TESTING instead.")
    set(BUILD_TESTING "${BUILD_TESTS}" CACHE BOOL "Build test executables" FORCE)
endif()
set(BUILD_TESTS "${BUILD_TESTING}" CACHE BOOL "Deprecated alias of BUILD_TESTING" FORCE)
mark_as_advanced(BUILD_TESTS)

option(BUILD_BENCHMARKS "Build benchmark executables" ON)
option(BUILD_EXAMPLES "Build example executables" ON)
option(BUILD_MODULE_EXAMPLES "Build C++23 module(import/export) examples" ON)
option(GALAY_RPC_INSTALL_MODULE_INTERFACE "Install C++ module interface files (*.cppm)" ON)

# 库类型选项（CMake 标准变量）
option(BUILD_SHARED_LIBS "Build shared library instead of static" ON)

# IO后端选项
option(DISABLE_IOURING "Disable io_uring and use epoll on Linux" ON)

# 平台特定的异步IO后端检测
function(configure_io_backend)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(DISABLE_IOURING)
            message(STATUS "io_uring disabled by user, using epoll")
            add_compile_definitions(USE_EPOLL)
            set(GALAY_RPC_BACKEND "epoll" PARENT_SCOPE)
        else()
            find_library(URING_LIB uring)
            if(URING_LIB)
                message(STATUS "Found liburing: ${URING_LIB}, using io_uring")
                add_compile_definitions(USE_IOURING)
                set(GALAY_RPC_BACKEND "io_uring" PARENT_SCOPE)
                set(PLATFORM_LIBS ${URING_LIB} PARENT_SCOPE)
            else()
                message(STATUS "liburing not found, falling back to epoll")
                add_compile_definitions(USE_EPOLL)
                set(GALAY_RPC_BACKEND "epoll" PARENT_SCOPE)
            endif()
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(STATUS "macOS detected, using kqueue")
        add_compile_definitions(USE_KQUEUE)
        set(GALAY_RPC_BACKEND "kqueue" PARENT_SCOPE)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        message(STATUS "Windows detected, using IOCP")
        add_compile_definitions(USE_IOCP)
        set(GALAY_RPC_BACKEND "iocp" PARENT_SCOPE)
    else()
        message(WARNING "Unknown platform: ${CMAKE_SYSTEM_NAME}")
        set(GALAY_RPC_BACKEND "unknown" PARENT_SCOPE)
    endif()
endfunction()
