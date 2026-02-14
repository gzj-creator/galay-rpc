if(BUILD_EXAMPLES)
    # Echo示例 - 服务端
    add_executable(E1-EchoServer E1-EchoServer.cpp)
    target_link_libraries(E1-EchoServer galay-rpc)

    # Echo示例 - 客户端
    add_executable(E2-EchoClient E2-EchoClient.cpp)
    target_link_libraries(E2-EchoClient galay-rpc)

    # Stream示例 - 服务端
    add_executable(E3-StreamServer E3-StreamServer.cpp)
    target_link_libraries(E3-StreamServer galay-rpc)

    # Stream示例 - 客户端
    add_executable(E4-StreamClient E4-StreamClient.cpp)
    target_link_libraries(E4-StreamClient galay-rpc)
endif()
