#ifndef TURBO_ERROR_H
#define TURBO_ERROR_H

#include "platform.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_OK 0

typedef enum {
  TURBO_ERROR_DOMAIN_NONE = 0,
  TURBO_ERROR_DOMAIN_TURBO,
  TURBO_ERROR_DOMAIN_CUSTOM,
  TURBO_ERROR_DOMAIN_POSIX,
  TURBO_ERROR_DOMAIN_WIN32,
  TURBO_ERROR_DOMAIN_UNKNOWN
} turbo_error_domain_t;

#define TURBO_ERROR_CUSTOM_DOMAIN_MIN 1
#define TURBO_ERROR_CUSTOM_DOMAIN_MAX 32767
#define TURBO_ERROR_CUSTOM_LOCAL_MAX 65535
#define TURBO_ERROR_CUSTOM(domain, local)                                                           \
  (-(int)(((((domain) & 0x7fff) << 16) | ((local) & 0xffff))))
#define TURBO_ERROR_CUSTOM_DOMAIN(code) (((-(code)) >> 16) & 0x7fff)
#define TURBO_ERROR_CUSTOM_LOCAL(code) ((-(code)) & 0xffff)

// Mapping EAI codes
#define TURBO_EAI_ADDRFAMILY -3000
#define TURBO_EAI_AGAIN -3001
#define TURBO_EAI_BADFLAGS -3002
#define TURBO_EAI_FAIL -3003
#define TURBO_EAI_FAMILY -3004
#define TURBO_EAI_MEMORY -3005
#define TURBO_EAI_NODATA -3006
#define TURBO_EAI_NONAME -3007
#define TURBO_EAI_SERVICE -3008
#define TURBO_EAI_SOCKTYPE -3009
#define TURBO_EAI_SYSTEM -3010
#define TURBO_EAI_CANCELED -3011

// Common networking error codes
#define TURBO_EADDRINUSE -4000
#define TURBO_EADDRNOTAVAIL -4001
#define TURBO_EAFNOSUPPORT -4002
#define TURBO_EALREADY -4003
#define TURBO_EBADF -4004
#define TURBO_EBUSY -4005
#define TURBO_ECANCELED -4006
#define TURBO_ECHARSET -4007
#define TURBO_ECONNABORTED -4008
#define TURBO_ECONNREFUSED -4009
#define TURBO_ECONNRESET -4010
#define TURBO_EDESTADDRREQ -4011
#define TURBO_EFAULT -4012
#define TURBO_EFBIG -4013
#define TURBO_EHOSTUNREACH -4014
#define TURBO_EINTR -4015
#define TURBO_EINVAL -4016
#define TURBO_EIO -4017
#define TURBO_EISCONN -4018
#define TURBO_EISDIR -4019
#define TURBO_ELOOP -4020
#define TURBO_EMFILE -4021
#define TURBO_EMSGSIZE -4022
#define TURBO_ENAMETOOLONG -4023
#define TURBO_ENETDOWN -4024
#define TURBO_ENETUNREACH -4025
#define TURBO_ENFILE -4026
#define TURBO_ENOBUFS -4027
#define TURBO_ENODEV -4028
#define TURBO_ENOENT -4029
#define TURBO_ENOMEM -4030
#define TURBO_ENONET -4031
#define TURBO_ENOPROTOOPT -4032
#define TURBO_ENOSPC -4033
#define TURBO_ENOSYS -4034
#define TURBO_ENOTCONN -4035
#define TURBO_ENOTDIR -4036
#define TURBO_ENOTEMPTY -4037
#define TURBO_ENOTSOCK -4038
#define TURBO_ENOTSUP -4039
#define TURBO_EPERM -4040
#define TURBO_EPIPE -4041
#define TURBO_EPROTO -4042
#define TURBO_EPROTONOSUPPORT -4043
#define TURBO_EPROTOTYPE -4044
#define TURBO_ERANGE -4045
#define TURBO_EROFS -4046
#define TURBO_ESHUTDOWN -4047
#define TURBO_ESPIPE -4048
#define TURBO_ESRCH -4049
#define TURBO_ETIMEDOUT -4050
#define TURBO_ETXTBSY -4051
#define TURBO_EXDEV -4052
#define TURBO_UNKNOWN -4053
#define TURBO_EOF -4095

typedef struct {
  int code;
  int custom_domain;
  turbo_error_domain_t domain;
  const char *domain_name;
  const char *name;
  const char *message;
} turbo_error_info_t;

typedef struct {
  int code;
  const char *name;
  const char *message;
} turbo_error_entry_t;

typedef struct {
  int domain;
  const char *domain_name;
  const turbo_error_entry_t *entries;
  size_t count;
} turbo_error_domain_desc_t;

typedef struct {
  bool ok;
  int code;
  const char *message;
} turbo_result_t;

/** Return human-readable text for TURBO_*, negative errno, or negative Win32 error codes. */
CXX_C_API const char *turbo_strerror(int err);

/** Return structured metadata for an error code. */
CXX_C_API turbo_error_info_t turbo_error_info(int err);

/**
 * Register a custom error domain.
 *
 * The descriptor and entry table must remain alive until process exit or until
 * turbo_error_unregister_domain() is called. Domain ids must be in
 * [TURBO_ERROR_CUSTOM_DOMAIN_MIN, TURBO_ERROR_CUSTOM_DOMAIN_MAX].
 */
CXX_C_API int turbo_error_register_domain(const turbo_error_domain_desc_t *domain);

/** Remove a previously registered custom domain. */
CXX_C_API int turbo_error_unregister_domain(int domain);

static inline turbo_result_t turbo_result_ok(void) {
  turbo_result_t r;
  r.ok = true;
  r.code = TURBO_OK;
  r.message = "success";
  return r;
}

static inline turbo_result_t turbo_result_err(int code) {
  turbo_result_t r;
  r.ok = false;
  r.code = code;
  r.message = turbo_strerror(code);
  return r;
}

static inline turbo_result_t turbo_result_from_code(int code) {
  return code == TURBO_OK ? turbo_result_ok() : turbo_result_err(code);
}

static inline bool turbo_result_is_ok(turbo_result_t r) { return r.ok; }
static inline bool turbo_result_is_err(turbo_result_t r) { return !r.ok; }

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ERROR_H */
