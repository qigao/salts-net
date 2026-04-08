set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")

# =============================================================================
# Library
# =============================================================================
file(GLOB_RECURSE HEADER_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
# Build as SHARED library (DLL)
add_library(mustache SHARED ${HEADER_FILES} ${SOURCE_FILES})
add_library(TurboNet::Mustache ALIAS mustache)

target_compile_definitions(mustache PRIVATE SHARED_CXX)

target_include_directories(
  mustache
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/mustache>
  PRIVATE ${CMAKE_SOURCE_DIR}/parser/internal/json_parser/include
          ${CMAKE_SOURCE_DIR}/utils/include
          ${CMAKE_SOURCE_DIR}/vendor/cxml/include
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link to json_parser and common
target_link_libraries(
  mustache
  PRIVATE TurboNet::JsonParser cxml TurboNet::SDS
  PUBLIC TurboNet::Utils)

set_target_properties(
  mustache
  PROPERTIES FOLDER mustache
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Mustache)

# Example executable

add_executable(mustache_json_example examples/json_example.c)
target_link_libraries(mustache_json_example PRIVATE TurboNet::Mustache
                                                    TurboNet::JsonParser)
set_target_properties(mustache_json_example PROPERTIES FOLDER mustache/examples)

add_executable(mustache_xml_example examples/xml_example.c)
target_link_libraries(mustache_xml_example PRIVATE TurboNet::Mustache cxml)
set_target_properties(mustache_xml_example PROPERTIES FOLDER mustache/examples)

add_subdirectory(test)

# Install targets
install(
  TARGETS mustache
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(FILES include/mustache.h include/mustache_json.h include/mustache_xml.h
              DESTINATION include/mustache)
