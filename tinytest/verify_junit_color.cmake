if(NOT DEFINED TEST_EXE)
  message(FATAL_ERROR "TEST_EXE is required")
endif()

if(NOT DEFINED JUNIT_FILE)
  message(FATAL_ERROR "JUNIT_FILE is required")
endif()

file(REMOVE "${JUNIT_FILE}")

execute_process(
  COMMAND "${TEST_EXE}" --color --junit "${JUNIT_FILE}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(NOT rc EQUAL 1)
  message(FATAL_ERROR "color JUnit run should fail with rc=1, got ${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()

if(NOT EXISTS "${JUNIT_FILE}")
  message(FATAL_ERROR "JUnit file was not created: ${JUNIT_FILE}")
endif()

file(READ "${JUNIT_FILE}" xml)
string(REPLACE "\r" "" xml "${xml}")
string(REPLACE "\n" "" xml_oneline "${xml}")
string(ASCII 27 esc)

if(xml MATCHES "${esc}")
  message(FATAL_ERROR "JUnit XML still contains raw ESC bytes:\n${xml}")
endif()

if(xml_oneline MATCHES "\\[[0-9;]+m")
  message(FATAL_ERROR "JUnit XML still contains ANSI color payload:\n${xml}")
endif()

if(NOT xml_oneline MATCHES "<failure message=\"Check failed:")
  message(FATAL_ERROR "failure node missing from color JUnit XML:\n${xml}")
endif()
