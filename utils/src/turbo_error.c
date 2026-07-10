#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define TURBO_ERROR_MAX_CUSTOM_DOMAINS 64

#define TURBO_ERROR_TABLE(X)                                                                        \
  X(TURBO_EAI_ADDRFAMILY, "TURBO_EAI_ADDRFAMILY", "address family for hostname not supported")     \
  X(TURBO_EAI_AGAIN, "TURBO_EAI_AGAIN", "temporary DNS failure")                                  \
  X(TURBO_EAI_BADFLAGS, "TURBO_EAI_BADFLAGS", "bad DNS flags")                                    \
  X(TURBO_EAI_FAIL, "TURBO_EAI_FAIL", "non-recoverable DNS failure")                              \
  X(TURBO_EAI_FAMILY, "TURBO_EAI_FAMILY", "address family not supported by DNS result")            \
  X(TURBO_EAI_MEMORY, "TURBO_EAI_MEMORY", "out of memory during DNS lookup")                       \
  X(TURBO_EAI_NODATA, "TURBO_EAI_NODATA", "no DNS data")                                          \
  X(TURBO_EAI_NONAME, "TURBO_EAI_NONAME", "hostname not found")                                   \
  X(TURBO_EAI_SERVICE, "TURBO_EAI_SERVICE", "service not available for socket type")               \
  X(TURBO_EAI_SOCKTYPE, "TURBO_EAI_SOCKTYPE", "socket type not supported")                         \
  X(TURBO_EAI_SYSTEM, "TURBO_EAI_SYSTEM", "system error during DNS lookup")                        \
  X(TURBO_EAI_CANCELED, "TURBO_EAI_CANCELED", "DNS lookup canceled")                              \
  X(TURBO_EADDRINUSE, "TURBO_EADDRINUSE", "address already in use")                               \
  X(TURBO_EADDRNOTAVAIL, "TURBO_EADDRNOTAVAIL", "address not available")                          \
  X(TURBO_EAFNOSUPPORT, "TURBO_EAFNOSUPPORT", "address family not supported")                      \
  X(TURBO_EALREADY, "TURBO_EALREADY", "operation already in progress")                            \
  X(TURBO_EBADF, "TURBO_EBADF", "bad file descriptor")                                            \
  X(TURBO_EBUSY, "TURBO_EBUSY", "resource busy or locked")                                        \
  X(TURBO_ECANCELED, "TURBO_ECANCELED", "operation canceled")                                     \
  X(TURBO_ECHARSET, "TURBO_ECHARSET", "invalid character set")                                    \
  X(TURBO_ECONNABORTED, "TURBO_ECONNABORTED", "connection aborted")                               \
  X(TURBO_ECONNREFUSED, "TURBO_ECONNREFUSED", "connection refused")                               \
  X(TURBO_ECONNRESET, "TURBO_ECONNRESET", "connection reset by peer")                             \
  X(TURBO_EDESTADDRREQ, "TURBO_EDESTADDRREQ", "destination address required")                      \
  X(TURBO_EFAULT, "TURBO_EFAULT", "bad address")                                                  \
  X(TURBO_EFBIG, "TURBO_EFBIG", "file too large")                                                 \
  X(TURBO_EHOSTUNREACH, "TURBO_EHOSTUNREACH", "host is unreachable")                              \
  X(TURBO_EINTR, "TURBO_EINTR", "operation interrupted")                                          \
  X(TURBO_EINVAL, "TURBO_EINVAL", "invalid argument")                                             \
  X(TURBO_EIO, "TURBO_EIO", "I/O error")                                                          \
  X(TURBO_EISCONN, "TURBO_EISCONN", "socket is already connected")                                \
  X(TURBO_EISDIR, "TURBO_EISDIR", "is a directory")                                               \
  X(TURBO_ELOOP, "TURBO_ELOOP", "too many symbolic links")                                        \
  X(TURBO_EMFILE, "TURBO_EMFILE", "too many open files")                                          \
  X(TURBO_EMSGSIZE, "TURBO_EMSGSIZE", "message too long")                                         \
  X(TURBO_ENAMETOOLONG, "TURBO_ENAMETOOLONG", "name too long")                                    \
  X(TURBO_ENETDOWN, "TURBO_ENETDOWN", "network is down")                                          \
  X(TURBO_ENETUNREACH, "TURBO_ENETUNREACH", "network is unreachable")                             \
  X(TURBO_ENFILE, "TURBO_ENFILE", "too many open files in system")                                \
  X(TURBO_ENOBUFS, "TURBO_ENOBUFS", "no buffer space available")                                  \
  X(TURBO_ENODEV, "TURBO_ENODEV", "no such device")                                               \
  X(TURBO_ENOENT, "TURBO_ENOENT", "no such file or directory")                                    \
  X(TURBO_ENOMEM, "TURBO_ENOMEM", "not enough memory")                                            \
  X(TURBO_ENONET, "TURBO_ENONET", "machine is not on the network")                                \
  X(TURBO_ENOPROTOOPT, "TURBO_ENOPROTOOPT", "protocol option not available")                      \
  X(TURBO_ENOSPC, "TURBO_ENOSPC", "no space left on device")                                      \
  X(TURBO_ENOSYS, "TURBO_ENOSYS", "function not implemented")                                     \
  X(TURBO_ENOTCONN, "TURBO_ENOTCONN", "socket is not connected")                                  \
  X(TURBO_ENOTDIR, "TURBO_ENOTDIR", "not a directory")                                            \
  X(TURBO_ENOTEMPTY, "TURBO_ENOTEMPTY", "directory not empty")                                    \
  X(TURBO_ENOTSOCK, "TURBO_ENOTSOCK", "not a socket")                                             \
  X(TURBO_ENOTSUP, "TURBO_ENOTSUP", "operation not supported")                                    \
  X(TURBO_EPERM, "TURBO_EPERM", "operation not permitted")                                        \
  X(TURBO_EPIPE, "TURBO_EPIPE", "broken pipe")                                                    \
  X(TURBO_EPROTO, "TURBO_EPROTO", "protocol error")                                               \
  X(TURBO_EPROTONOSUPPORT, "TURBO_EPROTONOSUPPORT", "protocol not supported")                     \
  X(TURBO_EPROTOTYPE, "TURBO_EPROTOTYPE", "protocol wrong type for socket")                       \
  X(TURBO_ERANGE, "TURBO_ERANGE", "result too large")                                             \
  X(TURBO_EROFS, "TURBO_EROFS", "read-only file system")                                          \
  X(TURBO_ESHUTDOWN, "TURBO_ESHUTDOWN", "cannot send after transport endpoint shutdown")          \
  X(TURBO_ESPIPE, "TURBO_ESPIPE", "invalid seek")                                                 \
  X(TURBO_ESRCH, "TURBO_ESRCH", "no such process")                                                \
  X(TURBO_ETIMEDOUT, "TURBO_ETIMEDOUT", "operation timed out")                                    \
  X(TURBO_ETXTBSY, "TURBO_ETXTBSY", "text file busy")                                             \
  X(TURBO_EXDEV, "TURBO_EXDEV", "cross-device link")                                              \
  X(TURBO_UNKNOWN, "TURBO_UNKNOWN", "unknown error")                                              \
  X(TURBO_EOF, "TURBO_EOF", "end of file")

