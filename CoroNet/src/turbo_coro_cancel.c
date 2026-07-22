#include "CoroNet/turbo_coro_cancel.h"

#include "CoroNet/turbo_coro_context.h"
#include "turbo_thread.h"

#include <stdatomic.h>
#include <stdlib.h>

struct coro_cancel_token_s {
  struct coro_cancel_source_s *source;
};

struct coro_cancel_registration_s {
  struct coro_cancel_source_s *source;
  coro_cancel_fn fn;
  void *arg;
  struct coro_cancel_registration_s *next;
  int linked;
};

struct coro_cancel_source_s {
  coro_context_t *ctx;
  struct coro_cancel_token_s token;
  struct coro_cancel_registration_s *registrations;
  atomic_int requested;
  atomic_int dispatch_pending;
};

static void coro_cancel_dispatch(void *arg1, void *arg2) {
  coro_cancel_source_t *source = (coro_cancel_source_t *)arg1;
  coro_cancel_registration_t *registration;
  (void)arg2;

  registration = source->registrations;
  source->registrations = NULL;
  atomic_store_explicit(&source->dispatch_pending, 0, memory_order_release);

  while (registration) {
    coro_cancel_registration_t *next = registration->next;
    coro_cancel_fn fn = registration->fn;
    void *arg = registration->arg;
    registration->source = NULL;
    registration->next = NULL;
    registration->linked = 0;
    fn(arg);
    registration = next;
  }
}

coro_cancel_source_t *coro_cancel_source_create(coro_context_t *ctx) {
  coro_cancel_source_t *source;
  if (!ctx) return NULL;

  source = (coro_cancel_source_t *)calloc(1, sizeof(*source));
  if (!source) return NULL;
  source->ctx = ctx;
  source->token.source = source;
  atomic_init(&source->requested, 0);
  atomic_init(&source->dispatch_pending, 0);
  return source;
}

int coro_cancel_source_destroy(coro_cancel_source_t *source) {
  if (!source) return TURBO_EINVAL;
  if (source->registrations ||
      atomic_load_explicit(&source->dispatch_pending, memory_order_acquire)) {
    return TURBO_EBUSY;
  }
  source->token.source = NULL;
  free(source);
  return TURBO_OK;
}

const coro_cancel_token_t *coro_cancel_source_token(coro_cancel_source_t *source) {
  return source ? &source->token : NULL;
}

int coro_cancel_source_request(coro_cancel_source_t *source) {
  int expected = 0;
  int rc;
  if (!source || !source->ctx) return TURBO_EINVAL;

  if (!atomic_compare_exchange_strong_explicit(&source->requested, &expected, 1,
                                               memory_order_acq_rel,
                                               memory_order_acquire)) {
    return TURBO_EALREADY;
  }

  atomic_store_explicit(&source->dispatch_pending, 1, memory_order_release);
  do {
    rc = coro_post(source->ctx, coro_cancel_dispatch, source, NULL);
    if (rc == TURBO_ENOMEM) turbo_thread_yield();
  } while (rc == TURBO_ENOMEM);

  if (rc != TURBO_OK) {
    atomic_store_explicit(&source->dispatch_pending, 0, memory_order_release);
    return rc;
  }
  return TURBO_OK;
}

int coro_cancel_token_is_requested(const coro_cancel_token_t *token) {
  return token && token->source &&
         atomic_load_explicit(&token->source->requested, memory_order_acquire) != 0;
}

coro_context_t *coro_cancel_token_context(const coro_cancel_token_t *token) {
  return token && token->source ? token->source->ctx : NULL;
}

int coro_cancel_register(const coro_cancel_token_t *token, coro_cancel_fn fn,
                         void *arg,
                         coro_cancel_registration_t **out_registration) {
  coro_cancel_source_t *source;
  coro_cancel_registration_t *registration;
  if (out_registration) *out_registration = NULL;
  if (!token || !token->source || !fn || !out_registration) return TURBO_EINVAL;

  source = token->source;
  if (atomic_load_explicit(&source->requested, memory_order_acquire)) {
    return TURBO_ECANCELED;
  }

  registration = (coro_cancel_registration_t *)calloc(1, sizeof(*registration));
  if (!registration) return TURBO_ENOMEM;
  registration->source = source;
  registration->fn = fn;
  registration->arg = arg;
  registration->next = source->registrations;
  registration->linked = 1;
  source->registrations = registration;

  *out_registration = registration;
  return TURBO_OK;
}

int coro_cancel_unregister(coro_cancel_registration_t *registration) {
  coro_cancel_registration_t **link;
  if (!registration) return TURBO_EINVAL;

  if (registration->linked && registration->source) {
    link = &registration->source->registrations;
    while (*link && *link != registration) link = &(*link)->next;
    if (*link != registration) return TURBO_EINVAL;
    *link = registration->next;
  }

  registration->source = NULL;
  registration->next = NULL;
  registration->linked = 0;
  free(registration);
  return TURBO_OK;
}
