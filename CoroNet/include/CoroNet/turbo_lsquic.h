/**
 * @file turbo_lsquic.h
 * @brief CoroNet datagram adapter for the LSQUIC engine.
 */

#ifndef TURBO_LSQUIC_H
#define TURBO_LSQUIC_H

#include <stddef.h>

#include "platform.h"
#include "turbo_coro_context.h"
#include "turbo_datagram.h"

#include "lsquic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_lsquic_s turbo_lsquic_t;

/**
 * Configuration for one LSQUIC engine and its CoroNet UDP socket.
 *
 * The adapter copies @p engine_api during creation and replaces
 * ea_packets_out with its CoroNet-backed sender. Callback functions and their
 * user contexts remain owned by the caller and must outlive the adapter.
 * The caller owns @p context and must keep running it after creation.
 */
typedef struct turbo_lsquic_config_s {
  coro_context_t *context;
  turbo_datagram_kind_t datagram_kind;
  const char *bind_host;
  unsigned short bind_port;
  unsigned engine_flags;
  const struct lsquic_engine_api *engine_api;
  void *peer_ctx;
} turbo_lsquic_config_t;

/**
 * Create, bind, and start an LSQUIC engine over a CoroNet datagram.
 *
 * The adapter initializes LSQUIC's process-global state on first use and
 * releases it after the last adapter is destroyed. Do not call
 * lsquic_global_init() or lsquic_global_cleanup() directly while using this
 * adapter.
 */
CXX_C_API int turbo_lsquic_create(const turbo_lsquic_config_t *config,
                                  turbo_lsquic_t **out_adapter);

/**
 * Stop receive callbacks and destroy the engine and datagram.
 * The context passed at creation remains owned by the caller.
 */
CXX_C_API void turbo_lsquic_destroy(turbo_lsquic_t *adapter);

/** Process tickable connections; call this from the owning event-loop thread. */
CXX_C_API void turbo_lsquic_process(turbo_lsquic_t *adapter);

/** Retry packets that LSQUIC could not send earlier. */
CXX_C_API void turbo_lsquic_send_unsent(turbo_lsquic_t *adapter);

/** Borrow the underlying engine for LSQUIC connection APIs. */
CXX_C_API lsquic_engine_t *turbo_lsquic_engine(turbo_lsquic_t *adapter);

/** Borrow the underlying CoroNet datagram for transport configuration. */
CXX_C_API turbo_datagram_t *turbo_lsquic_datagram(turbo_lsquic_t *adapter);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_LSQUIC_H */
