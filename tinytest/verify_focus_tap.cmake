if(NOT DEFINED TEST_EXE)
  message(FATAL_ERROR "TEST_EXE is required")
endif()

execute_process(
  COMMAND "${TEST_EXE}" --tap
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "focus TAP run failed: ${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()

string(REPLACE "\r" "" out "${out}")

if(NOT out MATCHES "TAP version 13")
  message(FATAL_ERROR "missing TAP header:\n${out}")
endif()

if(NOT out MATCHES "1\\.\\.3")
  message(FATAL_ERROR "unexpected TAP plan:\n${out}")
endif()

if(NOT out MATCHES "ok [0-9]+ - explicit skip # SKIP Test was skipped")
  message(FATAL_ERROR "explicit skip line missing or malformed:\n${out}")
endif()

if(NOT out MATCHES "ok [0-9]+ - focused test")
  message(FATAL_ERROR "focused test line missing:\n${out}")
endif()

if(NOT out MATCHES "ok [0-9]+ - unfocused test # SKIP filtered by focus")
  message(FATAL_ERROR "focus-skipped line missing or malformed:\n${out}")
endif()

if(out MATCHES "(^|\\n)skipped [0-9]+ -")
  message(FATAL_ERROR "legacy non-TAP skip syntax still present:\n${out}")
endif()
