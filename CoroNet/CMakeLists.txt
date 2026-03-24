set(TARGET_NAME CoroNet)
find_package(Stb REQUIRED)
# Add WebSocket parser subdirectory
add_subdirectory(parser)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)
list(APPEND SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_coro_socket_tls.c)

set(TURBO_HAS_IO_URING OFF)
set(TURBO_IO_URING_LIBRARY "")
set(TURBO_IO_URING_INCLUDE_DIR "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  find_path(TURBO_IO_URING_INCLUDE_DIR liburing.h)
  find_library(TURBO_IO_URING_LIBRARY NAMES uring liburing)
  if(TURBO_IO_URING_INCLUDE_DIR AND TURBO_IO_URING_LIBRARY)
    set(TURBO_HAS_IO_URING ON)
  endif()
endif()

# Strip out all platform-specific backend files from glob results
list(REMOVE_ITEM SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_iocp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_win.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_iocp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_io_uring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_io_uring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_epoll.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_kqueue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_epoll.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_kqueue.c)

if(WIN32)
  list(APPEND SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_iocp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_win.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_iocp.c)


elseif(APPLE) # macOS, iOS, tvOS, watchOS
  list(APPEND SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_kqueue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_kqueue.c)


elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
  if(TURBO_HAS_IO_URING AND NOT ANDROID)
    list(APPEND SOURCE_FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_io_uring.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_io_uring.c)
  else()
    list(APPEND SOURCE_FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_epoll.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_datagram_epoll.c)
  endif()


else()
  # Fallback for other Unix-like OSes
  list(APPEND SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_stream_pipe_unix.c)

endif()

if(WIN32)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
# Build STATIC library for internal use (tests)
set(STATIC_TARGET_NAME ${TARGET_NAME}_static)
add_library(${STATIC_TARGET_NAME} STATIC ${SOURCE_FILES})
add_library(TurboNet::CoroNet_static ALIAS ${STATIC_TARGET_NAME})
set_target_properties(${STATIC_TARGET_NAME} PROPERTIES OUTPUT_NAME coronet_static FOLDER ${TARGET_NAME})

# Build SHARED library
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)
add_library(TurboNet::CoroNet ALIAS ${TARGET_NAME})
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${TARGET_NAME} EXPORT_NAME CoroNet)

# Apply configuration to both
foreach(tgt ${TARGET_NAME} ${STATIC_TARGET_NAME})
  target_include_directories(
    ${tgt}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/CoroNet>
           $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/CoroNet>
    PRIVATE ${Stb_INCLUDE_DIR} ${WEBSOCKET_PARSER_INCLUDE_DIRS}
            ${TURBO_IO_URING_INCLUDE_DIR})

  add_sanitizers(${tgt})

  if(TURBO_HAS_IO_URING)
    target_compile_definitions(${tgt} PRIVATE TURBO_HAS_IO_URING=1)
  endif()

  target_link_libraries(
    ${tgt}
    PUBLIC OpenSSL::SSL OpenSSL::Crypto TurboNet::Utils TurboNet::Parser
           Threads::Threads c-ares::cares
    PRIVATE kcp::kcp llhttp::llhttp_static PkgConfig::STC TurboNet::SDS
            TurboNet::UriParser TurboNet::WebSocketParser)

  if(WIN32)
    target_link_libraries(${tgt} PRIVATE ws2_32 mswsock)
  endif()

  if(TURBO_HAS_IO_URING)
    target_link_libraries(${tgt} PRIVATE ${TURBO_IO_URING_LIBRARY})
  endif()
endforeach()
# Install library (for development/internal use)
install(
  TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers preserving directory structure
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/CoroNet.h DESTINATION include)
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/CoroNet
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "internal.h" EXCLUDE
  PATTERN "turbo_coro_internal.h" EXCLUDE
  PATTERN "turbo_native_*_internal.h" EXCLUDE)

add_subdirectory(tests)

add_subdirectory(examples)
