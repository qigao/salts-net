# fin_plugin — import("fin")
set(TARGET_NAME fin)
set(TARGET_NAME_PLUGIN fin_plugin)

find_path(SIMDE_INCLUDE_DIRS "simde/simde-common.h")
if(SIMDE_INCLUDE_DIRS)
  message(STATUS "Found SIMDe: ${SIMDE_INCLUDE_DIRS}")
else()
  message(FATAL_ERROR "SIMDe not found. Please install it (e.g., via vcpkg).")
endif()

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# ── STATIC lib (for unit tests — direct linking) ─────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboNet::Utils)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_script/modules/fin
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Fin)

# ── SHARED plugin DLL (loaded at runtime via ts_plugin_load) ─────
add_library(${TARGET_NAME_PLUGIN} SHARED ${SOURCE_FILES})

target_link_libraries(${TARGET_NAME_PLUGIN} PRIVATE ${TARGET_NAME})

target_include_directories(${TARGET_NAME_PLUGIN}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES FOLDER turbo_script/modules/fin
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# ── Tests ─────────────────────────────────────────────────────────
add_subdirectory(test)
