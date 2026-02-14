if(BUILD_BENCHMARKS)
    # 压测服务端
    add_executable(B1-RpcBenchServer B1-RpcBenchServer.cpp)
    target_link_libraries(B1-RpcBenchServer galay-rpc)

    # 压测客户端
    add_executable(B2-RpcBenchClient B2-RpcBenchClient.cpp)
    target_link_libraries(B2-RpcBenchClient galay-rpc)

    # ServiceDiscovery 压测
    add_executable(B3-ServiceDiscoveryBench B3-ServiceDiscoveryBench.cpp)
    target_link_libraries(B3-ServiceDiscoveryBench galay-rpc)

    # Stream 压测服务端
    add_executable(B4-RpcStreamBenchServer B4-RpcStreamBenchServer.cpp)
    target_link_libraries(B4-RpcStreamBenchServer galay-rpc)

    # Stream 压测客户端
    add_executable(B5-RpcStreamBenchClient B5-RpcStreamBenchClient.cpp)
    target_link_libraries(B5-RpcStreamBenchClient galay-rpc)
endif()
