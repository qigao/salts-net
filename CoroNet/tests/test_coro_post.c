#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "tinytest.h"
#include "turbo_thread.h"

#include <stdatomic.h>

enum {
  TEST_POST_USABLE_CAPACITY = 16383,
  TEST_POST_PRODUCERS = 4,
  TEST_POSTS_PER_PRODUCER = 4096,
  TEST_POST_TOTAL = TEST_POST_PRODUCERS * TEST_POSTS_PER_PRODUCER,
  TEST_POST_WAKE_ROUNDS = 256,
  TEST_POST_TIMEOUT_MS = 10000,
  TEST_IDLE_EXIT_TIMEOUT_MS = 500
};

static void count_post(void *arg1, void *arg2) {
  (void)arg2;
  atomic_fetch_add_explicit((atomic_int *)arg1, 1, memory_order_release);
}

static int wait_for_value(atomic_int *value, int expected) {
  const uint64_t deadline = turbo_monotonic_ms() + TEST_POST_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) != expected) {
    if (turbo_monotonic_ms() >= deadline) return TURBO_ETIMEDOUT;
    turbo_thread_yield();
  }
  return TURBO_OK;
}

static void context_runner(void *arg) {
  (void)coro_context_run((coro_context_t *)arg, TURBO_RUN_DEFAULT);
}

typedef struct {
  coro_context_t *ctx;
  atomic_int marker;
  atomic_int exited;
} idle_exit_state_t;

static void mark_loop_running(void *arg1, void *arg2) {
  idle_exit_state_t *state = (idle_exit_state_t *)arg1;
  (void)arg2;
  atomic_store_explicit(&state->marker, 1, memory_order_release);
}

static void idle_exit_context_runner(void *arg) {
  idle_exit_state_t *state = (idle_exit_state_t *)arg;
  (void)coro_context_run(state->ctx, TURBO_RUN_DEFAULT);
  atomic_store_explicit(&state->exited, 1, memory_order_release);
}

static int wait_for_idle_exit(idle_exit_state_t *state, turbo_thread_t *thread) {
  const uint64_t deadline = turbo_monotonic_ms() + TEST_IDLE_EXIT_TIMEOUT_MS;
  int rc = TURBO_OK;

  while (!atomic_load_explicit(&state->exited, memory_order_acquire) &&
         turbo_monotonic_ms() < deadline) {
    turbo_thread_yield();
  }
  if (!atomic_load_explicit(&state->exited, memory_order_acquire)) {
    rc = TURBO_ETIMEDOUT;
    coro_context_stop(state->ctx);
  }
  (void)turbo_thread_join(thread);
  turbo_thread_destroy(thread);
  return rc;
}

typedef struct {
  coro_context_t *ctx;
  atomic_int *count;
  atomic_int repost_rc;
} reentrant_post_state_t;

static void reentrant_post(void *arg1, void *arg2) {
  reentrant_post_state_t *state = (reentrant_post_state_t *)arg1;
  (void)arg2;
  atomic_fetch_add_explicit(state->count, 1, memory_order_release);
  atomic_store_explicit(&state->repost_rc,
                        coro_post(state->ctx, count_post, (void *)state->count, NULL),
                        memory_order_release);
}

typedef struct {
  coro_context_t *ctx;
  atomic_int *count;
  atomic_int *done;
  atomic_int *error;
} producer_state_t;

static void post_producer(void *arg) {
  producer_state_t *state = (producer_state_t *)arg;

  for (int i = 0; i < TEST_POSTS_PER_PRODUCER; ++i) {
    int rc;
    do {
      rc = coro_post(state->ctx, count_post, (void *)state->count, NULL);
      if (rc == TURBO_ENOMEM) turbo_thread_yield();
    } while (rc == TURBO_ENOMEM);
    if (rc != TURBO_OK) {
      atomic_store_explicit(state->error, rc, memory_order_release);
      break;
    }
  }
  atomic_fetch_add_explicit(state->done, 1, memory_order_release);
}

