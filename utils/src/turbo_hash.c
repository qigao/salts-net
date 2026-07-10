#include "turbo_hash.h"

#include <stdlib.h>
#include <string.h>

#define TURBO_HASH_EMPTY 0U
#define TURBO_HASH_OCCUPIED 1U
#define TURBO_HASH_TOMBSTONE 2U
#define TURBO_HASH_MIN_CAPACITY 16U

size_t turbo_hash_bytes(const void *key, size_t key_size, void *ctx) {
  const unsigned char *p = (const unsigned char *)key;
  uint64_t h = UINT64_C(1469598103934665603);
  size_t i;
  (void)ctx;
  for (i = 0; i < key_size; ++i) {
    h ^= (uint64_t)p[i];
    h *= UINT64_C(1099511628211);
  }
  if (h == 0) h = 1;
  return (size_t)h;
}

bool turbo_hash_key_equal(const void *left, const void *right, size_t key_size, void *ctx) {
  (void)ctx;
  return memcmp(left, right, key_size) == 0;
}

static int turbo_hash_map_valid(const turbo_hash_map_t *map) {
  return map != NULL && map->key_size > 0 && map->value_size > 0 && map->hash != NULL &&
         map->equal != NULL;
}

static size_t turbo_hash_round_pow2(size_t value) {
  size_t n = TURBO_HASH_MIN_CAPACITY;
  while (n < value) {
    if (n > SIZE_MAX / 2U) return 0;
    n *= 2U;
  }
  return n;
}

static int turbo_hash_alloc_arrays(turbo_hash_map_t *map, size_t capacity) {
  if (capacity > SIZE_MAX / map->key_size) return TURBO_ENOMEM;
  if (capacity > SIZE_MAX / map->value_size) return TURBO_ENOMEM;

  map->states = (uint8_t *)calloc(capacity, sizeof(uint8_t));
  map->hashes = (size_t *)malloc(capacity * sizeof(size_t));
  map->keys = (unsigned char *)malloc(capacity * map->key_size);
  map->values = (unsigned char *)malloc(capacity * map->value_size);
  if (!map->states || !map->hashes || !map->keys || !map->values) {
    free(map->states);
    free(map->hashes);
    free(map->keys);
    free(map->values);
    map->states = NULL;
    map->hashes = NULL;
    map->keys = NULL;
    map->values = NULL;
    return TURBO_ENOMEM;
  }
  map->capacity = capacity;
  return TURBO_OK;
}

static void turbo_hash_insert_occupied(turbo_hash_map_t *map, size_t hash, const void *key,
                                       const void *value) {
  size_t mask = map->capacity - 1U;
  size_t i;
  for (i = 0; i < map->capacity; ++i) {
    size_t slot = (hash + i) & mask;
    if (map->states[slot] == TURBO_HASH_EMPTY) {
      map->states[slot] = TURBO_HASH_OCCUPIED;
      map->hashes[slot] = hash;
      memcpy(map->keys + slot * map->key_size, key, map->key_size);
      memcpy(map->values + slot * map->value_size, value, map->value_size);
      map->size += 1U;
      return;
    }
  }
}

static int turbo_hash_map_rehash(turbo_hash_map_t *map, size_t new_capacity) {
  turbo_hash_map_t next;
  size_t i;
  int rc;

  memset(&next, 0, sizeof(next));
  next.key_size = map->key_size;
  next.value_size = map->value_size;
  next.hash = map->hash;
  next.equal = map->equal;
  next.ctx = map->ctx;

  rc = turbo_hash_alloc_arrays(&next, new_capacity);
  if (rc != TURBO_OK) return rc;

  for (i = 0; i < map->capacity; ++i) {
    if (map->states[i] == TURBO_HASH_OCCUPIED) {
      turbo_hash_insert_occupied(&next, map->hashes[i], map->keys + i * map->key_size,
                                 map->values + i * map->value_size);
    }
  }

  free(map->states);
  free(map->hashes);
  free(map->keys);
  free(map->values);
  map->states = next.states;
  map->hashes = next.hashes;
  map->keys = next.keys;
  map->values = next.values;
  map->size = next.size;
  map->capacity = next.capacity;
  map->tombstones = 0;
  return TURBO_OK;
}

