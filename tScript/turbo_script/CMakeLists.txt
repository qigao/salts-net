project(turbo_script)

set(TARGET_NAME turbo_script)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# ── STATIC lib for testing ────────────────────────────────────────
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)

target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE exprtk datetime_parser mir_static  
  PUBLIC TurboNet::Utils)

set_target_properties(
  ${TARGET_NAME_LIB}
  PROPERTIES FOLDER turbo_script
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# ── SHARED library ────────────────────────────────────────────────
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
add_library(TurboNet::TurboScript ALIAS ${TARGET_NAME})

 
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

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_script
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME TurboScript)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_script.h
        DESTINATION include)

add_subdirectory(test)
add_subdirectory(app)
