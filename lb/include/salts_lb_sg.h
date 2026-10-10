#ifndef SALTS_LB_SG_H
#define SALTS_LB_SG_H

#include "salts_lb.h"
#include <cnet/cnet.h>
#include <cnet/owner_placement.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_LB_SG_VERSION 1u
#define SALTS_LB_SG_MAX_OWNERS 4u
typedef struct salts_lb_sg salts_lb_sg_t;

/** STRICT_KEY only: runs on ingress Owner 0 before frontend protocol I/O.
 * Peer is borrowed during this call. Return a known application key hash or
 * an error to reject admission. Must not block or reenter host control APIs. */
typedef int (*salts_lb_sg_key_fn)(const cnet_stream_peer *, uint64_t *out_key, void *user);

typedef struct salts_lb_sg_config {
  size_t size;
  uint32_t version;
  size_t owner_count; /* Fixed 1, 2 or 4. */
  size_t task_queue_capacity; /* Positive power of two, per Owner. */
  size_t handoff_queue_capacity; /* Positive, <= connection_capacity. */
  cnet_owner_placement_kind placement; /* Frontends only; workers use explicit ports. */
  size_t explicit_owner;
  salts_lb_sg_key_fn key;
  void *key_user;
} salts_lb_sg_config_t;

typedef struct salts_lb_sg_owner_stats {
  uint64_t local_admissions; /* Frontends accepted on their final Owner 0. */
  uint64_t handoff_admissions; /* Frontends adopted after remote handoff. */
  uint64_t worker_admissions;
  uint64_t observe_calls;
  size_t reserved, queued, taken; /* Combined frontend/worker connection credits. */
  bool drained;
} salts_lb_sg_owner_stats_t;

typedef struct salts_lb_sg_stats {
  size_t owner_count;
  uint64_t placement_calls; /* Includes choose failures, excludes key rejection. */
  uint64_t rejected_admissions; /* Includes frontend and worker admission failures. */
  bool stopping, stopped;
  salts_lb_sg_owner_stats_t owners[SALTS_LB_SG_MAX_OWNERS];
} salts_lb_sg_stats_t;

SALTSNET_LB_C_API salts_lb_sg_config_t salts_lb_sg_config_default(void);
/** Create one NativeIO SG with one external LB/manager per Owner. All LB
 * capacities are per Owner; connection_capacity covers BOTH frontend and
 * worker connections. Config is copied, callback users borrowed until destroy.
 * Route/filter/frame run on final Owners and may overlap across Owners; shared
 * user data needs synchronization. Callbacks must not block.
 * Worker selection and REQUEST reuse stay local to the final Owner; applications
 * provision each Owner/group through accept_workers. Missing workers retain
 * bounded waiting; no cross-Owner borrowing, DATA replay or implicit failover.
 * Invalid config: EINVAL/ERANGE; otherwise allocation/runtime errors. Failure
 * normally leaves *out NULL; failed rollback retains a non-listening cleanup
 * handle requiring stop/destroy. Never discard that handle.
 */
SALTSNET_LB_C_API int salts_lb_sg_create(const salts_lb_config_t *lb_config,
                                       const salts_lb_sg_config_t *sg_config, salts_lb_sg_t **out);
/** One frontend listener on Owner 0. address is borrowed until return. Zero
 * port requests an ephemeral port. EALREADY on repeat; ESHUTDOWN during stop.
 * Listener setup errors preserve any partial ownership for stop/destroy. */
SALTSNET_LB_C_API int salts_lb_sg_listen(salts_lb_sg_t *host, const char *address, uint16_t port);
/** One worker listener per explicit Owner; same setup/error contract as listen.
 * Each accepted worker is adopted locally. When route is configured, its first
 * data must be the existing newline-terminated group registration (<=63 bytes).
 * owner >= owner_count is EINVAL. No Worker is moved after admission. */
SALTSNET_LB_C_API int salts_lb_sg_accept_workers(salts_lb_sg_t *host, size_t owner,
                                               const char *address, uint16_t port);
/** Copy a successfully bound port; EINVAL before corresponding listen. */
SALTSNET_LB_C_API int salts_lb_sg_frontend_port(const salts_lb_sg_t *host, uint16_t *out_port);
SALTSNET_LB_C_API int salts_lb_sg_worker_port(const salts_lb_sg_t *host, size_t owner, uint16_t *out_port);
/** Dispatch/join short progress tasks on all Owners. One nonblocking backend
 * observe per Owner per round. Zero timeout does one round; otherwise return
 * after work/error or timeout. out_work is zeroed on entry. Admission ENOBUFS
 * rejects that child and preserves host ownership; other Owners still progress.
 * All control calls, including port/stats queries, must not overlap. Callback
 * reentry returns EBUSY. Only current_owner is callback-safe.
 */
SALTSNET_LB_C_API int salts_lb_sg_poll(salts_lb_sg_t *host, uint32_t timeout_ms, size_t *out_work);
/** Seal admission, cancel/route terminals, retire contexts and release leases
 * before SG shutdown. Uses one shutdown_timeout_ms budget. Error preserves
 * ownership for retry; may report a prior progress error after drain finishes.
 * Inspect stopped or retry stop; completed repeated stop returns OK. */
SALTSNET_LB_C_API int salts_lb_sg_stop(salts_lb_sg_t *host);
/** Requires completed stop (otherwise EBUSY). Error retains ownership. */
SALTSNET_LB_C_API int salts_lb_sg_destroy(salts_lb_sg_t *host);
/** Copy a control-plane snapshot between polls; not callable in a callback. */
SALTSNET_LB_C_API int salts_lb_sg_get_stats(const salts_lb_sg_t *host, salts_lb_sg_stats_t *out);
/** Callback-safe fixed Owner index, SIZE_MAX outside this host. */
SALTSNET_LB_C_API size_t salts_lb_sg_current_owner(const salts_lb_sg_t *host);

#ifdef __cplusplus
}
#endif
#endif
