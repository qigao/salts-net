#include "CoroNet/turbo_coro_context.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdatomic.h>
#include <stdint.h>

enum {
  CORO_WAIT_DEADLINE_MS = 20,
  CORO_WAIT_LONG_DELAY_MS = 5000,
  CORO_WAIT_PUMP_TIMEOUT_MS = 1000,
  CORO_WAIT_RACE_DELAY_MS = 2,
  CORO_WAIT_RACE_ITERATIONS = 32
};

static const uint64_t CORO_WAIT_INTERRUPT_DEADLINE_NS = UINT64_C(500000000);
static const uint64_t CORO_WAIT_MAX_ELAPSED_NS = UINT64_C(500000000);

typedef struct coro_wait_test_state_s {
  coro_context_t *ctx;
  coro_wait_t *wait;
  atomic_int entered;
  atomic_int result;
  uint64_t delay_ms;
  uint64_t wait_started_ns;
  uint64_t wait_finished_ns;
  atomic_int companion_ran;
} coro_wait_test_state_t;

static void coro_wait_test_state_init(coro_wait_test_state_t *state, coro_context_t *ctx,
                                      coro_wait_t *wait, uint64_t delay_ms) {
  state->ctx = ctx;
  state->wait = wait;
  state->delay_ms = delay_ms;
  state->wait_started_ns = 0;
  state->wait_finished_ns = 0;
  atomic_init(&state->entered, 0);
  atomic_init(&state->result, TURBO_EALREADY);
  atomic_init(&state->companion_ran, 0);
}

static void coro_wait_test_task(coro_t *co, void *arg) {
  coro_wait_test_state_t *state = (coro_wait_test_state_t *)arg;
  int rc;
  (void)co;
  atomic_store_explicit(&state->entered, 1, memory_order_release);
  state->wait_started_ns = turbo_hrtime();
  rc = coro_wait_for(state->wait, state->delay_ms);
  state->wait_finished_ns = turbo_hrtime();
  atomic_store_explicit(&state->result, rc, memory_order_release);
}

static void coro_wait_context_thread(void *arg) {
  coro_wait_test_state_t *state = (coro_wait_test_state_t *)arg;
  (void)coro_context_run(state->ctx, TURBO_RUN_DEFAULT);
}

static void coro_wait_interrupt_and_stop(void *arg1, void *arg2) {
  coro_wait_test_state_t *state = (coro_wait_test_state_t *)arg1;
  (void)arg2;
  (void)coro_wait_interrupt(state->wait, TURBO_ESHUTDOWN);
  coro_context_stop(state->ctx);
}

static void coro_wait_stop_persistent_context(void *arg1, void *arg2) {
  coro_wait_test_state_t *state = (coro_wait_test_state_t *)arg1;
  (void)arg2;

  atomic_store_explicit(&state->companion_ran, 1, memory_order_release);
  coro_context_set_persistent(state->ctx, 0);
  coro_context_stop(state->ctx);
}

static void coro_wait_companion_task(coro_t *co, void *arg) {
  coro_wait_test_state_t *state = (coro_wait_test_state_t *)arg;
  (void)co;
  atomic_store_explicit(&state->companion_ran, 1, memory_order_release);
}

static int coro_wait_run_until_complete(coro_context_t *ctx, coro_wait_test_state_t *state,
                                        uint64_t timeout_ms) {
  uint64_t now_ms;
  uint64_t deadline_ms;

  if (!ctx || !state) return TURBO_EINVAL;
  now_ms = turbo_monotonic_ms();
  deadline_ms = timeout_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + timeout_ms;
  while (atomic_load_explicit(&state->result, memory_order_acquire) == TURBO_EALREADY &&
         turbo_monotonic_ms() < deadline_ms) {
    (void)coro_context_run(ctx, TURBO_RUN_ONCE);
  }
  return atomic_load_explicit(&state->result, memory_order_acquire) == TURBO_EALREADY
             ? TURBO_ETIMEDOUT
             : TURBO_OK;
}

static int coro_wait_until_entered(coro_wait_test_state_t *state, uint64_t timeout_ns) {
  uint64_t now_ns;
  uint64_t deadline_ns;

  if (!state) return TURBO_EINVAL;
  now_ns = turbo_hrtime();
  deadline_ns = timeout_ns > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + timeout_ns;
  while (!atomic_load_explicit(&state->entered, memory_order_acquire) &&
         turbo_hrtime() < deadline_ns) {
    turbo_thread_yield();
  }
  return atomic_load_explicit(&state->entered, memory_order_acquire) ? TURBO_OK
                                                                     : TURBO_ETIMEDOUT;
}

static void coro_wait_yield_for(uint64_t duration_ns) {
  uint64_t started_ns = turbo_hrtime();

  while (turbo_hrtime() - started_ns < duration_ns) turbo_thread_yield();
}

