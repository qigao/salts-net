/**
 * @file coro_bidi_pump.h
 * @brief Bidirectional data pump between two coroutine clients.
 *
 * Extracted from tproxy for reuse by LB and other modules.
 * Spawns a background coroutine for one direction, uses the calling
 * coroutine for the other. Blocks until either side disconnects.
 */

#ifndef coro_BIDI_PUMP_H
#define coro_BIDI_PUMP_H

#include "platform.h"
#include <netcore/turbo_coro_client.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t rate_limit_bps; /**< 0 = unlimited */
} turbo_bidi_pump_config_t;

#define TURBO_BIDI_PUMP_CONFIG_DEFAULT { 0 }

/**
 * @brief Run bidirectional data pump between two clients.
 *
 * Forwards data in both directions (a→b and b→a) until either side
 * disconnects or errors. Blocks the calling coroutine.
 *
 * Does NOT destroy either client — caller manages lifecycle.
 *
 * @param a       First client (typically the "client" side)
 * @param b       Second client (typically the "upstream/worker" side)
 * @param config  Optional config (NULL for defaults)
 */
CXX_C_API void coro_bidi_pump(coro_client_t *a,
                                     coro_client_t *b,
                                     const turbo_bidi_pump_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* coro_BIDI_PUMP_H */
