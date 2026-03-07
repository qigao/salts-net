/**
 * @file turbo_coro.c
 * @brief Coroutine implementation using minicoro
 */

#define MINICORO_IMPL
#include "turbo_coro.h"
#include "minicoro.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>


// =============================================================================
// Coroutine
// =============================================================================

// Internal coroutine structure
struct coro_s {
  mco_coro *mco;               // minicoro handle
  coro_fn fn;                  // user entry function
  void *arg;                   // user argument
  void *user_data;             // user data
  coro_scheduler_t *scheduler; // owning scheduler (if any)
  coro_t *next;                // linked list for scheduler
  uint8_t waiting_for_io;      // 1 = blocked on I/O, skip in scheduler
};

// Wrapper to adapt minicoro callback to our API
static void coro_entry_wrapper(mco_coro *mco) {
  coro_t *co = (coro_t *)mco_get_user_data(mco);
  if (co && co->fn) {
    co->fn(co, co->arg);
  }
}

coro_t *coro_create(coro_fn fn, void *arg, const coro_opts_t *opts) {
  if (!fn) return NULL;

  coro_t *co = calloc(1, sizeof(coro_t));
  if (!co) return NULL;

  co->fn = fn;
  co->arg = arg;

  // Setup minicoro descriptor
  mco_desc desc =
      mco_desc_init(coro_entry_wrapper, opts && opts->stack_size ? opts->stack_size : 0);

  if (opts && opts->storage_size) {
    desc.storage_size = opts->storage_size;
  }

  desc.user_data = co;

  if (opts) {
    co->user_data = opts->user_data;
  }

  // Create minicoro
  mco_result res = mco_create(&co->mco, &desc);
  if (res != MCO_SUCCESS) {
    free(co);
    return NULL;
  }

  return co;
}

void coro_destroy(coro_t *co) {
  if (!co) return;
  if (co->mco) {
    mco_destroy(co->mco);
  }
  free(co);
}

int coro_resume(coro_t *co) {
  if (!co || !co->mco) return -1;
  mco_result res = mco_resume(co->mco);
  return (res == MCO_SUCCESS) ? 0 : -1;
}

int coro_yield(void) {
  mco_coro *mco = mco_running();
  assert(mco && "coro_yield() must be called from within a coroutine");
  mco_result res = mco_yield(mco);
  return (res == MCO_SUCCESS) ? 0 : -1;
}

coro_state_t coro_state(coro_t *co) {
  if (!co || !co->mco) return coro_DEAD;

  mco_state state = mco_status(co->mco);
  switch (state) {
  case MCO_DEAD:
    return coro_DEAD;
  case MCO_RUNNING:
    return coro_RUNNING;
  case MCO_SUSPENDED:
    return coro_SUSPENDED;
  case MCO_NORMAL:
    /* MCO_NORMAL: this coroutine is alive but not the one currently
       running (it resumed a child coroutine). Treat as SUSPENDED —
       it's alive and will be resumed again when the child yields. */
    return coro_SUSPENDED;
  default:
    return coro_DEAD;
  }
}

int coro_alive(coro_t *co) { return coro_state(co) != coro_DEAD; }

coro_t *coro_running(void) {
  mco_coro *mco = mco_running();
  if (!mco) return NULL;
  return (coro_t *)mco_get_user_data(mco);
}

void *coro_get_data(coro_t *co) { return co ? co->user_data : NULL; }

void coro_set_data(coro_t *co, void *data) {
  if (co) co->user_data = data;
}

int coro_push(coro_t *co, const void *data, size_t size) {
  if (!co || !co->mco || !data || !size) return -1;
  mco_result res = mco_push(co->mco, data, size);
  return (res == MCO_SUCCESS) ? 0 : -1;
}

int coro_pop(coro_t *co, void *data, size_t size) {
  if (!co || !co->mco || !data || !size) return -1;
  mco_result res = mco_pop(co->mco, data, size);
  return (res == MCO_SUCCESS) ? 0 : -1;
}

size_t coro_bytes_stored(coro_t *co) {
  if (!co || !co->mco) return 0;
  return mco_get_bytes_stored(co->mco);
}

