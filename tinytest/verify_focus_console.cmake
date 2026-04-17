if(NOT DEFINED TEST_EXE)
  message(FATAL_ERROR "TEST_EXE is required")
endif()

execute_process(
  COMMAND "${TEST_EXE}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "focus console run failed: ${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()

string(REPLACE "\r" "" out "${out}")

string(FIND "${out}" "  explicit skip\n  [ SKIP  ]" explicit_skip_pos)
if(explicit_skip_pos EQUAL -1)
  message(FATAL_ERROR "explicit skip entry is missing from console output:\n${out}")
endif()

string(FIND "${out}" "  focused test\n  [ OK    ]" focused_ok_pos)
if(focused_ok_pos EQUAL -1)
  message(FATAL_ERROR "focused test entry is missing from console output:\n${out}")
endif()

string(FIND "${out}" "  unfocused test\n  [ SKIP  ]" focus_skip_pos)
if(focus_skip_pos EQUAL -1)
  message(FATAL_ERROR "focus-skipped test entry is missing from console output:\n${out}")
endif()
