set(TARGET_NAME TurboCommon)
find_package(Stb REQUIRED)

# Add parser subdirectory first (generates lexer)
add_subdirectory(parser)

# Common/base utilities - contains stats, base64, DNS, mDNS, filesystem, logger,
# platform, and client utilities Find all files recursively
file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
# Public alias
add_library(TurboNet::Common ALIAS ${TARGET_NAME})

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/common>
         $<BUILD_INTERFACE:${Stb_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include>
         $<INSTALL_INTERFACE:include/common>
  PRIVATE $<BUILD_INTERFACE:${LOG_PATTERN_LEXER_INCLUDE_DIRS}>)

target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)
# Link dependencies
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC libuv::uv
  PRIVATE c-ares::cares # DNS resolver uses c-ares
          aklomp::base64 # Base64 utilities
          PkgConfig::STC # Thread-safe containers
          ${LOG_PATTERN_LEXER_LIBRARY} # Log pattern lexer
)

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF
             FOLDER "common"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Common)
if(ENABLE_ASAN)
  add_sanitizers(${TARGET_NAME})
endif()

# Install library
install(
  TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers preserving directory structure
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/common.h
        DESTINATION include)
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/common
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h")

# Tests
add_subdirectory(tests)
add_subdirectory(examples)
