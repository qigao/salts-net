set(TARGET_NAME turbo_utils)
find_package(Stb REQUIRED)
find_package(libuv CONFIG REQUIRED)
# Add parser subdirectory first (generates lexer)
add_subdirectory(parser)

# Common/base utilities - contains stats, base64, DNS, mDNS, filesystem, logger,
# platform, and client utilities Find all files recursively
file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# Platform-specific async I/O
if(WIN32)
  # Windows: use IOCP for high-performance I/O
  list(FILTER SOURCE_FILES EXCLUDE REGEX ".*turbo_aio_uv\\.c$")
  list(FILTER SOURCE_FILES EXCLUDE REGEX ".*turbo_aio_uring\\.c$")
elseif(APPLE OR ANDROID)
  # macOS/iOS/Android: use libuv thread pool
  list(FILTER SOURCE_FILES EXCLUDE REGEX ".*turbo_aio_iocp\\.c$")
  list(FILTER SOURCE_FILES EXCLUDE REGEX ".*turbo_aio_uring\\.c$")
else()
  # Linux: use io_uring for true kernel-level async
  list(FILTER SOURCE_FILES EXCLUDE REGEX ".*turbo_aio_iocp\\.c$")
  list(FILTER SOURCE_FILES EXCLUDE REGEX ".*turbo_aio_uv\\.c$")
endif()

add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
# Public alias
add_library(TurboNet::Utils ALIAS ${TARGET_NAME})

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${Stb_INCLUDE_DIR}>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
         $<INSTALL_INTERFACE:include>
  PRIVATE $<BUILD_INTERFACE:${FMT_LEXER_INCLUDE_DIRS}>
          $<BUILD_INTERFACE:${LOG_PATTERN_LEXER_INCLUDE_DIRS}>)

target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

# Link dependencies
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC c-ares::cares # DNS resolver uses c-ares
         aklomp::base64 # Base64 utilities
         $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
  PRIVATE ${FMT_LEXER_LIBRARY} PkgConfig::STC # Thread-safe containers
          TurboNet::SDS # Simple Dynamic Strings
          uri_parser ${LOG_PATTERN_LEXER_LIBRARY} # Log pattern lexer
)

# Linux: link liburing for io_uring async I/O
if(NOT WIN32
   AND NOT APPLE
   AND NOT ANDROID)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(URING REQUIRED liburing)
  target_include_directories(${TARGET_NAME} PRIVATE ${URING_INCLUDE_DIRS})
  target_link_libraries(${TARGET_NAME} PRIVATE ${URING_LIBRARIES})
endif()

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER "shared/utils"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Utils)

add_sanitizers(${TARGET_NAME})

# Install library
install(
  TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers preserving directory structure
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h")

# Install parser headers (fmt_lexer.h is needed by fmt.h)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/parser/fmt_lexer.h
        DESTINATION include)

# Tests
add_subdirectory(tests)
add_subdirectory(examples)
