/**
 * @file exprtk_dispatch.h
 * @brief Sorted dispatch table + binary search for O(log n) built-in lookup.
 */
#ifndef EXPRTK_DISPATCH_H
#define EXPRTK_DISPATCH_H

#include "exprtk_internal.h"
#include "arena_buffer.h"

/* Sorted enum + table generated from function names */
#include "exprtk_dispatch_table.inc"

/* Binary search comparator */
static inline int exprtk_dispatch_cmp(const void *key, const void *entry) {
    typedef struct { const char *name; int id; } entry_t;
    return strcmp((const char *)key, ((const entry_t *)entry)->name);
}

/**
 * @brief Look up a built-in function name and return its ID.
 * @return The builtin ID (>= 0), or -1 if not found.
 */
static inline int exprtk_dispatch_find(const char *name) {
    typedef struct { const char *name; int id; } entry_t;
    const size_t count = sizeof(exprtk_builtin_table) / sizeof(exprtk_builtin_table[0]);
    const entry_t *hit = (const entry_t *)bsearch(
        name, exprtk_builtin_table, count,
        sizeof(exprtk_builtin_table[0]), exprtk_dispatch_cmp);
    return hit ? hit->id : -1;
}

/* Shorthand macros for arena array allocation */
#define ALLOC_DBL(arena, n) TURBO_ARENA_ALLOC_ARRAY(arena, double, n)

#endif /* EXPRTK_DISPATCH_H */
