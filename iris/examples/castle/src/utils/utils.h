#ifndef UTILS_H
#define UTILS_H

#include "turbo_buffer.h"

int compute_reading_time(const char *content);

static inline void free_ctx(mem_pool_t *arena)
{
    if (!arena)
        return;
    mem_destroy(arena);
    free(arena);
}

#endif
