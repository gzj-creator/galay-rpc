if(BUILD_BENCHMARKS)
    # 压测服务端
    add_executable(b1_rpc b1_rpc.cpp)
    target_link_libraries(b1_rpc galay-rpc)

    # 压测客户端
    add_executable(b2_rpc b2_rpc.cpp)
    target_link_libraries(b2_rpc galay-rpc)

    # ServiceDiscovery 压测
    add_executable(b3_discovery b3_discovery.cpp)
    target_link_libraries(b3_discovery galay-rpc)

    # Stream 压测服务端
    add_executable(b4_stream b4_stream.cpp)
    target_link_libraries(b4_stream galay-rpc)

    # Stream 压测客户端
    add_executable(b5_stream b5_stream.cpp)
    target_link_libraries(b5_stream galay-rpc)
endif()