// =============================================================================
// Scheduler
// =============================================================================

struct coro_scheduler_s {
  coro_t *head; // linked list of coroutines
  coro_t *tail;
  int count; // number of alive coroutines
};

// Thread-local current scheduler (for coro_current_scheduler)
#ifdef _WIN32
static __declspec(thread) coro_scheduler_t *tls_current_scheduler = NULL;
#else
static __thread coro_scheduler_t *tls_current_scheduler = NULL;
#endif

coro_scheduler_t *coro_scheduler_create(void) {
  coro_scheduler_t *sched = calloc(1, sizeof(coro_scheduler_t));
  return sched;
}

void coro_scheduler_destroy(coro_scheduler_t *sched) {
  if (!sched) return;

  // Assert no live coroutines remain
  assert(sched->count == 0 && "Cannot destroy scheduler with live coroutines");

  free(sched);
}

coro_t *coro_spawn(coro_scheduler_t *sched, coro_fn fn, void *arg, const coro_opts_t *opts) {
  if (!sched || !fn) return NULL;

  coro_t *co = coro_create(fn, arg, opts);
  if (!co) return NULL;

  co->scheduler = sched;
  co->next = NULL;

  // Add to tail
  if (sched->tail) {
    sched->tail->next = co;
  } else {
    sched->head = co;
  }
  sched->tail = co;
  sched->count++;

  return co;
}

int coro_scheduler_tick(coro_scheduler_t *sched) {
  if (!sched) return 0;

  coro_scheduler_t *prev_sched = tls_current_scheduler;
  tls_current_scheduler = sched;

  coro_t *prev = NULL;
  coro_t *co = sched->head;

  /* Count how many coroutines to process in this tick. New coroutines
     spawned during this tick will be deferred to the next tick. */
  int batch_count = sched->count;
  int processed = 0;

  while (co && processed < batch_count) {
    coro_t *next = co->next;
    processed++;

    // Skip coroutines waiting for I/O
    if (co->waiting_for_io) {
      prev = co;
      co = next;
      continue;
    }

    int was_alive = coro_alive(co);

    if (was_alive) {
      // Resume coroutine
      coro_resume(co);

      // Safe to check status: mco struct is valid until mco_destroy().
      int still_alive = coro_alive(co);

      if (!still_alive) {
        // Coroutine finished — remove from list and destroy
        if (prev) {
          prev->next = next;
        } else {
          sched->head = next;
        }
        if (co == sched->tail) {
          sched->tail = prev;
        }
        sched->count--;
        coro_destroy(co);
      } else {
        // Still alive, keep in list
        prev = co;
      }
    } else {
      /* Coroutine already dead — something else resumed it and it finished,
         or it was already cleaned up. Remove from list and destroy here to
         ensure managed lifecycle. */
      if (prev) {
        prev->next = next;
      } else {
        sched->head = next;
      }
      if (co == sched->tail) {
        sched->tail = prev;
      }
      sched->count--;
      coro_destroy(co);
    }

    co = next;
  }

  tls_current_scheduler = prev_sched;
  return sched->count;
}

void coro_scheduler_run(coro_scheduler_t *sched) {
  if (!sched) return;

  while (coro_scheduler_tick(sched) > 0) {
    // Keep running until all coroutines complete
  }
}

int coro_scheduler_count(coro_scheduler_t *sched) { return sched ? sched->count : 0; }

int coro_scheduler_has_ready(coro_scheduler_t *sched) {
  if (!sched || sched->count == 0) return 0;

  coro_t *co = sched->head;
  while (co) {
    if (!co->waiting_for_io) return 1;
    co = co->next;
  }
  return 0;
}

coro_scheduler_t *coro_current_scheduler(void) { return tls_current_scheduler; }

int coro_is_scheduled(coro_t *co) { return (co && co->scheduler) ? 1 : 0; }

void coro_set_waiting_for_io(coro_t *co, int waiting) {
  if (co) co->waiting_for_io = (uint8_t)(waiting ? 1 : 0);
}
