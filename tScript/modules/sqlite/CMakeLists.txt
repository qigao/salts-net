# sqlite_plugin — import("sqlite")
set(TARGET_NAME sqlite_plugin)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# Find SQLite3
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Static lib for tests
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})
target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE exprtk ts_loader unofficial::sqlite3::sqlite3
  PUBLIC TurboNet::Core)
set_target_properties(${TARGET_NAME_LIB} PROPERTIES FOLDER
                                                    turbo_script/modules/sqlite)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
target_link_libraries(
  ${TARGET_NAME}
  PRIVATE ts_loader exprtk unofficial::sqlite3::sqlite3
  PUBLIC TurboNet::Core)
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_script/modules/sqlite
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

add_subdirectory(tests)
