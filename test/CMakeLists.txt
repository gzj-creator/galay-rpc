if(BUILD_TESTING)
    # 测试结果写入器
    add_library(test_result_writer INTERFACE)
    target_include_directories(test_result_writer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

    # 协议测试
    add_executable(T1-RpcProtocolTest T1-rpc_protocol_test.cpp)
    target_link_libraries(T1-RpcProtocolTest galay-rpc)

    # 集成测试 - 服务端
    add_executable(T2-RpcServerTest T2-rpc_server_test.cpp)
    target_link_libraries(T2-RpcServerTest galay-rpc)

    # 集成测试 - 客户端
    add_executable(T3-RpcClientTest T3-rpc_client_test.cpp)
    target_link_libraries(T3-RpcClientTest galay-rpc)

    # 编译面测试 - Awaitable facade
    add_executable(T4-RpcAwaitableSurface T4-rpc_awaitable_surface.cpp)
    target_link_libraries(T4-RpcAwaitableSurface galay-rpc)

    # 运行时兼容测试 - 0=auto 的 IO scheduler 入口应仍可用
    add_executable(T5-RpcRuntimeIoSchedulerCompat T5-runtime_io_scheduler_compat.cpp)
    target_link_libraries(T5-RpcRuntimeIoSchedulerCompat galay-rpc)

    add_test(NAME RpcProtocolTest COMMAND T1-RpcProtocolTest)
    add_test(NAME RpcAwaitableSurfaceTest COMMAND T4-RpcAwaitableSurface)
    add_test(NAME RpcRuntimeIoSchedulerCompatTest COMMAND T5-RpcRuntimeIoSchedulerCompat)
endif()
