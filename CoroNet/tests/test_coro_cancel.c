#include "CoroNet.h"
#include "tinytest.h"

#include <stdatomic.h>

typedef struct cancel_wait_state_s {
  coro_wait_t *wait;
  const coro_cancel_token_t *token;
  coro_cancel_registration_t *registration;
  atomic_int entered;
  atomic_int callback_count;
  atomic_int result;
} cancel_wait_state_t;

static void interrupt_cancelled_wait(void *arg) {
  cancel_wait_state_t *state = (cancel_wait_state_t *)arg;
  atomic_fetch_add_explicit(&state->callback_count, 1, memory_order_release);
  (void)coro_wait_interrupt(state->wait, TURBO_ECANCELED);
}

static void wait_with_cancellation(coro_t *co, void *arg) {
  cancel_wait_state_t *state = (cancel_wait_state_t *)arg;
  int rc;
  (void)co;

  rc = coro_cancel_register(state->token, interrupt_cancelled_wait, state,
                            &state->registration);
  if (rc == TURBO_OK) {
    atomic_store_explicit(&state->entered, 1, memory_order_release);
    rc = coro_wait_for(state->wait, 5000);
    (void)coro_cancel_unregister(state->registration);
    state->registration = NULL;
  }
  atomic_store_explicit(&state->result, rc, memory_order_release);
}

static void count_cancel(void *arg) {
  int *count = (int *)arg;
  (*count)++;
}

spec("coro_cancel") {
  it("interrupts a registered coroutine wait exactly once") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_cancel_source_t *source = coro_cancel_source_create(ctx);
    cancel_wait_state_t state;
    check_not_null(ctx);
    check_not_null(source);

    state.wait = coro_wait_create(ctx);
    state.token = coro_cancel_source_token(source);
    check_ptr_eq(coro_cancel_token_context(state.token), ctx);
    state.registration = NULL;
    atomic_init(&state.entered, 0);
    atomic_init(&state.callback_count, 0);
    atomic_init(&state.result, TURBO_EALREADY);
    check_not_null(state.wait);
    check_int_eq(coro_context_spawn(ctx, wait_with_cancellation, &state), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_int_eq(atomic_load_explicit(&state.entered, memory_order_acquire), 1);

    check_int_eq(coro_cancel_source_request(source), TURBO_OK);
    check_int_eq(coro_cancel_source_request(source), TURBO_EALREADY);
    while (atomic_load_explicit(&state.result, memory_order_acquire) == TURBO_EALREADY) {
      (void)coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    check_int_eq(atomic_load_explicit(&state.callback_count, memory_order_acquire), 1);
    check_int_eq(atomic_load_explicit(&state.result, memory_order_acquire), TURBO_ECANCELED);
    check_int_eq(coro_wait_destroy(state.wait), TURBO_OK);
    check_int_eq(coro_cancel_source_destroy(source), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("rejects late registration after cancellation") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_cancel_source_t *source = coro_cancel_source_create(ctx);
    coro_cancel_registration_t *registration = NULL;
    int callback_count = 0;
    check_not_null(ctx);
    check_not_null(source);

    check_int_eq(coro_cancel_source_request(source), TURBO_OK);
    check_true(coro_cancel_token_is_requested(coro_cancel_source_token(source)));
    check_int_eq(coro_cancel_register(coro_cancel_source_token(source), count_cancel,
                                      &callback_count, &registration),
                 TURBO_ECANCELED);
    check_null(registration);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_int_eq(callback_count, 0);
    check_int_eq(coro_cancel_source_destroy(source), TURBO_OK);
    coro_context_destroy(ctx);
  }

  it("keeps a source alive while a registration is linked") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_cancel_source_t *source = coro_cancel_source_create(ctx);
    coro_cancel_registration_t *registration = NULL;
    int callback_count = 0;
    check_not_null(ctx);
    check_not_null(source);

    check_int_eq(coro_cancel_register(coro_cancel_source_token(source), count_cancel,
                                      &callback_count, &registration), TURBO_OK);
    check_int_eq(coro_cancel_source_destroy(source), TURBO_EBUSY);
    check_int_eq(coro_cancel_unregister(registration), TURBO_OK);
    check_int_eq(coro_cancel_source_destroy(source), TURBO_OK);
    coro_context_destroy(ctx);
  }
}
