set(TARGET_NAME vec)
set(TARGET_NAME_PLUGIN vec_plugin)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# ── STATIC lib (for unit tests — direct linking) ─────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})
add_library(TurboNet::Vec ALIAS ${TARGET_NAME})

target_include_directories(${TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include
 )

target_link_libraries(${TARGET_NAME} PUBLIC exprtk PRIVATE ts_loader)

set_target_properties(${TARGET_NAME} PROPERTIES
  FOLDER        turbo_script/modules/vec
  VERSION       ${PROJECT_VERSION}
  SOVERSION     ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME   Vec)

# ── SHARED plugin DLL (loaded at runtime via ts_plugin_load) ─────
add_library(${TARGET_NAME_PLUGIN} SHARED ${SOURCE_FILES})

target_link_libraries(${TARGET_NAME_PLUGIN} PRIVATE vec ts_loader)

target_include_directories(${TARGET_NAME_PLUGIN}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include
    )

set_target_properties(${TARGET_NAME_PLUGIN} PROPERTIES
  FOLDER        turbo_script/modules/vec
  VERSION       ${PROJECT_VERSION}
  SOVERSION     ${PROJECT_VERSION_MAJOR})
