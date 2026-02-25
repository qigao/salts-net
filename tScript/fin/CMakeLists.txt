set(TARGET_NAME fin)

file(GLOB FIN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${FIN_SOURCES})
add_library(TurboNet::Fin ALIAS ${TARGET_NAME})

# SIMDe support
find_path(SIMDE_INCLUDE_DIRS "simde/simde-common.h")
if(SIMDE_INCLUDE_DIRS)
  message(STATUS "Found SIMDe: ${SIMDE_INCLUDE_DIRS}")
else()
  message(FATAL_ERROR "SIMDe not found. Please install it (e.g., via vcpkg).")
endif()

find_package(unofficial-simsimd CONFIG REQUIRED)

target_include_directories(${TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src              # fin_internal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include # exprtk_module.h, exprtk_types.h, exprtk.h
    ${CMAKE_SOURCE_DIR}/shared/utils/include      # arena_buffer.h, turbo_str_view.h
    ${SIMDE_INCLUDE_DIRS}
)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk PRIVATE unofficial::simsimd::simsimd_shared)

set_target_properties(${TARGET_NAME} PROPERTIES
  FOLDER        turbo_script/fin
  VERSION       ${PROJECT_VERSION}
  SOVERSION     ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME   Fin
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(test_fin test/test_fin.c)
target_link_libraries(test_fin PRIVATE fin exprtk TurboNet::TinyTest unofficial::simsimd::simsimd_shared)
target_include_directories(test_fin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include
  ${CMAKE_SOURCE_DIR}/shared/utils/include
)
add_test(NAME test_fin COMMAND test_fin)
set_target_properties(test_fin PROPERTIES FOLDER turbo_script/fin)

add_executable(test_universe test/test_universe.c)
target_link_libraries(test_universe PRIVATE fin exprtk TurboNet::TinyTest unofficial::simsimd::simsimd_shared)
target_include_directories(test_universe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include
  ${CMAKE_SOURCE_DIR}/shared/utils/include
)
add_test(NAME test_universe COMMAND test_universe)
set_target_properties(test_universe PROPERTIES FOLDER turbo_script/fin)

# ---------------------------------------------------------------------------
# Mock Executor DLL (for loader testing)
# ---------------------------------------------------------------------------
add_library(mock_executor SHARED test/mock_executor.c)
target_include_directories(mock_executor PRIVATE 
  include
  ${CMAKE_SOURCE_DIR}/tScript/executor/include
  ${CMAKE_SOURCE_DIR}/tScript/exprtk/include
)
set_target_properties(mock_executor PROPERTIES FOLDER turbo_script/fin)
target_link_libraries(mock_executor PRIVATE  executor exprtk TurboNet::TinyTest TurboNet::Utils)

# Integration test: full event-driven backtest pipeline
add_executable(test_strategy test/test_strategy.c)
target_link_libraries(test_strategy PRIVATE fin executor exprtk TurboNet::TinyTest unofficial::simsimd::simsimd_shared)
target_include_directories(test_strategy PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include
  ${CMAKE_SOURCE_DIR}/shared/utils/include
  ${CMAKE_SOURCE_DIR}/tScript/executor/include
)

# On Windows, we need to make sure the DLL is in the same folder as the exe
if(WIN32)
  add_custom_command(TARGET test_strategy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:mock_executor>
    $<TARGET_FILE_DIR:test_strategy>
  )
endif()

add_test(NAME test_strategy COMMAND test_strategy)
set_target_properties(test_strategy PROPERTIES FOLDER turbo_script/fin)
