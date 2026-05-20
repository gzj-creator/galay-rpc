if(BUILD_TESTING)
    # 测试结果写入器
    add_library(result_writer INTERFACE)
    target_include_directories(result_writer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

    # 协议测试
    add_executable(t1_proto t1_proto.cpp)
    target_link_libraries(t1_proto galay-rpc)

    # 集成测试 - 服务端
    add_executable(t2_server t2_server.cpp)
    target_link_libraries(t2_server galay-rpc)

    # 集成测试 - 客户端
    add_executable(t3_client t3_client.cpp)
    target_link_libraries(t3_client galay-rpc)

    # 编译面测试 - Awaitable facade
    add_executable(t4_await t4_await.cpp)
    target_link_libraries(t4_await galay-rpc)

    # 运行时兼容测试 - 0=auto 的 IO scheduler 入口应仍可用
    add_executable(t5_compat t5_compat.cpp)
    target_link_libraries(t5_compat galay-rpc)

    # RPC 库级日志入口测试
    add_executable(t6_rpc_log t6_rpc_log.cpp)
    target_link_libraries(t6_rpc_log galay-rpc)

    add_test(NAME RpcProtocolTest COMMAND t1_proto)
    add_test(NAME RpcAwaitableSurfaceTest COMMAND t4_await)
    add_test(NAME RpcRuntimeIoSchedulerCompatTest COMMAND t5_compat)
    add_test(NAME RpcLibraryLoggerTest COMMAND t6_rpc_log)
endif()
