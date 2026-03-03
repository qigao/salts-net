set(TARGET_NAME simple_wasm_app)

add_executable(${TARGET_NAME} simple_wasm_app.c)
target_link_libraries(${TARGET_NAME} PRIVATE TurboNet::Wasm)
target_include_directories(
  ${TARGET_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/extra)

set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "wasm/examples")

set(TARGET_NAME_BENCH wasm_bench_app)
add_executable(${TARGET_NAME_BENCH} wasm_bench_app.c)
target_link_libraries(${TARGET_NAME_BENCH} PRIVATE TurboNet::Wasm
                                                   TurboNet::Utils
                                                   TurboNet::TinyTest)
target_include_directories(
  ${TARGET_NAME_BENCH} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/extra
                               ${CMAKE_SOURCE_DIR}/shared/utils/include)
set_target_properties(${TARGET_NAME_BENCH} PROPERTIES FOLDER "wasm/examples")

if(BUILD_TESTS)
  add_test(NAME wasm_bench_regression COMMAND $<TARGET_FILE:wasm_bench_app>)
endif()