#define TURBO_ERROR_ENTRY(code, name, message) {code, name, message},

static const turbo_error_entry_t turbo_error_table[] = {TURBO_ERROR_TABLE(TURBO_ERROR_ENTRY)};

static turbo_once_t turbo_error_registry_once = TURBO_ONCE_INIT;
static turbo_mutex_t turbo_error_registry_mutex;
static const turbo_error_domain_desc_t *turbo_error_custom_domains[TURBO_ERROR_MAX_CUSTOM_DOMAINS];
static size_t turbo_error_custom_domain_count;

static void turbo_error_registry_init(void) {
  turbo_mutex_init(&turbo_error_registry_mutex);
}

static const turbo_error_entry_t *turbo_find_error(int err) {
  for (size_t i = 0; i < sizeof(turbo_error_table) / sizeof(turbo_error_table[0]); ++i) {
    if (turbo_error_table[i].code == err) return &turbo_error_table[i];
  }
  return NULL;
}

static int turbo_custom_domain_valid(const turbo_error_domain_desc_t *domain) {
  if (!domain || !domain->domain_name || !domain->entries || domain->count == 0) return 0;
  if (domain->domain < TURBO_ERROR_CUSTOM_DOMAIN_MIN ||
      domain->domain > TURBO_ERROR_CUSTOM_DOMAIN_MAX)
    return 0;
  for (size_t i = 0; i < domain->count; ++i) {
    const turbo_error_entry_t *entry = &domain->entries[i];
    if (!entry->name || !entry->message) return 0;
    if (entry->code >= 0) return 0;
    if (TURBO_ERROR_CUSTOM_DOMAIN(entry->code) != domain->domain) return 0;
    if (TURBO_ERROR_CUSTOM_LOCAL(entry->code) == 0) return 0;
  }
  return 1;
}

