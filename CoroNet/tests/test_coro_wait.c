#include "CoroNet/turbo_coro_context.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdatomic.h>
#include <stdint.h>

static const uint64_t CORO_WAIT_INTERRUPT_DEADLINE_NS = UINT64_C(500000000);

typedef struct coro_wait_test_state_s {
  coro_context_t *ctx;
  coro_wait_t *wait;
  atomic_int entered;
  atomic_int result;
  uint64_t delay_ms;
  atomic_int companion_ran;
} coro_wait_test_state_t;

static void coro_wait_test_task(coro_t *co, void *arg) {
  coro_wait_test_state_t *state = (coro_wait_test_state_t *)arg;
  (void)co;
  atomic_store_explicit(&state->entered, 1, memory_order_release);
  atomic_store_explicit(&state->result, coro_wait_for(state->wait, state->delay_ms),
                        memory_order_release);
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

spec("coro_wait") {
  it("returns after its deadline without blocking other coroutines") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    uint64_t started = turbo_hrtime();
    check_not_null(ctx);
    check_not_null(wait);
    state.ctx = ctx;
    state.wait = wait;
    state.delay_ms = 20;
    atomic_init(&state.entered, 0);
    atomic_init(&state.result, TURBO_EALREADY);
    atomic_init(&state.companion_ran, 0);
    check_int_eq(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    check_int_eq(coro_context_spawn(ctx, coro_wait_companion_task, &state), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_int_eq(atomic_load_explicit(&state.companion_ran, memory_order_acquire), 1);
    check_int_eq(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_EALREADY);
    while (atomic_load_explicit(&state.result, memory_order_acquire) == TURBO_EALREADY) {
      (void)coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    check_int_eq(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_OK);
    check_true(turbo_hrtime() - started >= UINT64_C(20000000));
    check_int_eq(coro_wait_destroy(wait), TURBO_OK);
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
    state.ctx = ctx;
    state.wait = wait;
    state.delay_ms = 5000;
    atomic_init(&state.entered, 0);
    atomic_init(&state.result, TURBO_EALREADY);
    atomic_init(&state.companion_ran, 0);
    check_int_eq(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    check_int_eq(turbo_thread_create(&thread, coro_wait_context_thread, &state), TURBO_OK);
    while (!atomic_load_explicit(&state.entered, memory_order_acquire))
      turbo_thread_yield();
    started = turbo_hrtime();
    do {
      interrupt_rc = coro_wait_interrupt(wait, TURBO_ESHUTDOWN);
      if (interrupt_rc == TURBO_EALREADY) turbo_thread_yield();
    } while (interrupt_rc == TURBO_EALREADY &&
             turbo_hrtime() - started < CORO_WAIT_INTERRUPT_DEADLINE_NS);
    check_int_eq(interrupt_rc, TURBO_OK);
    check_int_eq(coro_wait_interrupt(wait, TURBO_ECANCELED), TURBO_EALREADY);
    (void)turbo_thread_join(&thread);
    check_int_eq(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ESHUTDOWN);
    check_true(turbo_hrtime() - started < CORO_WAIT_INTERRUPT_DEADLINE_NS);
    check_int_eq(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("rejects destroy while a wait is active") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    check_not_null(ctx);
    check_not_null(wait);
    state.ctx = ctx;
    state.wait = wait;
    state.delay_ms = 5000;
    atomic_init(&state.entered, 0);
    atomic_init(&state.result, TURBO_EALREADY);
    atomic_init(&state.companion_ran, 0);
    check_int_eq(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_int_eq(coro_wait_destroy(wait), TURBO_EBUSY);
    check_int_eq(coro_wait_interrupt(wait, TURBO_ECANCELED), TURBO_OK);
    while (atomic_load_explicit(&state.result, memory_order_acquire) == TURBO_EALREADY) {
      (void)coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    check_int_eq(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ECANCELED);
    check_int_eq(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("drains an accepted wait interruption before context stop returns") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_t *wait = coro_wait_create(ctx);
    coro_wait_test_state_t state;
    check_not_null(ctx);
    check_not_null(wait);
    state.ctx = ctx;
    state.wait = wait;
    state.delay_ms = 5000;
    atomic_init(&state.entered, 0);
    atomic_init(&state.result, TURBO_EALREADY);
    atomic_init(&state.companion_ran, 0);
    check_int_eq(coro_context_spawn(ctx, coro_wait_test_task, &state), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_int_eq(coro_post(ctx, coro_wait_interrupt_and_stop, &state, NULL), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_DEFAULT);
    check_int_eq(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ESHUTDOWN);
    check_int_eq(coro_wait_destroy(wait), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("drains posts queued before a persistent default run starts") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_wait_test_state_t state;
    check_not_null(ctx);
    state.ctx = ctx;
    atomic_init(&state.companion_ran, 0);

    coro_context_set_persistent(ctx, 1);
    check_int_eq(coro_post(ctx, coro_wait_stop_persistent_context, &state, NULL), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_DEFAULT);

    check_int_eq(atomic_load_explicit(&state.companion_ran, memory_order_acquire), 1);
    coro_context_destroy(ctx);
  }
}