static int turbo_hash_map_ensure_load(turbo_hash_map_t *map, size_t extra) {
  size_t wanted;
  size_t min_slots;
  size_t capacity;
  size_t used;

  if (extra > SIZE_MAX - map->size) return TURBO_ENOMEM;
  wanted = map->size + extra;
  if (map->size > SIZE_MAX - map->tombstones) return TURBO_ENOMEM;
  if (extra > SIZE_MAX - map->size - map->tombstones) return TURBO_ENOMEM;
  used = map->size + map->tombstones + extra;
  if (map->capacity != 0 && used <= map->capacity - (map->capacity / 10U) * 3U) {
    return TURBO_OK;
  }

  if (wanted > (SIZE_MAX - 1U) / 10U) return TURBO_ENOMEM;
  min_slots = (wanted * 10U) / 7U + 1U;
  capacity = turbo_hash_round_pow2(min_slots);
  if (capacity == 0) return TURBO_ENOMEM;
  if (map->capacity <= SIZE_MAX / 2U && capacity < map->capacity * 2U && map->tombstones == 0) {
    capacity = map->capacity ? map->capacity * 2U : capacity;
  }
  if (capacity < TURBO_HASH_MIN_CAPACITY) capacity = TURBO_HASH_MIN_CAPACITY;
  return turbo_hash_map_rehash(map, capacity);
}

static int turbo_hash_map_find_slot(const turbo_hash_map_t *map, const void *key, size_t hash,
                                    size_t *out_slot) {
  size_t mask;
  size_t i;

  if (map->capacity == 0) return 0;
  mask = map->capacity - 1U;
  for (i = 0; i < map->capacity; ++i) {
    size_t slot = (hash + i) & mask;
    uint8_t state = map->states[slot];
    if (state == TURBO_HASH_EMPTY) return 0;
    if (state == TURBO_HASH_OCCUPIED && map->hashes[slot] == hash &&
        map->equal(map->keys + slot * map->key_size, key, map->key_size, map->ctx)) {
      *out_slot = slot;
      return 1;
    }
  }
  return 0;
}

int turbo_hash_map_init(turbo_hash_map_t *map, size_t key_size, size_t value_size,
                        turbo_hash_fn hash, turbo_hash_equal_fn equal, void *ctx) {
  if (!map || key_size == 0 || value_size == 0) return TURBO_EINVAL;
  memset(map, 0, sizeof(*map));
  map->key_size = key_size;
  map->value_size = value_size;
  map->hash = hash ? hash : turbo_hash_bytes;
  map->equal = equal ? equal : turbo_hash_key_equal;
  map->ctx = ctx;
  return TURBO_OK;
}

void turbo_hash_map_destroy(turbo_hash_map_t *map) {
  if (!map) return;
  free(map->states);
  free(map->hashes);
  free(map->keys);
  free(map->values);
  memset(map, 0, sizeof(*map));
}

void turbo_hash_map_clear(turbo_hash_map_t *map) {
  if (!map || !map->states) return;
  memset(map->states, 0, map->capacity * sizeof(uint8_t));
  map->size = 0;
  map->tombstones = 0;
}

int turbo_hash_map_reserve(turbo_hash_map_t *map, size_t min_capacity) {
  size_t min_slots;
  size_t capacity;

  if (!turbo_hash_map_valid(map)) return TURBO_EINVAL;
  if (min_capacity == 0) return TURBO_OK;
  if (min_capacity > (SIZE_MAX - 1U) / 10U) return TURBO_ENOMEM;
  min_slots = (min_capacity * 10U) / 7U + 1U;
  capacity = turbo_hash_round_pow2(min_slots);
  if (capacity == 0) return TURBO_ENOMEM;
  if (capacity <= map->capacity) return TURBO_OK;
  return turbo_hash_map_rehash(map, capacity);
}

