set(TARGET_NAME cmd_parser)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(CMD_ARGER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/cmd_arger.re)
set(CMD_ARGER_GEN ${CMAKE_CURRENT_BINARY_DIR}/cmd_arger_gen.c)

add_custom_command(
  OUTPUT ${CMD_ARGER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${CMD_ARGER_GEN} ${CMD_ARGER_RE}
  DEPENDS ${CMD_ARGER_RE}
  COMMENT "Generating cmd_arger with re2c"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

add_library(${TARGET_NAME} STATIC 
    ${CMD_ARGER_GEN}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cmd_arger.c
)
add_library(TurboNet::CmdParser ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${TARGET_NAME} PRIVATE TurboNet::DotEnvParser)

set_target_properties(
  ${TARGET_NAME} PROPERTIES FOLDER parser/${TARGET_NAME}
                            EXPORT_NAME CmdParser)

add_subdirectory(test)
