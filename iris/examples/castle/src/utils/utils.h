#ifndef UTILS_H
#define UTILS_H

#include "arena_buffer.h"

int compute_reading_time(const char *content);

static inline void free_ctx(turbo_arena_t *arena)
{
    if (!arena)
        return;
    turbo_arena_free(arena);
    free(arena);
}

#endif
