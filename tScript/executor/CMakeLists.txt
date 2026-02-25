set(TARGET_NAME executor)

file(GLOB EXECUTOR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${EXECUTOR_SOURCES})
add_library(TurboNet::Executor ALIAS ${TARGET_NAME})

target_include_directories(${TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${CMAKE_SOURCE_DIR}/tScript/fin/include    # for order_manager.h
    ${CMAKE_SOURCE_DIR}/tScript/exprtk/include # for exprtk_types.h
)

target_link_libraries(${TARGET_NAME} PRIVATE TurboNet::Utils)
# The loader needs to link against the fin module to resolve labels if they aren't header-only
# But usually the loader doesn't need to link against fin if it only uses the headers.
# However, if we want to ensure everything is resolved:
# target_link_libraries(${TARGET_NAME} PRIVATE fin)

set_target_properties(${TARGET_NAME} PROPERTIES
  FOLDER        turbo_script/executor
)
