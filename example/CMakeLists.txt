if(BUILD_EXAMPLES)
    # Echo示例 - 服务端
    add_executable(E1-EchoServer E1-EchoServer.cpp)
    target_link_libraries(E1-EchoServer galay-rpc)

    # Echo示例 - 客户端
    add_executable(E2-EchoClient E2-EchoClient.cpp)
    target_link_libraries(E2-EchoClient galay-rpc)
endif()
