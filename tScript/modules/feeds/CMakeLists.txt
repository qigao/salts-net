# feeds_plugin — import("feeds")
set(TARGET_NAME feeds_plugin)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

add_library(${TARGET_NAME} SHARED ${SOURCE_FILES}) 

target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboNet::Parser)

target_include_directories(${TARGET_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_script/modules/feeds
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

add_subdirectory(tests)
