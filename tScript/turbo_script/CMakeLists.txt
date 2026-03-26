project(turbo_script)

set(TARGET_NAME turbo_script)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

# Collect sources
turbonet_collect_files(SOURCE_FILES RECURSE)

# ── STATIC lib for testing ────────────────────────────────────────
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

turbonet_configure_target(${TARGET_NAME_LIB}
  FOLDER turbo_script
  NO_INSTALL)

target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)

target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE exprtk datetime_parser mir_static  
  PUBLIC TurboNet::Utils)

# ── SHARED library ────────────────────────────────────────────────
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

turbonet_configure_target(${TARGET_NAME}
  ALIAS TurboNet::TurboScript
  FOLDER turbo_script
  EXPORT_NAME TurboScript)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)

target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

target_link_libraries(
  ${TARGET_NAME}
  PRIVATE  datetime_parser mir_static  
  PUBLIC TurboNet::Utils exprtk)

# Install headers
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_script.h
        DESTINATION include)

add_subdirectory(test)
add_subdirectory(app)
