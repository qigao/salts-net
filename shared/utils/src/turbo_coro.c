/**
 * @file turbo_coro.c
 * @brief Coroutine implementation using minicoro
 */

#define MINICORO_IMPL
#include "minicoro.h"
#include "turbo_coro.h"
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Coroutine
// =============================================================================

// Internal coroutine structure
struct turbo_coro_s {
    mco_coro *mco;                       // minicoro handle
    turbo_coro_fn fn;                    // user entry function
    void *arg;                           // user argument
    void *user_data;                     // user data
    turbo_coro_scheduler_t *scheduler;   // owning scheduler (if any)
    turbo_coro_t *next;                  // linked list for scheduler
};

// Wrapper to adapt minicoro callback to our API
static void coro_entry_wrapper(mco_coro *mco) {
    turbo_coro_t *co = (turbo_coro_t *)mco_get_user_data(mco);
    if (co && co->fn) {
        co->fn(co, co->arg);
    }
}

turbo_coro_t *turbo_coro_create(turbo_coro_fn fn, void *arg, const turbo_coro_opts_t *opts) {
    if (!fn) return NULL;

    turbo_coro_t *co = calloc(1, sizeof(turbo_coro_t));
    if (!co) return NULL;

    co->fn = fn;
    co->arg = arg;

    // Setup minicoro descriptor
    mco_desc desc = mco_desc_init(coro_entry_wrapper,
        opts && opts->stack_size ? opts->stack_size : 0);

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

void turbo_coro_destroy(turbo_coro_t *co) {
    if (!co) return;
    if (co->mco) {
        mco_destroy(co->mco);
    }
    free(co);
}

int turbo_coro_resume(turbo_coro_t *co) {
    if (!co || !co->mco) return -1;
    mco_result res = mco_resume(co->mco);
    return (res == MCO_SUCCESS) ? 0 : -1;
}

int turbo_coro_yield(void) {
    mco_coro *mco = mco_running();
    if (!mco) return -1;
    mco_result res = mco_yield(mco);
    return (res == MCO_SUCCESS) ? 0 : -1;
}

turbo_coro_state_t turbo_coro_state(turbo_coro_t *co) {
    if (!co || !co->mco) return TURBO_CORO_DEAD;

    mco_state state = mco_status(co->mco);
    switch (state) {
        case MCO_DEAD:      return TURBO_CORO_DEAD;
        case MCO_NORMAL:    return TURBO_CORO_READY;
        case MCO_RUNNING:   return TURBO_CORO_RUNNING;
        case MCO_SUSPENDED: return TURBO_CORO_SUSPENDED;
        default:            return TURBO_CORO_DEAD;
    }
}

int turbo_coro_alive(turbo_coro_t *co) {
    return turbo_coro_state(co) != TURBO_CORO_DEAD;
}

turbo_coro_t *turbo_coro_running(void) {
    mco_coro *mco = mco_running();
    if (!mco) return NULL;
    return (turbo_coro_t *)mco_get_user_data(mco);
}

void *turbo_coro_get_data(turbo_coro_t *co) {
    return co ? co->user_data : NULL;
}

void turbo_coro_set_data(turbo_coro_t *co, void *data) {
    if (co) co->user_data = data;
}

int turbo_coro_push(turbo_coro_t *co, const void *data, size_t size) {
    if (!co || !co->mco || !data || !size) return -1;
    mco_result res = mco_push(co->mco, data, size);
    return (res == MCO_SUCCESS) ? 0 : -1;
}

int turbo_coro_pop(turbo_coro_t *co, void *data, size_t size) {
    if (!co || !co->mco || !data || !size) return -1;
    mco_result res = mco_pop(co->mco, data, size);
    return (res == MCO_SUCCESS) ? 0 : -1;
}

size_t turbo_coro_bytes_stored(turbo_coro_t *co) {
    if (!co || !co->mco) return 0;
    return mco_get_bytes_stored(co->mco);
}

// =============================================================================
// Scheduler
// =============================================================================

struct turbo_coro_scheduler_s {
    turbo_coro_t *head;      // linked list of coroutines
    turbo_coro_t *tail;
    int count;               // number of alive coroutines
};

// Thread-local current scheduler (for turbo_coro_current_scheduler)
#ifdef _WIN32
static __declspec(thread) turbo_coro_scheduler_t *tls_current_scheduler = NULL;
#else
static __thread turbo_coro_scheduler_t *tls_current_scheduler = NULL;
#endif

turbo_coro_scheduler_t *turbo_coro_scheduler_create(void) {
    turbo_coro_scheduler_t *sched = calloc(1, sizeof(turbo_coro_scheduler_t));
    return sched;
}

void turbo_coro_scheduler_destroy(turbo_coro_scheduler_t *sched) {
    if (!sched) return;

    // Destroy all coroutines
    turbo_coro_t *co = sched->head;
    while (co) {
        turbo_coro_t *next = co->next;
        turbo_coro_destroy(co);
        co = next;
    }

    free(sched);
}

turbo_coro_t *turbo_coro_spawn(turbo_coro_scheduler_t *sched, turbo_coro_fn fn, void *arg) {
    if (!sched || !fn) return NULL;

    turbo_coro_t *co = turbo_coro_create(fn, arg, NULL);
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

int turbo_coro_scheduler_tick(turbo_coro_scheduler_t *sched) {
    if (!sched) return 0;

    turbo_coro_scheduler_t *prev_sched = tls_current_scheduler;
    tls_current_scheduler = sched;

    turbo_coro_t *prev = NULL;
    turbo_coro_t *co = sched->head;

    while (co) {
        turbo_coro_t *next = co->next;

        if (turbo_coro_alive(co)) {
            // Resume coroutine
            turbo_coro_resume(co);

            // Check if still alive after resume
            if (!turbo_coro_alive(co)) {
                // Remove from list
                if (prev) {
                    prev->next = next;
                } else {
                    sched->head = next;
                }
                if (co == sched->tail) {
                    sched->tail = prev;
                }
                sched->count--;
                turbo_coro_destroy(co);
            } else {
                prev = co;
            }
        } else {
            // Already dead, remove
            if (prev) {
                prev->next = next;
            } else {
                sched->head = next;
            }
            if (co == sched->tail) {
                sched->tail = prev;
            }
            sched->count--;
            turbo_coro_destroy(co);
        }

        co = next;
    }

    tls_current_scheduler = prev_sched;
    return sched->count;
}

void turbo_coro_scheduler_run(turbo_coro_scheduler_t *sched) {
    if (!sched) return;

    while (turbo_coro_scheduler_tick(sched) > 0) {
        // Keep running until all coroutines complete
    }
}

int turbo_coro_scheduler_count(turbo_coro_scheduler_t *sched) {
    return sched ? sched->count : 0;
}

turbo_coro_scheduler_t *turbo_coro_current_scheduler(void) {
    return tls_current_scheduler;
}
