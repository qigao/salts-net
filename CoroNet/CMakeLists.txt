set(TARGET_NAME CoroNet)
# Add WebSocket parser subdirectory
add_subdirectory(parser)

set(TURBO_HAS_IO_URING OFF)

set(TURBO_IO_URING_MIN_KERNEL_VERSION 5.1)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND
   NOT CMAKE_SYSTEM_VERSION VERSION_LESS TURBO_IO_URING_MIN_KERNEL_VERSION)
  pkg_check_modules(TURBO_IO_URING QUIET IMPORTED_TARGET liburing)
  if(TURBO_IO_URING_FOUND)
    set(TURBO_HAS_IO_URING ON)
  endif()
endif()

if(TURBO_HAS_IO_URING)
  add_compile_definitions(TURBO_HAS_IO_URING=1)
endif()

# Platform-specific backend mapping
set(BACKEND_FILES_IOCP
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_iocp_pool.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_iocp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_win.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_iocp.c)

set(BACKEND_FILES_KQUEUE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_kqueue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_kqueue.c)

set(BACKEND_FILES_IO_URING
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_io_uring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_epoll.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_io_uring.c)

set(BACKEND_FILES_EPOLL
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_epoll.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_epoll.c)

set(BACKEND_FILES_UNIX_GENERIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c)

# Collect all generic sources, excluding all backend implementations
cmake_add_source(
  SOURCE_FILES
  RECURSE
  EXCLUDES
  ${BACKEND_FILES_IOCP}
  ${BACKEND_FILES_KQUEUE}
  ${BACKEND_FILES_IO_URING}
  ${BACKEND_FILES_EPOLL}
  ${BACKEND_FILES_UNIX_GENERIC})

# Select and add the appropriate backend
if(WIN32)
  list(APPEND SOURCE_FILES ${BACKEND_FILES_IOCP})
elseif(APPLE)
  list(APPEND SOURCE_FILES ${BACKEND_FILES_KQUEUE})
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
  if(TURBO_HAS_IO_URING AND NOT ANDROID)
    list(APPEND SOURCE_FILES ${BACKEND_FILES_IO_URING})
  else()
    list(APPEND SOURCE_FILES ${BACKEND_FILES_EPOLL})
  endif()
else()
  list(APPEND SOURCE_FILES ${BACKEND_FILES_UNIX_GENERIC})
endif()

if(WIN32)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
# Build STATIC library for internal use (tests)
set(STATIC_TARGET_NAME ${TARGET_NAME}_static)
add_library(${STATIC_TARGET_NAME} STATIC ${SOURCE_FILES})
# Build SHARED library
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

# Configure targets
cmake_config_target(${STATIC_TARGET_NAME} ALIAS TurboNet::CoroNet_static FOLDER
                    ${TARGET_NAME} NO_INSTALL)

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  TurboNet::CoroNet
  FOLDER
  ${TARGET_NAME}
  EXPORT_NAME
  CoroNet)
if(WIN32)
  target_compile_definitions(${STATIC_TARGET_NAME}
                             PUBLIC WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(${TARGET_NAME} PUBLIC WIN32_LEAN_AND_MEAN
                                                   _CRT_SECURE_NO_WARNINGS)
endif()
# Apply configuration to both
foreach(tgt ${TARGET_NAME} ${STATIC_TARGET_NAME})
  target_include_directories(
    ${tgt}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/CoroNet>
           $<INSTALL_INTERFACE:include>
           $<INSTALL_INTERFACE:include/CoroNet>
    PRIVATE ${WEBSOCKET_PARSER_INCLUDE_DIRS})

  target_link_libraries(
    ${tgt}
    PUBLIC TurboUtils::Core TurboUtils::Parser Threads::Threads
    PRIVATE kcp::kcp ${TURBONET_CARES_TARGET} OpenSSL::SSL OpenSSL::Crypto
            TurboNet::WebSocketParser reed)

  if(WIN32)
    target_link_libraries(${tgt} PRIVATE ws2_32 mswsock crypt32)
  endif()

  if(TURBO_HAS_IO_URING)
    target_link_libraries(${tgt} PRIVATE PkgConfig::TURBO_IO_URING)
  endif()

endforeach()

# Install
install(
  TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/CoroNet.h DESTINATION include)
cmake_install_headers(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/CoroNet EXCLUDES "internal.h"
  "turbo_coro_internal.h" "turbo_native_*_internal.h")

add_subdirectory(tests)
if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

add_subdirectory(examples)
