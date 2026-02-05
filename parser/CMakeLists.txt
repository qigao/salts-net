message(STATUS "JSON Parser generation enabled")
message(STATUS "  re2c: ${RE2C_EXECUTABLE}")
message(STATUS "  lemon: ${LEMON_EXECUTABLE}")
message(STATUS "Parser module enabled")
# =============================================================================
# Submodules (must be added first so we can link to them)
# =============================================================================

add_subdirectory(internal/ini_parser)
add_subdirectory(internal/json_parser)
add_subdirectory(internal/csv_parser)
add_subdirectory(internal/uri_parser)
add_subdirectory(internal/tlv_parser)
add_subdirectory(internal/ltv_parser)
add_subdirectory(internal/soa_parser)
add_subdirectory(internal/dotenv_parser)
add_subdirectory(internal/cmd_parser)
add_subdirectory(internal/toon)
add_subdirectory(internal/toml)
add_subdirectory(turbo_parser)
 