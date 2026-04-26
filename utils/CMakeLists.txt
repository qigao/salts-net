set(TARGET_NAME turbo_utils)
# Add parser subdirectory first (generates lexer)
add_subdirectory(parser)
cmake_add_source(SOURCE_FILES RECURSE)

add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
# Configure target
cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboNet::Utils
  FOLDER "utils"
  EXPORT_NAME Utils)

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
if(UNIX)
  target_compile_definitions(${TARGET_NAME} PUBLIC _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
endif()

# Link dependencies
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC aklomp::base64 # Base64 utilities
  PRIVATE ${FMT_LEXER_LIBRARY} PkgConfig::STC # Thread-safe containers
          TurboNet::SDS # Simple Dynamic Strings
          ${LOG_PATTERN_LEXER_LIBRARY} # Log pattern lexer
)

if(WIN32)
  target_link_libraries(${TARGET_NAME} PRIVATE iphlpapi ws2_32)
endif()

# Install headers
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)

# Install extra parser header
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/parser/fmt_lexer.h
        DESTINATION include)

# Tests
add_subdirectory(tests)
if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
add_subdirectory(examples)
