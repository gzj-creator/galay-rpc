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
    add_rpc_example(e1_echo include/e1_echo.cc)
    add_rpc_example(e2_echo include/e2_echo.cc)
    add_rpc_example(e3_stream include/e3_stream.cc)
    add_rpc_example(e4_stream include/e4_stream.cc)

    # import 版本（按工具链支持自动开启）
    if(BUILD_MODULE_EXAMPLES AND TARGET galay-rpc-modules)
        add_rpc_import_example(e1_echo_import import/e1_echo.cc)
        add_rpc_import_example(e2_echo_import import/e2_echo.cc)
        add_rpc_import_example(e3_stream_import import/e3_stream.cc)
        add_rpc_import_example(e4_stream_import import/e4_stream.cc)
    endif()
endif()
