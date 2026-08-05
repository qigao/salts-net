/**
 * @file turbo_tls.h
 * @brief TLS integration helpers for caller-owned native contexts.
 */

#ifndef TURBO_TLS_H
#define TURBO_TLS_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ssl_ctx_st;

/**
 * Add system-trusted CA certificates to a caller-owned OpenSSL/BoringSSL
 * context.
 *
 * The context remains owned by the caller. Call this during single-threaded
 * context configuration, before creating SSL objects or starting handshakes.
 * On Windows this imports the ROOT and CA system stores in addition to the
 * TLS library's default verification paths.
 *
 * @param context Borrowed OpenSSL/BoringSSL context.
 * @return TURBO_OK when at least one system trust source is available,
 *         TURBO_EINVAL for NULL, or TURBO_EIO when every source fails.
 */
CXX_C_API int turbo_tls_load_system_ca_certificates(struct ssl_ctx_st *context);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TLS_H */
