set(TARGET_NAME asn1_parser)

# Find OpenSSL for Ed25519 key generation and signing
find_package(OpenSSL REQUIRED)

# =============================================================================
# re2c Generated Files
# =============================================================================

# Schema scanner (for MIB files)
set(ASN1_SCANNER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/scanner.re)
set(ASN1_SCANNER_GEN ${CMAKE_CURRENT_BINARY_DIR}/scanner_gen.c)

add_custom_command(
  OUTPUT ${ASN1_SCANNER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${ASN1_SCANNER_GEN} ${ASN1_SCANNER_RE}
  DEPENDS ${ASN1_SCANNER_RE} "${RE2C_EXECUTABLE}"
  COMMENT "Generating ASN.1 schema scanner with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

# Schema parser (for MIB files)
set(ASN1_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/parser.y)
set(ASN1_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/parser.c)
set(ASN1_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/parser.h)

if(NOT LEMPAR)
  set(LEMPAR ${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c)
endif()

add_custom_command(
  OUTPUT ${ASN1_GRAMMAR_GEN} ${ASN1_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${ASN1_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/parser.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR} ${CMAKE_CURRENT_BINARY_DIR}/parser.y
  DEPENDS ${ASN1_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating ASN.1 schema parser with lemon"
  VERBATIM)

# Binary parser (for DER/BER data)
set(ASN1_BINARY_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/binary_parser.y)
set(ASN1_BINARY_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/binary_parser.c)
set(ASN1_BINARY_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/binary_parser.h)

add_custom_command(
  OUTPUT ${ASN1_BINARY_GRAMMAR_GEN} ${ASN1_BINARY_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${ASN1_BINARY_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/binary_parser.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/binary_parser.y
  DEPENDS ${ASN1_BINARY_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating ASN.1 binary parser with lemon"
  VERBATIM)

# =============================================================================
# Library (respects BUILD_SHARED_LIBS)
# =============================================================================

file(
  GLOB_RECURSE
  SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/asn1_runtime.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/asn1_encoder.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ast.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mib_compiler.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/asn1_der_compat.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/binary_scanner.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/crypto_random.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/sha256.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/x509_cert.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/x509_generate.c
  ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)

add_library(
  ${TARGET_NAME} SHARED ${SOURCE_FILES} ${ASN1_SCANNER_GEN} ${ASN1_GRAMMAR_GEN}
                        ${ASN1_BINARY_GRAMMAR_GEN})

add_library(SaltsNet::Asn1Parser ALIAS ${TARGET_NAME})
cmake_config_api(${TARGET_NAME} SALTSNET_ASN1_C_API)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/asn1>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(${TARGET_NAME} PUBLIC Salts::Core OpenSSL::Crypto)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER snmp/asn1
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Asn1Parser)

# =============================================================================
# MIB Compiler Executable
# =============================================================================

add_executable(mib_compiler
               ${CMAKE_CURRENT_SOURCE_DIR}/tools/mib_compiler_main.c)

target_link_libraries(mib_compiler PRIVATE ${TARGET_NAME})
target_include_directories(mib_compiler
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(mib_compiler PROPERTIES FOLDER snmp/asn1/tools)

# Install
install(
  TARGETS ${TARGET_NAME} mib_compiler
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include/
  FILES_MATCHING
  PATTERN "*.h")

# =============================================================================
# Tests
# =============================================================================

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
