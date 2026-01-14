# Tests for QuickJS TurboNet modules
file(GLOB TEST_SOURCES
    js_uv_dns_test.c
    js_uv_fs_test.c
    js_uv_http_test.c
    js_uv_module_test.c
    js_uv_multi_runtime_test.c
    js_uv_net_test.c
    js_uv_os_test.c
    js_uv_process_test.c
    js_uv_signal_test.c
    js_uv_timers_test.c
    js_uv_utils_test.c
)
set(folder_name quick/test)

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE unity TurboNet::Quick)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_target_properties(${test_name} PROPERTIES FOLDER ${folder_name})
endforeach()