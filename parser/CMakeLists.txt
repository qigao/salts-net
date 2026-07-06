message(STATUS "JSON Parser generation enabled")
message(STATUS "  re2c: ${RE2C_EXECUTABLE}")
message(STATUS "  lemon: ${LEMON_EXECUTABLE}")
message(STATUS "Parser module enabled")
# =============================================================================
# Submodules (must be added first so we can link to them)
# =============================================================================

add_subdirectory(ini_parser)
add_subdirectory(json_parser)
add_subdirectory(csv_parser)
add_subdirectory(uri_parser)
add_subdirectory(tlv_parser)
add_subdirectory(ltv_parser)
add_subdirectory(modbus_parser)
add_subdirectory(soa_parser)
add_subdirectory(dotenv_parser)
add_subdirectory(cmd_parser)
add_subdirectory(toon)
add_subdirectory(toml)
add_subdirectory(datetime_parser)
add_subdirectory(mime_parser)

