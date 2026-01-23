#ifndef CUNIT_TO_UNITY_H
#define CUNIT_TO_UNITY_H

#include "unity.h"
#include <string.h>

#define CU_ASSERT(cond) TEST_ASSERT(cond)
#define CU_ASSERT_FATAL(cond) TEST_ASSERT(cond)
#define CU_ASSERT_STRING_EQUAL(act, exp) TEST_ASSERT_EQUAL_STRING(exp, act)
#define CU_ASSERT_PTR_NOT_NULL(ptr) TEST_ASSERT_NOT_NULL(ptr)
#define CU_ASSERT_PTR_NULL(ptr) TEST_ASSERT_NULL(ptr)
#define CU_ASSERT_EQUAL(act, exp) TEST_ASSERT_EQUAL(exp, act)

// Registry and Suite Shims
typedef int CU_ErrorCode;
#define CUE_SUCCESS 0
#define CU_initialize_registry() CUE_SUCCESS
#define CU_cleanup_registry()
#define CU_basic_set_mode(x)
#define CU_BRM_VERBOSE 0
#define CU_basic_run_tests()
#define CU_basic_show_failures(x)
#define CU_get_failure_list() NULL
#define CU_get_number_of_tests_failed() 0

typedef void* CU_pSuite;
#define CU_add_suite(name, init, cleanup) (void*)1
#define CU_add_test(suite, name, func) func()

#endif
