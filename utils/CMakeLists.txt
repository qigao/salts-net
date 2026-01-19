set(TARGET_NAME turbo_utils)
find_package(Stb REQUIRED)

# Add parser subdirectory first (generates lexer)
add_subdirectory(parser)

# Common/base utilities - contains stats, base64, DNS, mDNS, filesystem, logger,
# platform, and client utilities Find all files recursively
file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*  )
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
# Public alias
add_library(TurboNet::Utils ALIAS ${TARGET_NAME})

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${Stb_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include>
  PRIVATE $<BUILD_INTERFACE:${LOG_PATTERN_LEXER_INCLUDE_DIRS}>
          $<BUILD_INTERFACE:${FMT_LEXER_INCLUDE_DIRS}>)

target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)
# Link dependencies
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC libuv::uv c-ares::cares # DNS resolver uses c-ares
         aklomp::base64 # Base64 utilities
  PRIVATE PkgConfig::STC # Thread-safe containers
          ${LOG_PATTERN_LEXER_LIBRARY} # Log pattern lexer
          ${FMT_LEXER_LIBRARY} # fmt_lexer

)

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER "utils"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Utils)

add_sanitizers(${TARGET_NAME})

# Install library
install(
  TARGETS ${TARGET_NAME} fmt_lexer log_pattern_lexer
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers preserving directory structure
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include
  DESTINATION  /
  FILES_MATCHING
  PATTERN "*.h")

# Tests
add_subdirectory(tests)
add_subdirectory(examples)