spec("coro_wait") {
  it("returns no earlier than its deadline") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    int pump_rc;
    check_not_null(ctx);
    check_not_null(wait);
    coro_wait_test_state_init(&state, ctx, wait, CORO_WAIT_DEADLINE_MS);
    check_equal(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    pump_rc = coro_wait_run_until_complete(ctx, &state, CORO_WAIT_PUMP_TIMEOUT_MS);
    check_equal(pump_rc, TURBO_OK);
    if (pump_rc == TURBO_OK) {
      check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_OK);
      check_true(state.wait_finished_ns >= state.wait_started_ns);
      if (state.wait_finished_ns >= state.wait_started_ns) {
        uint64_t elapsed_ns = state.wait_finished_ns - state.wait_started_ns;
        check_true(elapsed_ns >= turbo_ms_to_ns(CORO_WAIT_DEADLINE_MS));
        check_true(elapsed_ns <= CORO_WAIT_MAX_ELAPSED_NS);
      }
    }
    check_equal(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("does not block other coroutines") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    check_not_null(ctx);
    check_not_null(wait);
    coro_wait_test_state_init(&state, ctx, wait, CORO_WAIT_LONG_DELAY_MS);
    check_equal(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    check_equal(coro_context_spawn(ctx, coro_wait_companion_task, &state), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_equal(atomic_load_explicit(&state.companion_ran, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_EALREADY);
    check_equal(coro_wait_interrupt(wait, TURBO_ECANCELED), TURBO_OK);
    check_equal(coro_wait_run_until_complete(ctx, &state, CORO_WAIT_PUMP_TIMEOUT_MS), TURBO_OK);
    check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ECANCELED);
    check_equal(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("accepts a cross-thread interrupt and resumes exactly once") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    turbo_thread_t thread;
    uint64_t started;
    int interrupt_rc;
    check_not_null(ctx);
    check_not_null(wait);
    coro_wait_test_state_init(&state, ctx, wait, CORO_WAIT_LONG_DELAY_MS);
    check_equal(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    check_equal(turbo_thread_create(&thread, coro_wait_context_thread, &state), TURBO_OK);
    check_equal(coro_wait_until_entered(&state, CORO_WAIT_INTERRUPT_DEADLINE_NS), TURBO_OK);
    started = turbo_hrtime();
    do {
      interrupt_rc = coro_wait_interrupt(wait, TURBO_ESHUTDOWN);
      if (interrupt_rc == TURBO_EALREADY) turbo_thread_yield();
    } while (interrupt_rc == TURBO_EALREADY &&
             turbo_hrtime() - started < CORO_WAIT_INTERRUPT_DEADLINE_NS);
    check_equal(interrupt_rc, TURBO_OK);
    check_equal(coro_wait_interrupt(wait, TURBO_ECANCELED), TURBO_EALREADY);
    (void)turbo_thread_join(&thread);
    check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ESHUTDOWN);
    check_true(turbo_hrtime() - started < CORO_WAIT_INTERRUPT_DEADLINE_NS);
    check_equal(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("resolves timer and interrupt races without deadlock") {
    for (int iteration = 0; iteration < CORO_WAIT_RACE_ITERATIONS; ++iteration) {
      coro_context_t *ctx = coro_context_create(NULL);
      coro_wait_t *wait = coro_wait_create(ctx);
      coro_wait_test_state_t state;
      turbo_thread_t thread;
      int interrupt_rc;

      check_not_null(ctx);
      check_not_null(wait);
      coro_wait_test_state_init(&state, ctx, wait, CORO_WAIT_RACE_DELAY_MS);
      check_equal(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
      check_equal(turbo_thread_create(&thread, coro_wait_context_thread, &state), TURBO_OK);
      check_equal(coro_wait_until_entered(&state, CORO_WAIT_INTERRUPT_DEADLINE_NS), TURBO_OK);
      coro_wait_yield_for(turbo_ms_to_ns(1));
      interrupt_rc = coro_wait_interrupt(wait, TURBO_ECANCELED);
      check_true(interrupt_rc == TURBO_OK || interrupt_rc == TURBO_EALREADY);
      check_equal(turbo_thread_join(&thread), TURBO_OK);
      if (interrupt_rc == TURBO_OK) {
        check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ECANCELED);
      } else {
        check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_OK);
      }
      check_equal(coro_wait_destroy(wait), TURBO_OK);
      coro_context_destroy(ctx);
    }
  }

  it("rejects destroy while a wait is active") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    check_not_null(ctx);
    check_not_null(wait);
    coro_wait_test_state_init(&state, ctx, wait, CORO_WAIT_LONG_DELAY_MS);
    check_equal(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_equal(coro_wait_destroy(wait), TURBO_EBUSY);
    check_equal(coro_wait_interrupt(wait, TURBO_ECANCELED), TURBO_OK);
    check_equal(coro_wait_run_until_complete(ctx, &state, CORO_WAIT_PUMP_TIMEOUT_MS), TURBO_OK);
    check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ECANCELED);
    check_equal(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("drains an accepted wait interruption before context stop returns") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    check_not_null(ctx);
    check_not_null(wait);
    coro_wait_test_state_init(&state, ctx, wait, CORO_WAIT_LONG_DELAY_MS);
    check_equal(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_equal(coro_post(ctx, coro_wait_interrupt_and_stop, &state, NULL), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_DEFAULT);
    check_equal(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ESHUTDOWN);
    check_equal(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("drains posts queued before a persistent default run starts") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_test_state_t state;
    check_not_null(ctx);
    state.ctx = ctx;
    atomic_init(&state.companion_ran, 0);

    coro_context_set_persistent(ctx, 1);
    check_equal(coro_post(ctx, coro_wait_stop_persistent_context, &state, NULL), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_DEFAULT);

    check_equal(atomic_load_explicit(&state.companion_ran, memory_order_acquire), 1);
    coro_context_destroy(ctx);
  }
}
