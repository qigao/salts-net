if(NOT DEFINED TEST_EXE)
  message(FATAL_ERROR "TEST_EXE is required")
endif()

if(NOT DEFINED JUNIT_FILE)
  message(FATAL_ERROR "JUNIT_FILE is required")
endif()

file(REMOVE "${JUNIT_FILE}")

get_filename_component(JUNIT_DIR "${JUNIT_FILE}" DIRECTORY)
execute_process(
  COMMAND "${TEST_EXE}" --junit "${JUNIT_DIR}" --filter keep
  RESULT_VARIABLE bad_rc
  OUTPUT_VARIABLE bad_out
  ERROR_VARIABLE bad_err)

if(NOT bad_rc EQUAL 1)
  message(FATAL_ERROR "unwritable JUnit path should fail with rc=1, got ${bad_rc}\nstdout:\n${bad_out}\nstderr:\n${bad_err}")
endif()

if(NOT bad_err MATCHES "could not open JUnit output file")
  message(FATAL_ERROR "unwritable JUnit error message missing:\nstdout:\n${bad_out}\nstderr:\n${bad_err}")
endif()

execute_process(
  COMMAND "${TEST_EXE}" --junit "${JUNIT_FILE}" --filter keep
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "filter JUnit run failed: ${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()

if(NOT EXISTS "${JUNIT_FILE}")
  message(FATAL_ERROR "JUnit file was not created: ${JUNIT_FILE}")
endif()

file(READ "${JUNIT_FILE}" xml)
string(REPLACE "\r" "" xml "${xml}")
string(REPLACE "\n" "" xml_oneline "${xml}")

if(NOT xml_oneline MATCHES "tests=\"3\"")
  message(FATAL_ERROR "expected 3 testcases in JUnit:\n${xml}")
endif()

if(NOT xml_oneline MATCHES "skipped=\"2\"")
  message(FATAL_ERROR "expected skipped count of 2 in JUnit:\n${xml}")
endif()

if(NOT xml_oneline MATCHES "name=\"explicit skip\"[^>]*>[ ]*<skipped message=\"Test was skipped\" */>")
  message(FATAL_ERROR "explicit skip testcase missing skipped node:\n${xml}")
endif()

if(NOT xml_oneline MATCHES "name=\"drop test\"[^>]*>[ ]*<skipped message=\"filtered out\" */>")
  message(FATAL_ERROR "filtered testcase missing skipped node:\n${xml}")
endif()

if(xml_oneline MATCHES "name=\"drop test\"[^>]*/>")
  message(FATAL_ERROR "filtered testcase was emitted as a passing self-closing testcase:\n${xml}")
endif()
