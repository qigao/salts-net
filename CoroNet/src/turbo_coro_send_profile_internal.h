#ifndef TURBO_CORO_SEND_PROFILE_INTERNAL_H
#define TURBO_CORO_SEND_PROFILE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_coro_send_profile_snapshot_s {
  size_t size;
  uint64_t samples;
  uint64_t resume_samples;
  uint64_t prepare_submit_sum_ns;
  uint64_t kernel_completion_sum_ns;
  uint64_t completion_publish_sum_ns;
  uint64_t post_drain_sum_ns;
  uint64_t handler_sum_ns;
  uint64_t signal_to_scheduler_sum_ns;
  uint64_t scheduler_dispatch_sum_ns;
  uint64_t scheduler_resume_sum_ns;
  uint64_t total_to_handler_sum_ns;
} turbo_coro_send_profile_snapshot_t;

#define TURBO_CORO_SEND_PROFILE_SNAPSHOT_INIT                                                     \
  {sizeof(turbo_coro_send_profile_snapshot_t), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}

void turbo_coro_send_profile_reset(void);
void turbo_coro_send_profile_set_enabled(int enabled);
int turbo_coro_send_profile_enabled(void);
int turbo_coro_send_profile_snapshot(turbo_coro_send_profile_snapshot_t *out);

void turbo_coro_send_profile_record_backend(uint64_t started_ns, uint64_t submitted_ns,
                                            uint64_t completed_ns, uint64_t post_requested_ns,
                                            uint64_t handler_entry_ns,
                                            uint64_t handler_finished_ns);
void turbo_coro_send_profile_record_resume(uint64_t signaled_ns, uint64_t scheduler_entry_ns,
                                           uint64_t resumed_ns);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_SEND_PROFILE_INTERNAL_H */
