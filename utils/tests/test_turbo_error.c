#include "turbo_error.h"
#include "tinytest.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define TEST_ERROR_DOMAIN 42
#define TEST_ERROR_AUTH TURBO_ERROR_CUSTOM(TEST_ERROR_DOMAIN, 1)
#define TEST_ERROR_PARSE TURBO_ERROR_CUSTOM(TEST_ERROR_DOMAIN, 2)

static const turbo_error_entry_t test_errors[] = {
    {TEST_ERROR_AUTH, "TEST_EAUTH", "test authentication failed"},
    {TEST_ERROR_PARSE, "TEST_EPARSE", "test parse failed"},
};

static const turbo_error_domain_desc_t test_domain = {
    TEST_ERROR_DOMAIN,
    "test",
    test_errors,
    sizeof(test_errors) / sizeof(test_errors[0]),
};

suite("Turbo Error") {
  group("Turbo codes") {
    it("describes common and protocol errors") {
      check_str_eq(turbo_strerror(TURBO_OK), "success");
      check_str_eq(turbo_strerror(TURBO_EINVAL), "invalid argument");
      check_str_eq(turbo_strerror(TURBO_EPROTO), "protocol error");
      check_str_eq(turbo_strerror(TURBO_EAI_FAMILY), "address family not supported by DNS result");
    }

    it("returns structured metadata") {
      turbo_error_info_t info = turbo_error_info(TURBO_ENOMEM);

      check_int_eq(info.code, TURBO_ENOMEM);
      check_int_eq(info.domain, TURBO_ERROR_DOMAIN_TURBO);
      check_str_eq(info.name, "TURBO_ENOMEM");
      check_str_eq(info.message, "not enough memory");
    }
  }

  group("Native codes") {
    it("describes negative errno values") {
      turbo_error_info_t info = turbo_error_info(-ENOENT);

      check_int_eq(info.domain, TURBO_ERROR_DOMAIN_POSIX);
      check_str_eq(info.name, "POSIX");
      check_not_null(info.message);
      check(strstr(info.message, "unknown") == NULL && strstr(info.message, "Unknown") == NULL);
    }

#ifdef _WIN32
    it("describes negative Win32 values") {
      turbo_error_info_t info = turbo_error_info(-(int)ERROR_PIPE_BUSY);

      check_int_eq(info.domain, TURBO_ERROR_DOMAIN_WIN32);
      check_str_eq(info.name, "WIN32");
      check_not_null(info.message);
      check(strstr(info.message, "unknown") == NULL && strstr(info.message, "Unknown") == NULL);
    }
#endif
  }

  group("Result wrapper") {
    it("wraps success and errors") {
      turbo_result_t ok = turbo_result_from_code(TURBO_OK);
      turbo_result_t err = turbo_result_from_code(TURBO_EPERM);

      check_true(turbo_result_is_ok(ok));
      check_false(turbo_result_is_err(ok));
      check_int_eq(ok.code, TURBO_OK);
      check_str_eq(ok.message, "success");

      check_false(turbo_result_is_ok(err));
      check_true(turbo_result_is_err(err));
      check_int_eq(err.code, TURBO_EPERM);
      check_str_eq(err.message, "operation not permitted");
    }
  }

  group("Custom domains") {
    it("registers and resolves custom module errors") {
      turbo_error_info_t info;

      (void)turbo_error_unregister_domain(TEST_ERROR_DOMAIN);
      check_int_eq(turbo_error_register_domain(&test_domain), TURBO_OK);

      info = turbo_error_info(TEST_ERROR_AUTH);
      check_int_eq(info.code, TEST_ERROR_AUTH);
      check_int_eq(info.domain, TURBO_ERROR_DOMAIN_CUSTOM);
      check_int_eq(info.custom_domain, TEST_ERROR_DOMAIN);
      check_str_eq(info.domain_name, "test");
      check_str_eq(info.name, "TEST_EAUTH");
      check_str_eq(info.message, "test authentication failed");
      check_str_eq(turbo_strerror(TEST_ERROR_PARSE), "test parse failed");

      check_int_eq(turbo_error_register_domain(&test_domain), TURBO_EALREADY);
      check_int_eq(turbo_error_unregister_domain(TEST_ERROR_DOMAIN), TURBO_OK);
      check_int_eq(turbo_error_info(TEST_ERROR_AUTH).domain, TURBO_ERROR_DOMAIN_UNKNOWN);
    }

    it("rejects invalid custom domain descriptors") {
      static const turbo_error_entry_t bad_errors[] = {
          {TURBO_ERROR_CUSTOM(TEST_ERROR_DOMAIN + 1, 1), "BAD", "bad domain"},
      };
      static const turbo_error_domain_desc_t bad_domain = {
          TEST_ERROR_DOMAIN,
          "bad",
          bad_errors,
          sizeof(bad_errors) / sizeof(bad_errors[0]),
      };

      check_int_eq(turbo_error_register_domain(NULL), TURBO_EINVAL);
      check_int_eq(turbo_error_register_domain(&bad_domain), TURBO_EINVAL);
      check_int_eq(turbo_error_unregister_domain(TEST_ERROR_DOMAIN), TURBO_ENOENT);
    }
  }
}
