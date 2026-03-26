set(TARGET_NAME turbo_wasm)
set(TARGET_WASM3 turbo_wasm3_core)

set(WASM3_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_api_libc.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_api_tracer.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_bind.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_code.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_compile.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_core.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_env.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_exec.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_function.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_info.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_module.c
    ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/m3_parse.c)

add_library(${TARGET_WASM3} STATIC ${WASM3_CORE_SOURCES})
turbonet_configure_target(
  ${TARGET_WASM3}
  FOLDER "wasm/vendor")
target_include_directories(
  ${TARGET_WASM3}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/wasm3/source>
  PRIVATE ${CMAKE_SOURCE_DIR}/shared/utils/include)
target_link_libraries(${TARGET_WASM3} PRIVATE TurboNet::Utils)

add_library(
  ${TARGET_NAME} SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_wasm.h
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_wasm.c)
turbonet_configure_target(
  ${TARGET_NAME}
  ALIAS TurboNet::Wasm
  FOLDER "wasm"
  EXPORT_NAME Wasm)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/shared/utils/include>
         $<INSTALL_INTERFACE:include>
         $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/wasm3/source>
  PRIVATE ${CMAKE_SOURCE_DIR}/shared/utils/include)

target_link_libraries(${TARGET_NAME} PRIVATE ${TARGET_WASM3} TurboNet::Utils)
target_compile_definitions(${TARGET_NAME} PRIVATE TURBO_WASM_BUILD_SHARED)

turbonet_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
