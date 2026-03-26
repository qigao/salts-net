set(TARGET_NAME iris)
# Generate security validators from re2c
turbonet_add_grammar_target(SECURITY
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/security_validators.re)

# Generate route parser from re2c
turbonet_add_grammar_target(ROUTE
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/route_parser.re)

# Add generated files to sources
set(GENERATED_SOURCES ${SECURITY_LEXER_GEN} ${ROUTE_LEXER_GEN})

# Collect sources
turbonet_collect_files(SOURCE_FILES RECURSE)
list(APPEND SOURCE_FILES ${GENERATED_SOURCES})

add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

# Configure target
turbonet_configure_target(${TARGET_NAME}
  ALIAS TurboNet::Iris
  FOLDER "iris"
  EXPORT_NAME Iris)

# Link libraries first - vcpkg style
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC  OpenSSL::SSL OpenSSL::Crypto TurboNet::Utils TurboNet::CoroNet
  PRIVATE llhttp::llhttp_static
          kcp::kcp
          c-ares::cares
          TurboNet::Parser
          cjwt
          Threads::Threads)

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/CoroNet/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples/vendors)

# Platform-specific fixes
if(WIN32)
  target_compile_definitions(${TARGET_NAME} PUBLIC WIN32_LEAN_AND_MEAN
                                                   _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(${TARGET_NAME} PRIVATE ws2_32 advapi32)
endif()
target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Install headers
turbonet_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)
