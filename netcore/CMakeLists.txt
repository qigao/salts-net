set(TARGET_NAME NetCore)
find_package(Stb REQUIRED)
find_package(libuv CONFIG REQUIRED)
# Add WebSocket parser subdirectory
add_subdirectory(parser)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# Build SHARED library (DLL)
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
add_library(TurboNet::Core ALIAS ${TARGET_NAME})

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             OUTPUT_NAME netcore)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/netcore>
         $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/netcore>
  PRIVATE ${Stb_INCLUDE_DIR} ${WEBSOCKET_PARSER_INCLUDE_DIRS})

target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

add_sanitizers(${TARGET_NAME})
# Link libraries
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC OpenSSL::SSL OpenSSL::Crypto TurboNet::Utils
         $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
         TurboNet::Parser
  PRIVATE kcp::kcp Threads::Threads llhttp::llhttp_static PkgConfig::STC
          TurboNet::WebSocketParser)

# Set target properties
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${TARGET_NAME}
                                                EXPORT_NAME Core)

# Build STATIC library for internal use (tests)
set(STATIC_TARGET_NAME ${TARGET_NAME}_static)
add_library(${STATIC_TARGET_NAME} STATIC ${SOURCE_FILES})
add_library(TurboNet::Core_static ALIAS ${STATIC_TARGET_NAME})

set_target_properties(${STATIC_TARGET_NAME} PROPERTIES OUTPUT_NAME netcore_static
                                                        FOLDER ${TARGET_NAME})

target_include_directories(
  ${STATIC_TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/netcore>
         $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/netcore>
  PRIVATE ${Stb_INCLUDE_DIR} ${WEBSOCKET_PARSER_INCLUDE_DIRS})

add_sanitizers(${STATIC_TARGET_NAME})

target_link_libraries(
  ${STATIC_TARGET_NAME}
  PUBLIC OpenSSL::SSL OpenSSL::Crypto TurboNet::Utils
         $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
         TurboNet::Parser
  PRIVATE kcp::kcp Threads::Threads llhttp::llhttp_static PkgConfig::STC
          TurboNet::WebSocketParser)

# Install library (for development/internal use)
install(
  TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers preserving directory structure
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/netcore.h DESTINATION include)
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/netcore
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h")

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_subdirectory(stdlibuv)

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
