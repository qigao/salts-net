#include "turbo_coro_send_profile_internal.h"

#include "turbo_error.h"

#include <stdatomic.h>

#ifdef TURBO_CORONET_INTERNAL_PROFILING
typedef struct turbo_coro_send_profile_aggregate_s {
  atomic_int enabled;
  atomic_uint_fast64_t samples;
  atomic_uint_fast64_t resume_samples;
  atomic_uint_fast64_t prepare_submit_sum_ns;
  atomic_uint_fast64_t kernel_completion_sum_ns;
  atomic_uint_fast64_t completion_publish_sum_ns;
  atomic_uint_fast64_t post_drain_sum_ns;
  atomic_uint_fast64_t handler_sum_ns;
  atomic_uint_fast64_t signal_to_scheduler_sum_ns;
  atomic_uint_fast64_t scheduler_dispatch_sum_ns;
  atomic_uint_fast64_t scheduler_resume_sum_ns;
  atomic_uint_fast64_t total_to_handler_sum_ns;
} turbo_coro_send_profile_aggregate_t;

static turbo_coro_send_profile_aggregate_t turbo_coro_send_profile;

static uint64_t turbo_coro_send_profile_delta(uint64_t later, uint64_t earlier) {
  return earlier != 0u && later >= earlier ? later - earlier : 0u;
}

static void turbo_coro_send_profile_add(atomic_uint_fast64_t *sum, uint64_t value) {
  atomic_fetch_add_explicit(sum, value, memory_order_relaxed);
}
#endif

void turbo_coro_send_profile_reset(void) {
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  atomic_store_explicit(&turbo_coro_send_profile.samples, 0u, memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.resume_samples, 0u, memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.prepare_submit_sum_ns, 0u,
                        memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.kernel_completion_sum_ns, 0u,
                        memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.completion_publish_sum_ns, 0u,
                        memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.post_drain_sum_ns, 0u, memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.handler_sum_ns, 0u, memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.signal_to_scheduler_sum_ns, 0u,
                        memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.scheduler_dispatch_sum_ns, 0u,
                        memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.scheduler_resume_sum_ns, 0u,
                        memory_order_relaxed);
  atomic_store_explicit(&turbo_coro_send_profile.total_to_handler_sum_ns, 0u,
                        memory_order_relaxed);
#endif
}

void turbo_coro_send_profile_set_enabled(int enabled) {
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  atomic_store_explicit(&turbo_coro_send_profile.enabled, enabled != 0, memory_order_release);
#else
  (void)enabled;
#endif
}

int turbo_coro_send_profile_enabled(void) {
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  return atomic_load_explicit(&turbo_coro_send_profile.enabled, memory_order_acquire);
#else
  return 0;
#endif
}

int turbo_coro_send_profile_snapshot(turbo_coro_send_profile_snapshot_t *out) {
  if (!out || out->size < sizeof(*out)) return TURBO_EINVAL;
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  out->samples = atomic_load_explicit(&turbo_coro_send_profile.samples, memory_order_relaxed);
  out->resume_samples =
      atomic_load_explicit(&turbo_coro_send_profile.resume_samples, memory_order_relaxed);
  out->prepare_submit_sum_ns =
      atomic_load_explicit(&turbo_coro_send_profile.prepare_submit_sum_ns, memory_order_relaxed);
  out->kernel_completion_sum_ns = atomic_load_explicit(
      &turbo_coro_send_profile.kernel_completion_sum_ns, memory_order_relaxed);
  out->completion_publish_sum_ns = atomic_load_explicit(
      &turbo_coro_send_profile.completion_publish_sum_ns, memory_order_relaxed);
  out->post_drain_sum_ns =
      atomic_load_explicit(&turbo_coro_send_profile.post_drain_sum_ns, memory_order_relaxed);
  out->handler_sum_ns =
      atomic_load_explicit(&turbo_coro_send_profile.handler_sum_ns, memory_order_relaxed);
  out->signal_to_scheduler_sum_ns = atomic_load_explicit(
      &turbo_coro_send_profile.signal_to_scheduler_sum_ns, memory_order_relaxed);
  out->scheduler_dispatch_sum_ns = atomic_load_explicit(
      &turbo_coro_send_profile.scheduler_dispatch_sum_ns, memory_order_relaxed);
  out->scheduler_resume_sum_ns = atomic_load_explicit(
      &turbo_coro_send_profile.scheduler_resume_sum_ns, memory_order_relaxed);
  out->total_to_handler_sum_ns = atomic_load_explicit(
      &turbo_coro_send_profile.total_to_handler_sum_ns, memory_order_relaxed);
  return TURBO_OK;
#else
  return TURBO_ENOTSUP;
#endif
}

void turbo_coro_send_profile_record_backend(uint64_t started_ns, uint64_t submitted_ns,
                                            uint64_t completed_ns, uint64_t post_requested_ns,
                                            uint64_t handler_entry_ns,
                                            uint64_t handler_finished_ns) {
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  uint64_t completion_publish_ns = 0u;
  uint64_t post_drain_ns;
  if (started_ns == 0u || submitted_ns < started_ns || completed_ns < submitted_ns ||
      handler_entry_ns < completed_ns || handler_finished_ns < handler_entry_ns)
    return;

  if (post_requested_ns >= completed_ns && handler_entry_ns >= post_requested_ns) {
    completion_publish_ns = post_requested_ns - completed_ns;
    post_drain_ns = handler_entry_ns - post_requested_ns;
  } else {
    post_drain_ns = handler_entry_ns - completed_ns;
  }

  turbo_coro_send_profile_add(&turbo_coro_send_profile.prepare_submit_sum_ns,
                              submitted_ns - started_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.kernel_completion_sum_ns,
                              completed_ns - submitted_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.completion_publish_sum_ns,
                              completion_publish_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.post_drain_sum_ns, post_drain_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.handler_sum_ns,
                              handler_finished_ns - handler_entry_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.total_to_handler_sum_ns,
                              handler_finished_ns - started_ns);
  atomic_fetch_add_explicit(&turbo_coro_send_profile.samples, 1u, memory_order_relaxed);
#else
  (void)started_ns;
  (void)submitted_ns;
  (void)completed_ns;
  (void)post_requested_ns;
  (void)handler_entry_ns;
  (void)handler_finished_ns;
#endif
}

void turbo_coro_send_profile_record_resume(uint64_t signaled_ns, uint64_t scheduler_entry_ns,
                                           uint64_t resumed_ns) {
#ifdef TURBO_CORONET_INTERNAL_PROFILING
  uint64_t value_ns;
  if (signaled_ns == 0u || scheduler_entry_ns < signaled_ns || resumed_ns < scheduler_entry_ns)
    return;
  value_ns = turbo_coro_send_profile_delta(resumed_ns, signaled_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.signal_to_scheduler_sum_ns,
                              scheduler_entry_ns - signaled_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.scheduler_dispatch_sum_ns,
                              resumed_ns - scheduler_entry_ns);
  turbo_coro_send_profile_add(&turbo_coro_send_profile.scheduler_resume_sum_ns, value_ns);
  atomic_fetch_add_explicit(&turbo_coro_send_profile.resume_samples, 1u, memory_order_relaxed);
#else
  (void)signaled_ns;
  (void)scheduler_entry_ns;
  (void)resumed_ns;
#endif
}
