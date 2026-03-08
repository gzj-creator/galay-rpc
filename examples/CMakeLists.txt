function(add_rpc_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-rpc)
    target_include_directories(${target_name} PRIVATE ${CMAKE_SOURCE_DIR})
endfunction()

function(add_rpc_import_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-rpc-modules)
    target_include_directories(${target_name} PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(${target_name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
endfunction()

if(BUILD_EXAMPLES)
    # include 版本
    add_rpc_example(E1-EchoServer include/E1-echo_server.cc)
    add_rpc_example(E2-EchoClient include/E2-echo_client.cc)
    add_rpc_example(E3-StreamServer include/E3-stream_server.cc)
    add_rpc_example(E4-StreamClient include/E4-stream_client.cc)

    # import 版本（按工具链支持自动开启）
    if(BUILD_MODULE_EXAMPLES AND TARGET galay-rpc-modules)
        add_rpc_import_example(E1-EchoServerImport import/E1-echo_server.cc)
        add_rpc_import_example(E2-EchoClientImport import/E2-echo_client.cc)
        add_rpc_import_example(E3-StreamServerImport import/E3-stream_server.cc)
        add_rpc_import_example(E4-StreamClientImport import/E4-stream_client.cc)
    endif()
endif()
