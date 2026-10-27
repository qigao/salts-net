# SNMP-specific MIB textual schema compiler. NOT an ASN.1 runtime/library.
# BER/DER and X.509 are exclusively provided by qigao/crypto.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(_mib_scan "${CMAKE_CURRENT_BINARY_DIR}/mib_scanner.c")
  set(_mib_parser "${CMAKE_CURRENT_BINARY_DIR}/parser.c")
  set(_mib_parser_h "${CMAKE_CURRENT_BINARY_DIR}/parser.h")

  add_custom_command(
    OUTPUT "${_mib_scan}"
    COMMAND "${RE2C_EXECUTABLE}" -o "${_mib_scan}"
            "${CMAKE_CURRENT_SOURCE_DIR}/parser/scanner.re"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/parser/scanner.re"
            "${RE2C_EXECUTABLE}"
    VERBATIM)

  if(NOT LEMPAR)
    set(LEMPAR "${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c")
  endif()
  add_custom_command(
    OUTPUT "${_mib_parser}" "${_mib_parser_h}"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/parser/parser.y"
            "${CMAKE_CURRENT_BINARY_DIR}/parser.y"
    COMMAND "${LEMON_EXECUTABLE}" "-T${LEMPAR}"
            "${CMAKE_CURRENT_BINARY_DIR}/parser.y"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/parser/parser.y"
            ${LEMON_DEPENDS}
    VERBATIM)

  add_executable(mib_compiler
    src/ast.c src/mib_compiler.c tools/mib_compiler_main.c
    "${_mib_scan}" "${_mib_parser}")
  target_include_directories(mib_compiler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/private"
    "${CMAKE_CURRENT_BINARY_DIR}")
  set_target_properties(mib_compiler PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    FOLDER snmp/mib)
  # This is a CLI only. It is NOT installed as a SaltsNet:: library.
  install(TARGETS mib_compiler EXPORT SaltsNetTargets RUNTIME DESTINATION bin)
  if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME saltsnet.mib_compiler.help
             COMMAND mib_compiler --help)
  endif()
endif()
