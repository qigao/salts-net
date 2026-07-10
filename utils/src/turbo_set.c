#include "turbo_set.h"

#include <stdint.h>

int turbo_set_init(turbo_set_t *set, size_t key_size, turbo_hash_fn hash,
                   turbo_hash_equal_fn equal, void *ctx) {
  if (!set) return TURBO_EINVAL;
  return turbo_hash_map_init(&set->map, key_size, sizeof(uint8_t), hash, equal, ctx);
}

void turbo_set_destroy(turbo_set_t *set) {
  if (!set) return;
  turbo_hash_map_destroy(&set->map);
}

void turbo_set_clear(turbo_set_t *set) {
  if (!set) return;
  turbo_hash_map_clear(&set->map);
}

int turbo_set_reserve(turbo_set_t *set, size_t min_capacity) {
  if (!set) return TURBO_EINVAL;
  return turbo_hash_map_reserve(&set->map, min_capacity);
}

int turbo_set_add(turbo_set_t *set, const void *key) {
  uint8_t present = 1U;
  if (!set) return TURBO_EINVAL;
  return turbo_hash_map_put(&set->map, key, &present);
}

bool turbo_set_contains(const turbo_set_t *set, const void *key) {
  if (!set) return false;
  return turbo_hash_map_contains(&set->map, key);
}

int turbo_set_remove(turbo_set_t *set, const void *key) {
  if (!set) return TURBO_EINVAL;
  return turbo_hash_map_remove(&set->map, key, NULL);
}

size_t turbo_set_size(const turbo_set_t *set) {
  if (!set) return 0;
  return turbo_hash_map_size(&set->map);
}

size_t turbo_set_capacity(const turbo_set_t *set) {
  if (!set) return 0;
  return turbo_hash_map_capacity(&set->map);
}

bool turbo_set_empty(const turbo_set_t *set) {
  return set == NULL || turbo_hash_map_empty(&set->map);
}

const void *turbo_set_key_at(const turbo_set_t *set, size_t slot) {
  if (!set) return NULL;
  return turbo_hash_map_key_at(&set->map, slot);
}