static const turbo_error_entry_t *turbo_find_custom_error_locked(
    int err, const turbo_error_domain_desc_t **out_domain) {
  int domain_id;

  if (err >= 0) return NULL;
  domain_id = TURBO_ERROR_CUSTOM_DOMAIN(err);
  if (domain_id < TURBO_ERROR_CUSTOM_DOMAIN_MIN ||
      domain_id > TURBO_ERROR_CUSTOM_DOMAIN_MAX) {
    return NULL;
  }

  for (size_t i = 0; i < turbo_error_custom_domain_count; ++i) {
    const turbo_error_domain_desc_t *domain = turbo_error_custom_domains[i];
    if (!domain || domain->domain != domain_id) continue;
    for (size_t j = 0; j < domain->count; ++j) {
      if (domain->entries[j].code == err) {
        if (out_domain) *out_domain = domain;
        return &domain->entries[j];
      }
    }
  }
  return NULL;
}

int turbo_error_register_domain(const turbo_error_domain_desc_t *domain) {
  turbo_once(&turbo_error_registry_once, turbo_error_registry_init);
  if (!turbo_custom_domain_valid(domain)) return TURBO_EINVAL;

  turbo_mutex_lock(&turbo_error_registry_mutex);
  for (size_t i = 0; i < turbo_error_custom_domain_count; ++i) {
    if (turbo_error_custom_domains[i] &&
        turbo_error_custom_domains[i]->domain == domain->domain) {
      turbo_mutex_unlock(&turbo_error_registry_mutex);
      return TURBO_EALREADY;
    }
  }
  if (turbo_error_custom_domain_count >= TURBO_ERROR_MAX_CUSTOM_DOMAINS) {
    turbo_mutex_unlock(&turbo_error_registry_mutex);
    return TURBO_ENOSPC;
  }
  turbo_error_custom_domains[turbo_error_custom_domain_count++] = domain;
  turbo_mutex_unlock(&turbo_error_registry_mutex);
  return TURBO_OK;
}

int turbo_error_unregister_domain(int domain) {
  turbo_once(&turbo_error_registry_once, turbo_error_registry_init);
  if (domain < TURBO_ERROR_CUSTOM_DOMAIN_MIN || domain > TURBO_ERROR_CUSTOM_DOMAIN_MAX) {
    return TURBO_EINVAL;
  }

  turbo_mutex_lock(&turbo_error_registry_mutex);
  for (size_t i = 0; i < turbo_error_custom_domain_count; ++i) {
    if (turbo_error_custom_domains[i] && turbo_error_custom_domains[i]->domain == domain) {
      turbo_error_custom_domains[i] =
          turbo_error_custom_domains[turbo_error_custom_domain_count - 1];
      turbo_error_custom_domains[turbo_error_custom_domain_count - 1] = NULL;
      --turbo_error_custom_domain_count;
      turbo_mutex_unlock(&turbo_error_registry_mutex);
      return TURBO_OK;
    }
  }
  turbo_mutex_unlock(&turbo_error_registry_mutex);
  return TURBO_ENOENT;
}

