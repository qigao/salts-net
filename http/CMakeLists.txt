set(TARGET_NAME http_client)
find_package(llhttp CONFIG REQUIRED)
# Synchronous HTTP client library
add_library(http_client SHARED src/http_client.c src/stb_sprintf.c)
add_library(TurboNet::HttpClient ALIAS http_client)
target_compile_definitions(http_client PRIVATE SHARED_CXX)

target_include_directories(
  http_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/turbo>
  PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(
  http_client
  PUBLIC TurboNet::Core TurboNet::Common TurboNet::Parser
  PRIVATE llhttp::llhttp_static)

# Asynchronous HTTP client library
add_library(http_client_async SHARED src/http_client_async.c src/stb_sprintf.c)
add_library(TurboNet::HttpClient::Async ALIAS http_client_async)
target_compile_definitions(http_client_async PRIVATE SHARED_CXX)

target_include_directories(
  http_client_async
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${Stb_INCLUDE_DIR} ${LLHTTP_INCLUDE_DIR})

target_link_libraries(
  http_client_async
  PUBLIC TurboNet::Core TurboNet::Common TurboNet::Parser
  PRIVATE llhttp::llhttp_static)

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER http
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME HttpClient)
set_target_properties(
  http_client_async
  PROPERTIES FOLDER http
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME HttpClientAsync)
if(ENABLE_ASAN)
  add_sanitizers(http_client)
  add_sanitizers(http_client_async)
endif()
# Install
install(
  TARGETS http_client
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/http_client.h
        DESTINATION include)
# Examples subdirectory
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# Tests subdirectory
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
