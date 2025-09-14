/**
 * @file json_types.h
 * @brief JSON Parser Internal Types with MemoryPool Integration
 */

#ifndef JSON_TYPES_H
#define JSON_TYPES_H

#include "json_parser.h"
#include "memory_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_POOL_MIN_SIZE (4 * 1024)
#define JSON_POOL_MAX_SIZE (16 * 1024 * 1024)

typedef struct json_pool_node_s {
    MemoryPool *pool;
    struct json_pool_node_s *next;
} json_pool_node_t;

typedef struct json_arena_s {
    json_pool_node_t *head;
    json_pool_node_t *current;
    size_t initial_size;
    int external;  // If true, don't destroy pools
} json_arena_t;

typedef struct json_pair_s {
    const char *key;
    size_t key_len;
    int key_owned;  // 0 = zero-copy, 1 = arena-allocated
    struct json_value_s *value;
    struct json_pair_s *next;
} json_pair_t;

typedef struct json_element_s {
    struct json_value_s *value;
    struct json_element_s *next;
} json_element_t;

struct json_value_s {
    json_type_t type;
    json_arena_t *arena;
    union {
        bool bool_val;
        double num_val;
        struct {
            const char *str;
            size_t len;
            int owned;  // 0 = zero-copy (points to input), 1 = arena-allocated
        } string_val;
        struct {
            json_pair_t *pairs;
            json_pair_t *pairs_tail;
            size_t count;
        } object_val;
        struct {
            json_element_t *elements;
            json_element_t *elements_tail;
            size_t count;
        } array_val;
    } data;
};

typedef struct json_value_s json_value_t;

typedef struct {
    json_value_t *root;
    json_arena_t *arena;
    int error;
    char error_msg[256];
} json_parse_ctx_t;

json_arena_t *json_arena_create(void);
json_arena_t *json_arena_create_sized(size_t hint_size);
json_arena_t *json_arena_create_with_pool(MemoryPool *pool);
void         *json_arena_alloc(json_arena_t *arena, size_t size);
char         *json_arena_strdup(json_arena_t *arena, const char *str, size_t len);
void          json_arena_free(json_arena_t *arena);
size_t        json_arena_used(json_arena_t *arena);
size_t        json_arena_peak(json_arena_t *arena);

json_value_t *json_value_new_arena(json_arena_t *arena, json_type_t type);
json_value_t *json_value_null_arena(json_arena_t *arena);
json_value_t *json_value_bool_arena(json_arena_t *arena, bool val);
json_value_t *json_value_number_arena(json_arena_t *arena, double val);
json_value_t *json_value_string_arena(json_arena_t *arena, const char *str, size_t len);
json_value_t *json_value_array_arena(json_arena_t *arena);
json_value_t *json_value_object_arena(json_arena_t *arena);

void json_array_append_arena(json_arena_t *arena, json_value_t *arr, json_value_t *val);
void json_object_set_arena(json_arena_t *arena, json_value_t *obj, const char *key, size_t key_len, json_value_t *val);
void json_object_set_arena_ex(json_arena_t *arena, json_value_t *obj, const char *key, size_t key_len, int key_owned, json_value_t *val);

#ifdef __cplusplus
}
#endif

#endif /* JSON_TYPES_H */
