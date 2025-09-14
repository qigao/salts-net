set(TARGET_NAME iris)

# Generate security validators from re2c
set(SECURITY_VALIDATORS_RE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/security_validators.re)
set(SECURITY_VALIDATORS_GEN
    ${CMAKE_CURRENT_BINARY_DIR}/security_validators_gen.c)

add_custom_command(
  OUTPUT ${SECURITY_VALIDATORS_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${SECURITY_VALIDATORS_GEN}
          ${SECURITY_VALIDATORS_RE}
  DEPENDS ${SECURITY_VALIDATORS_RE}
  COMMENT "Generating security validators with re2c"
  VERBATIM)

# Generate route parser from re2c
set(ROUTE_PARSER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/route_parser.re)
set(ROUTE_PARSER_GEN ${CMAKE_CURRENT_BINARY_DIR}/route_parser_gen.c)

add_custom_command(
  OUTPUT ${ROUTE_PARSER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${ROUTE_PARSER_GEN} ${ROUTE_PARSER_RE}
  DEPENDS ${ROUTE_PARSER_RE}
  COMMENT "Generating route parser with re2c"
  VERBATIM)

# Add generated files to sources
set(GENERATED_SOURCES ${SECURITY_VALIDATORS_GEN} ${ROUTE_PARSER_GEN})

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

# Remove old server.c (replaced by server_refactored.c)
list(FILTER SOURCE_FILES EXCLUDE REGEX ".*server\\.c$")

# Add generated sources
list(APPEND SOURCE_FILES ${GENERATED_SOURCES})

add_library(${TARGET_NAME} ${SOURCE_FILES})
add_library(TurboNet::Iris ALIAS ${TARGET_NAME})

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER "iris"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# Link libraries first - vcpkg style
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC libuv::uv OpenSSL::SSL OpenSSL::Crypto TurboNet::Common
  PRIVATE llhttp::llhttp_static
          cjson
          kcp::kcp
          c-ares::cares
          TurboNet::Parser
          TurboNet::Core
          Threads::Threads)

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/netcore/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples/vendors)

# Platform-specific fixes
if(WIN32)
  # Windows-specific compile definitions
  target_compile_definitions(${TARGET_NAME} PUBLIC WIN32_LEAN_AND_MEAN
                                                   _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(${TARGET_NAME} PRIVATE ws2_32 advapi32)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# Tests subdirectory
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
