if(BUILD_TESTS)
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

    add_test(NAME RpcProtocolTest COMMAND T1-RpcProtocolTest)
endif()