int turbo_hash_map_put(turbo_hash_map_t *map, const void *key, const void *value) {
  size_t hash;
  size_t mask;
  size_t first_tombstone = SIZE_MAX;
  size_t i;
  int rc;

  if (!turbo_hash_map_valid(map) || !key || !value) return TURBO_EINVAL;
  rc = turbo_hash_map_ensure_load(map, 1U);
  if (rc != TURBO_OK) return rc;

  hash = map->hash(key, map->key_size, map->ctx);
  if (hash == 0) hash = 1;
  mask = map->capacity - 1U;

  for (i = 0; i < map->capacity; ++i) {
    size_t slot = (hash + i) & mask;
    uint8_t state = map->states[slot];
    if (state == TURBO_HASH_OCCUPIED) {
      if (map->hashes[slot] == hash &&
          map->equal(map->keys + slot * map->key_size, key, map->key_size, map->ctx)) {
        memcpy(map->values + slot * map->value_size, value, map->value_size);
        return TURBO_OK;
      }
    } else if (state == TURBO_HASH_TOMBSTONE) {
      if (first_tombstone == SIZE_MAX) first_tombstone = slot;
    } else {
      size_t target = first_tombstone != SIZE_MAX ? first_tombstone : slot;
      if (map->states[target] == TURBO_HASH_TOMBSTONE) map->tombstones -= 1U;
      map->states[target] = TURBO_HASH_OCCUPIED;
      map->hashes[target] = hash;
      memcpy(map->keys + target * map->key_size, key, map->key_size);
      memcpy(map->values + target * map->value_size, value, map->value_size);
      map->size += 1U;
      return TURBO_OK;
    }
  }

  if (map->capacity > SIZE_MAX / 2U) return TURBO_ENOMEM;
  rc = turbo_hash_map_rehash(map, map->capacity * 2U);
  if (rc != TURBO_OK) return rc;
  return turbo_hash_map_put(map, key, value);
}

void *turbo_hash_map_get(turbo_hash_map_t *map, const void *key) {
  size_t hash;
  size_t slot = 0;

  if (!turbo_hash_map_valid(map) || !key) return NULL;
  hash = map->hash(key, map->key_size, map->ctx);
  if (hash == 0) hash = 1;
  if (!turbo_hash_map_find_slot(map, key, hash, &slot)) return NULL;
  return map->values + slot * map->value_size;
}

const void *turbo_hash_map_get_const(const turbo_hash_map_t *map, const void *key) {
  return turbo_hash_map_get((turbo_hash_map_t *)map, key);
}

bool turbo_hash_map_contains(const turbo_hash_map_t *map, const void *key) {
  return turbo_hash_map_get_const(map, key) != NULL;
}

int turbo_hash_map_remove(turbo_hash_map_t *map, const void *key, void *out_value) {
  size_t hash;
  size_t slot = 0;

  if (!turbo_hash_map_valid(map) || !key) return TURBO_EINVAL;
  hash = map->hash(key, map->key_size, map->ctx);
  if (hash == 0) hash = 1;
  if (!turbo_hash_map_find_slot(map, key, hash, &slot)) return TURBO_ENOENT;
  if (out_value) memcpy(out_value, map->values + slot * map->value_size, map->value_size);
  map->states[slot] = TURBO_HASH_TOMBSTONE;
  map->size -= 1U;
  map->tombstones += 1U;
  if (map->tombstones > map->size && map->capacity > TURBO_HASH_MIN_CAPACITY) {
    (void)turbo_hash_map_rehash(map, map->capacity);
  }
  return TURBO_OK;
}

size_t turbo_hash_map_size(const turbo_hash_map_t *map) {
  if (!map) return 0;
  return map->size;
}

size_t turbo_hash_map_capacity(const turbo_hash_map_t *map) {
  if (!map) return 0;
  return map->capacity;
}

bool turbo_hash_map_empty(const turbo_hash_map_t *map) {
  return map == NULL || map->size == 0;
}

const void *turbo_hash_map_key_at(const turbo_hash_map_t *map, size_t slot) {
  if (!map || slot >= map->capacity || map->states[slot] != TURBO_HASH_OCCUPIED) return NULL;
  return map->keys + slot * map->key_size;
}

void *turbo_hash_map_value_at(turbo_hash_map_t *map, size_t slot) {
  if (!map || slot >= map->capacity || map->states[slot] != TURBO_HASH_OCCUPIED) return NULL;
  return map->values + slot * map->value_size;
}

const void *turbo_hash_map_value_at_const(const turbo_hash_map_t *map, size_t slot) {
  return turbo_hash_map_value_at((turbo_hash_map_t *)map, slot);
}
