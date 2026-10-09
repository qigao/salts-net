#ifndef SALTS_TCP_PROXY_SG_H
#define SALTS_TCP_PROXY_SG_H

#include "salts_tcp_proxy.h"
#include <cnet/owner_placement.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_TCP_PROXY_SG_VERSION 1u
#define SALTS_TCP_PROXY_SG_MAX_OWNERS 4u
typedef struct salts_tcp_proxy_sg salts_tcp_proxy_sg_t;

/** Runs once on ingress Owner 0 for STRICT_KEY admission, before protocol I/O.
 * Copy a known application key hash into out_key, or return an error to reject.
 * The peer is borrowed for this call. Must not block or reenter lifecycle APIs.
 */
typedef int (*salts_tcp_proxy_sg_key_fn)(const cnet_stream_peer *peer, uint64_t *out_key,
                                       void *user);

typedef struct salts_tcp_proxy_sg_config {
  size_t size;
  uint32_t version;
  size_t owner_count; /* Fixed 1, 2 or 4; session_capacity applies per Owner. */
  size_t task_queue_capacity; /* Positive power of two, per SG Owner. */
  size_t handoff_queue_capacity; /* Positive, <= per-Owner session_capacity. */
  cnet_owner_placement_kind placement;
  size_t explicit_owner;
  salts_tcp_proxy_sg_key_fn key;
  void *key_user;
} salts_tcp_proxy_sg_config_t;

typedef struct salts_tcp_proxy_sg_owner_stats {
  uint64_t local_admissions;
  uint64_t handoff_admissions;
  uint64_t observe_calls;
  size_t reserved, queued, taken;
  bool drained;
} salts_tcp_proxy_sg_owner_stats_t;

typedef struct salts_tcp_proxy_sg_stats {
  size_t owner_count;
  uint64_t placement_calls; /* Includes choose failures, excludes key rejection. */
  uint64_t rejected_admissions; /* Ingress rejection plus final-Owner adopt failure. */
  bool stopping, stopped;
  salts_tcp_proxy_sg_owner_stats_t owners[SALTS_TCP_PROXY_SG_MAX_OWNERS];
} salts_tcp_proxy_sg_stats_t;

SALTSNET_TCP_PROXY_C_API salts_tcp_proxy_sg_config_t salts_tcp_proxy_sg_config_default(void);

/** Own one NativeIO SG and one external CNet proxy per fixed Owner. Copies all
 * proxy strings/endpoint entries; callback users remain borrowed until destroy.
 * Access/route must not block and run concurrently on final Owners: shared user state must be safe
 * for that topology. key runs only on ingress Owner 0. No tunnel pooling/replay.
 * Returns EINVAL/ERANGE for invalid bounds, or allocation/runtime errors.
 * Normally failure leaves *out NULL. If rollback itself fails, *out retains a
 * non-listening cleanup handle: retry stop/destroy; never discard that handle.
 */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_sg_create(
    const salts_tcp_proxy_config_t *proxy_config, const salts_tcp_proxy_sg_config_t *config,
    salts_tcp_proxy_sg_t **out);

/** Open the sole ingress listener on Owner 0. host is borrowed until return.
 * One successful listen per host; returns EALREADY on repeat, ESHUTDOWN after
 * stop begins, or the listener error. Failed setup remains owned for stop.
 */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_sg_listen(salts_tcp_proxy_sg_t *host,
                                                     const char *address, uint16_t port);
/** Copy the bound port after successful listen. EINVAL before listen. */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_sg_port(const salts_tcp_proxy_sg_t *host,
                                                   uint16_t *out_port);
/** Drive short tasks on every Owner, each with exactly one nonblocking observe
 * per round. Return after work/error or timeout_ms; zero means one round.
 * Admission ENOBUFS rejects that child without changing Owner or retrying DATA;
 * other Owners still progress. out_work is zeroed on entry.
 * Control APIs must not overlap; callback reentry returns EBUSY. This explicit
 * caller-driven host creates no additional scheduler or progress thread.
 */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_sg_poll(salts_tcp_proxy_sg_t *host,
                                                   uint32_t timeout_ms, size_t *out_work);
/** Seal admission, cancel/route real terminals, recycle contexts, release host
 * leases, then shut down SG. Uses proxy_config.shutdown_timeout_ms; ETIMEDOUT
 * preserves ownership for retry. Never treats EBUSY as successful teardown.
 * Reports the first progress error even if drain finishes; inspect stopped in
 * get_stats or retry stop before destroy. Repeated completed stop returns OK.
 */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_sg_stop(salts_tcp_proxy_sg_t *host);
/** Requires completed stop (otherwise EBUSY). Error preserves host ownership. */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_sg_destroy(salts_tcp_proxy_sg_t *host);
/** Copy a quiescent control-plane snapshot between polls. Not callable from a callback. */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_sg_get_stats(
    const salts_tcp_proxy_sg_t *host, salts_tcp_proxy_sg_stats_t *out);
/** Callback-safe query: fixed Owner index, or SIZE_MAX outside this host. */
SALTSNET_TCP_PROXY_C_API size_t salts_tcp_proxy_sg_current_owner(
    const salts_tcp_proxy_sg_t *host);

#ifdef __cplusplus
}
#endif
#endif