spec("coro_post") {
  it("preserves the historical usable queue capacity") {
    coro_context_t *ctx = coro_context_create(NULL);
    atomic_int count;
    check_not_null(ctx);
    atomic_init(&count, 0);

    for (int i = 0; i < TEST_POST_USABLE_CAPACITY; ++i) {
      check_equal(coro_post(ctx, count_post, (void *)&count, NULL), TURBO_OK);
    }
    check_equal(coro_post(ctx, count_post, (void *)&count, NULL), TURBO_ENOMEM);

    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_equal(atomic_load_explicit(&count, memory_order_acquire), TEST_POST_USABLE_CAPACITY);
    check_equal(coro_post(ctx, count_post, (void *)&count, NULL), TURBO_OK);
    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_equal(atomic_load_explicit(&count, memory_order_acquire), TEST_POST_USABLE_CAPACITY + 1);
    coro_context_destroy(ctx);
  }

  it("releases one queue slot before invoking a reentrant callback") {
    coro_context_t *ctx = coro_context_create(NULL);
    reentrant_post_state_t state;
    atomic_int count;
    check_not_null(ctx);
    atomic_init(&count, 0);
    state.ctx = ctx;
    state.count = &count;
    atomic_init(&state.repost_rc, TURBO_EALREADY);

    check_equal(coro_post(ctx, reentrant_post, &state, NULL), TURBO_OK);
    for (int i = 1; i < TEST_POST_USABLE_CAPACITY; ++i) {
      check_equal(coro_post(ctx, count_post, (void *)&count, NULL), TURBO_OK);
    }
    check_equal(coro_post(ctx, count_post, (void *)&count, NULL), TURBO_ENOMEM);

    (void)coro_context_run(ctx, TURBO_RUN_NOWAIT);
    check_equal(atomic_load_explicit(&state.repost_rc, memory_order_acquire), TURBO_OK);
    check_equal(atomic_load_explicit(&count, memory_order_acquire), TEST_POST_USABLE_CAPACITY + 1);
    coro_context_destroy(ctx);
  }

  it("wakes a persistent loop across repeated empty queue transitions") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_thread_t context_thread;
    atomic_int count;
    check_not_null(ctx);
    atomic_init(&count, 0);
    coro_context_set_persistent(ctx, 1);
    check_equal(turbo_thread_create(&context_thread, context_runner, ctx), TURBO_OK);

    for (int i = 1; i <= TEST_POST_WAKE_ROUNDS; ++i) {
      check_equal(coro_post(ctx, count_post, (void *)&count, NULL), TURBO_OK);
      check_equal(wait_for_value(&count, i), TURBO_OK);
    }

    coro_context_set_persistent(ctx, 0);
    coro_context_stop(ctx);
    check_equal(turbo_thread_join(&context_thread), TURBO_OK);
    turbo_thread_destroy(&context_thread);
    coro_context_destroy(ctx);
  }

  it("wakes an idle default run when persistence is disabled") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_thread_t context_thread;
    idle_exit_state_t state;
    int exit_rc;
    check_not_null(ctx);
    state.ctx = ctx;
    atomic_init(&state.marker, 0);
    atomic_init(&state.exited, 0);
    coro_context_set_persistent(ctx, 1);
    check_equal(turbo_thread_create(&context_thread, idle_exit_context_runner, &state),
                 TURBO_OK);
    check_equal(coro_post(ctx, mark_loop_running, &state, NULL), TURBO_OK);
    check_equal(wait_for_value(&state.marker, 1), TURBO_OK);
    turbo_sleep_ms(10u);

    coro_context_set_persistent(ctx, 0);
    exit_rc = wait_for_idle_exit(&state, &context_thread);
    coro_context_destroy(ctx);

    check_equal(exit_rc, TURBO_OK);
  }

  it("wakes an idle default run when its last external reference is released") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_thread_t context_thread;
    idle_exit_state_t state;
    int exit_rc;
    check_not_null(ctx);
    state.ctx = ctx;
    atomic_init(&state.marker, 0);
    atomic_init(&state.exited, 0);
    coro_context_acquire_external(ctx);
    check_equal(turbo_thread_create(&context_thread, idle_exit_context_runner, &state),
                 TURBO_OK);
    check_equal(coro_post(ctx, mark_loop_running, &state, NULL), TURBO_OK);
    check_equal(wait_for_value(&state.marker, 1), TURBO_OK);
    turbo_sleep_ms(10u);

    coro_context_release_external(ctx);
    exit_rc = wait_for_idle_exit(&state, &context_thread);
    coro_context_destroy(ctx);

    check_equal(exit_rc, TURBO_OK);
  }

  it("delivers all callbacks from four concurrent producers") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_thread_t context_thread;
    turbo_thread_t producers[TEST_POST_PRODUCERS];
    producer_state_t states[TEST_POST_PRODUCERS];
    atomic_int count;
    atomic_int done;
    atomic_int error;
    check_not_null(ctx);
    atomic_init(&count, 0);
    atomic_init(&done, 0);
    atomic_init(&error, TURBO_OK);
    coro_context_set_persistent(ctx, 1);
    check_equal(turbo_thread_create(&context_thread, context_runner, ctx), TURBO_OK);

    for (int i = 0; i < TEST_POST_PRODUCERS; ++i) {
      states[i] = (producer_state_t){ctx, &count, &done, &error};
      check_equal(turbo_thread_create(&producers[i], post_producer, &states[i]), TURBO_OK);
    }
    check_equal(wait_for_value(&done, TEST_POST_PRODUCERS), TURBO_OK);
    check_equal(atomic_load_explicit(&error, memory_order_acquire), TURBO_OK);
    check_equal(wait_for_value(&count, TEST_POST_TOTAL), TURBO_OK);

    for (int i = 0; i < TEST_POST_PRODUCERS; ++i) {
      check_equal(turbo_thread_join(&producers[i]), TURBO_OK);
      turbo_thread_destroy(&producers[i]);
    }
    coro_context_set_persistent(ctx, 0);
    coro_context_stop(ctx);
    check_equal(turbo_thread_join(&context_thread), TURBO_OK);
    turbo_thread_destroy(&context_thread);
    coro_context_destroy(ctx);
  }
}