static const char *turbo_unknown_message(int err) {
  static TURBO_THREAD_LOCAL char buf[64];
  snprintf(buf, sizeof(buf), "unknown error %d", err);
  return buf;
}

static const char *turbo_posix_strerror(int posix_code) {
  const char *msg;

  if (posix_code <= 0) return NULL;
  msg = strerror(posix_code);
  if (!msg || msg[0] == '\0') return NULL;
  if (strstr(msg, "Unknown error") != NULL || strstr(msg, "unknown error") != NULL) return NULL;
  return msg;
}

#ifdef _WIN32
static const char *turbo_win32_strerror(int win32_code) {
  static TURBO_THREAD_LOCAL char buf[256];
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD written;

  if (win32_code <= 0) return NULL;
  written = FormatMessageA(flags, NULL, (DWORD)win32_code, 0, buf, (DWORD)sizeof(buf), NULL);
  if (written == 0) return NULL;
  while (written > 0 && (buf[written - 1] == '\n' || buf[written - 1] == '\r' ||
                         buf[written - 1] == ' ' || buf[written - 1] == '\t')) {
    buf[--written] = '\0';
  }
  return buf;
}
#endif

turbo_error_info_t turbo_error_info(int err) {
  turbo_error_info_t info;
  const turbo_error_entry_t *entry;
  const turbo_error_domain_desc_t *custom_domain = NULL;
  int native_code;
  const char *native_msg;

  info.code = err;
  info.custom_domain = 0;
  info.domain = TURBO_ERROR_DOMAIN_UNKNOWN;
  info.domain_name = "unknown";
  info.name = "UNKNOWN";
  info.message = NULL;

  if (err == TURBO_OK) {
    info.domain = TURBO_ERROR_DOMAIN_NONE;
    info.domain_name = "none";
    info.name = "TURBO_OK";
    info.message = "success";
    return info;
  }

  entry = turbo_find_error(err);
  if (entry) {
    info.domain = TURBO_ERROR_DOMAIN_TURBO;
    info.domain_name = "turbo";
    info.name = entry->name;
    info.message = entry->message;
    return info;
  }

  turbo_once(&turbo_error_registry_once, turbo_error_registry_init);
  turbo_mutex_lock(&turbo_error_registry_mutex);
  entry = turbo_find_custom_error_locked(err, &custom_domain);
  if (entry) {
    info.custom_domain = custom_domain->domain;
    info.domain = TURBO_ERROR_DOMAIN_CUSTOM;
    info.domain_name = custom_domain->domain_name;
    info.name = entry->name;
    info.message = entry->message;
    turbo_mutex_unlock(&turbo_error_registry_mutex);
    return info;
  }
  turbo_mutex_unlock(&turbo_error_registry_mutex);

  if (err < 0) {
    int domain_id = TURBO_ERROR_CUSTOM_DOMAIN(err);
    if (domain_id >= TURBO_ERROR_CUSTOM_DOMAIN_MIN &&
        domain_id <= TURBO_ERROR_CUSTOM_DOMAIN_MAX) {
      info.custom_domain = domain_id;
      info.domain_name = "custom";
      info.message = turbo_unknown_message(err);
      return info;
    }
  }

  native_code = err < 0 ? -err : err;
  native_msg = turbo_posix_strerror(native_code);
  if (native_msg) {
    info.domain = TURBO_ERROR_DOMAIN_POSIX;
    info.domain_name = "posix";
    info.name = "POSIX";
    info.message = native_msg;
    return info;
  }

#ifdef _WIN32
  native_msg = turbo_win32_strerror(native_code);
  if (native_msg) {
    info.domain = TURBO_ERROR_DOMAIN_WIN32;
    info.domain_name = "win32";
    info.name = "WIN32";
    info.message = native_msg;
    return info;
  }
#endif

  info.message = turbo_unknown_message(err);
  return info;
}

const char *turbo_strerror(int err) {
  return turbo_error_info(err).message;
}
