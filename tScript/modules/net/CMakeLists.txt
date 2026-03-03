set(TARGET_NAME net_plugin)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# Static lib for tests
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})
target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE exprtk ts_loader
  PUBLIC http_client)
set_target_properties(${TARGET_NAME_LIB} PROPERTIES FOLDER
                                                    turbo_script/modules/net)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
target_link_libraries(
  ${TARGET_NAME}
  PRIVATE ts_loader exprtk
  PUBLIC http_client)
target_include_directories(${TARGET_NAME} PRIVATE ${net_INCLUDES})
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_script/modules/net
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
add_subdirectory(test)
