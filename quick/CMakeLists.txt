find_package(qjs REQUIRED)

set(TARGET_NAME turbo_quick)
file(GLOB_RECURSE WORK_FLOW_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} SHARED ${WORK_FLOW_SOURCES})
add_library(TurboNet::Quick ALIAS ${TARGET_NAME})

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
         $<INSTALL_INTERFACE:include/turbo/quick>)

# Link dependencies
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC qjs TurboNet::HttpClient TurboNet::Core TurboNet::Utils libuv::uv
  PRIVATE TurboNet::UriParser TurboNet::JsonParser)

target_compile_definitions(${TARGET_NAME}
  PUBLIC $<$<BOOL:WIN32>:NOMINMAX>
  PRIVATE SHARED_CXX)

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER "quick"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Quick)

# Install library
install(
  TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include/turbo/quick
  FILES_MATCHING
  PATTERN "*.h")

add_subdirectory(examples)
add_subdirectory(test)
