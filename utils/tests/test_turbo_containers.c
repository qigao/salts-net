#include "turbo_containers.h"
#include "tinytest.h"

#include <stdint.h>
#include <string.h>

TURBO_VEC_DEFINE(int_vec_t, int)
TURBO_HASH_MAP_DEFINE(u64_int_map_t, uint64_t, int)
TURBO_SET_DEFINE(u64_set_t, uint64_t)
TURBO_DEQUE_DEFINE(int_deque_t, int)

static int int_min_compare(const void *left, const void *right, void *ctx) {
  int a = *(const int *)left;
  int b = *(const int *)right;
  (void)ctx;
  return (a > b) - (a < b);
}

TURBO_HEAP_DEFINE(int_heap_t, int, int_min_compare)

suite("Turbo Containers") {
  group("Vec") {
    it("pushes, indexes, inserts, erases, and pops generic values") {
      turbo_vec_t vec;
      int value = 0;
      int out = 0;

      check_int_eq(turbo_vec_init(&vec, sizeof(int)), TURBO_OK);
      for (value = 0; value < 8; ++value) {
        check_int_eq(turbo_vec_push(&vec, &value), TURBO_OK);
      }
      check_size_eq(turbo_vec_size(&vec), 8);
      check_int_eq(*(int *)turbo_vec_at(&vec, 3), 3);

      value = 99;
      check_int_eq(turbo_vec_insert(&vec, 4, &value), TURBO_OK);
      check_int_eq(*(int *)turbo_vec_at(&vec, 4), 99);
      check_size_eq(turbo_vec_size(&vec), 9);

      check_int_eq(turbo_vec_erase(&vec, 4, &out), TURBO_OK);
      check_int_eq(out, 99);
      check_int_eq(*(int *)turbo_vec_at(&vec, 4), 4);

      check_int_eq(turbo_vec_pop(&vec, &out), TURBO_OK);
      check_int_eq(out, 7);
      check_size_eq(turbo_vec_size(&vec), 7);

      turbo_vec_destroy(&vec);
    }

    it("supports typed wrappers") {
      int_vec_t vec;
      int out = 0;

      check_int_eq(int_vec_t_init(&vec), TURBO_OK);
      check_int_eq(int_vec_t_push(&vec, 10), TURBO_OK);
      check_int_eq(int_vec_t_push(&vec, 20), TURBO_OK);
      check_size_eq(int_vec_t_size(&vec), 2);
      check_int_eq(*int_vec_t_at(&vec, 1), 20);
      check_true(int_vec_t_pop(&vec, &out));
      check_int_eq(out, 20);
      int_vec_t_destroy(&vec);
    }

    it("zero-fills resize growth") {
      turbo_vec_t vec;

      check_int_eq(turbo_vec_init(&vec, sizeof(int)), TURBO_OK);
      check_int_eq(turbo_vec_resize(&vec, 3), TURBO_OK);
      check_int_eq(*(int *)turbo_vec_at(&vec, 0), 0);
      check_int_eq(*(int *)turbo_vec_at(&vec, 2), 0);
      turbo_vec_destroy(&vec);
    }
  }

  group("Heap") {
    it("pops values according to comparator priority") {
      int_heap_t heap;
      int values[] = {5, 1, 3, 2, 4};
      int expected[] = {1, 2, 3, 4, 5};
      int out = 0;
      size_t i = 0;

      check_int_eq(int_heap_t_init(&heap), TURBO_OK);
      for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        check_int_eq(int_heap_t_push(&heap, values[i]), TURBO_OK);
      }
      check_size_eq(int_heap_t_size(&heap), 5);
      check_int_eq(*int_heap_t_peek(&heap), 1);

      for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        check_true(int_heap_t_pop(&heap, &out));
        check_int_eq(out, expected[i]);
      }
      check_true(int_heap_t_empty(&heap));
      int_heap_t_destroy(&heap);
    }
  }

  group("Hash map") {
    it("puts, updates, gets, removes, and reuses tombstones") {
      u64_int_map_t map;
      uint64_t key = 0;
      int value = 0;
      int removed = 0;
      size_t i = 0;

      check_int_eq(u64_int_map_t_init(&map), TURBO_OK);
      for (i = 0; i < 128; ++i) {
        key = (uint64_t)i;
        value = (int)(i * 10);
        check_int_eq(u64_int_map_t_put(&map, key, value), TURBO_OK);
      }
      check_size_eq(u64_int_map_t_size(&map), 128);

      key = 42;
      check_not_null(u64_int_map_t_get(&map, key));
      check_int_eq(*u64_int_map_t_get(&map, key), 420);

      value = 777;
      check_int_eq(u64_int_map_t_put(&map, key, value), TURBO_OK);
      check_size_eq(u64_int_map_t_size(&map), 128);
      check_int_eq(*u64_int_map_t_get(&map, key), 777);

      check_true(u64_int_map_t_remove(&map, key, &removed));
      check_int_eq(removed, 777);
      check_false(u64_int_map_t_contains(&map, key));
      check_size_eq(u64_int_map_t_size(&map), 127);

      check_int_eq(u64_int_map_t_put(&map, key, 4242), TURBO_OK);
      check_int_eq(*u64_int_map_t_get(&map, key), 4242);
      u64_int_map_t_destroy(&map);
    }

    it("supports byte keys with custom value structs") {
      typedef struct {
        int id;
        char tag[8];
      } item_t;

      turbo_hash_map_t map;
      const char key1[4] = {'a', 'b', 'c', '\0'};
      const char key2[4] = {'x', 'y', 'z', '\0'};
      item_t item = {1, "one"};
      const item_t *found = NULL;

      check_int_eq(turbo_hash_map_init(&map, sizeof(key1), sizeof(item), NULL, NULL, NULL),
                   TURBO_OK);
      check_int_eq(turbo_hash_map_put(&map, key1, &item), TURBO_OK);
      item.id = 2;
      strcpy(item.tag, "two");
      check_int_eq(turbo_hash_map_put(&map, key2, &item), TURBO_OK);

      found = (const item_t *)turbo_hash_map_get_const(&map, key2);
      check_not_null(found);
      check_int_eq(found->id, 2);
      check_str_eq(found->tag, "two");
      turbo_hash_map_destroy(&map);
    }
  }

  group("Set") {
    it("adds, deduplicates, contains, and removes generic keys") {
      turbo_set_t set;
      uint64_t key = 0;
      size_t i = 0;

      check_int_eq(turbo_set_init(&set, sizeof(key), NULL, NULL, NULL), TURBO_OK);
      for (i = 0; i < 128; ++i) {
        key = (uint64_t)i;
        check_int_eq(turbo_set_add(&set, &key), TURBO_OK);
      }
      check_size_eq(turbo_set_size(&set), 128);

      key = 42;
      check_true(turbo_set_contains(&set, &key));
      check_int_eq(turbo_set_add(&set, &key), TURBO_OK);
      check_size_eq(turbo_set_size(&set), 128);

      check_int_eq(turbo_set_remove(&set, &key), TURBO_OK);
      check_false(turbo_set_contains(&set, &key));
      check_size_eq(turbo_set_size(&set), 127);
      check_int_eq(turbo_set_remove(&set, &key), TURBO_ENOENT);

      turbo_set_destroy(&set);
    }

    it("supports typed wrappers") {
      u64_set_t set;

      check_int_eq(u64_set_t_init(&set), TURBO_OK);
      check_int_eq(u64_set_t_reserve(&set, 32), TURBO_OK);
      check_int_eq(u64_set_t_add(&set, 7), TURBO_OK);
      check_int_eq(u64_set_t_add(&set, 11), TURBO_OK);
      check_true(u64_set_t_contains(&set, 7));
      check_false(u64_set_t_contains(&set, 5));
      check_true(u64_set_t_remove(&set, 7));
      check_false(u64_set_t_remove(&set, 7));
      check_size_eq(u64_set_t_size(&set), 1);
      check_true(u64_set_t_capacity(&set) >= 32);
      u64_set_t_destroy(&set);
    }
  }

  group("Deque") {
    it("pushes and pops from both ends in logical order") {
      turbo_deque_t deque;
      int value = 0;
      int out = 0;

      check_int_eq(turbo_deque_init(&deque, sizeof(int)), TURBO_OK);
      value = 1;
      check_int_eq(turbo_deque_push_back(&deque, &value), TURBO_OK);
      value = 2;
      check_int_eq(turbo_deque_push_back(&deque, &value), TURBO_OK);
      value = 0;
      check_int_eq(turbo_deque_push_front(&deque, &value), TURBO_OK);
      value = -1;
      check_int_eq(turbo_deque_push_front(&deque, &value), TURBO_OK);

      check_size_eq(turbo_deque_size(&deque), 4);
      check_int_eq(*(int *)turbo_deque_front(&deque), -1);
      check_int_eq(*(int *)turbo_deque_back(&deque), 2);
      check_int_eq(*(int *)turbo_deque_at(&deque, 0), -1);
      check_int_eq(*(int *)turbo_deque_at(&deque, 1), 0);
      check_int_eq(*(int *)turbo_deque_at(&deque, 2), 1);
      check_int_eq(*(int *)turbo_deque_at(&deque, 3), 2);

      check_int_eq(turbo_deque_pop_front(&deque, &out), TURBO_OK);
      check_int_eq(out, -1);
      check_int_eq(turbo_deque_pop_back(&deque, &out), TURBO_OK);
      check_int_eq(out, 2);
      check_size_eq(turbo_deque_size(&deque), 2);

      turbo_deque_destroy(&deque);
    }

    it("preserves order across wrap-around and growth") {
      int_deque_t deque;
      const int_deque_t *const_deque = NULL;
      int out = 0;
      int i = 0;

      check_int_eq(int_deque_t_init(&deque), TURBO_OK);
      check_int_eq(turbo_deque_pop_front(&deque.raw, &out), TURBO_ENOENT);
      check_int_eq(turbo_deque_pop_back(&deque.raw, &out), TURBO_ENOENT);
      check_int_eq(int_deque_t_reserve(&deque, 4), TURBO_OK);
      for (i = 0; i < 4; ++i) {
        check_int_eq(int_deque_t_push_back(&deque, i), TURBO_OK);
      }
      check_true(int_deque_t_pop_front(&deque, &out));
      check_int_eq(out, 0);
      check_true(int_deque_t_pop_front(&deque, &out));
      check_int_eq(out, 1);

      check_int_eq(int_deque_t_push_back(&deque, 4), TURBO_OK);
      check_int_eq(int_deque_t_push_back(&deque, 5), TURBO_OK);
      check_int_eq(int_deque_t_push_front(&deque, 1), TURBO_OK);
      check_int_eq(int_deque_t_push_back(&deque, 6), TURBO_OK);

      check_size_eq(int_deque_t_size(&deque), 6);
      const_deque = &deque;
      check_int_eq(*int_deque_t_front_const(const_deque), 1);
      check_int_eq(*int_deque_t_back_const(const_deque), 6);
      for (i = 0; i < 6; ++i) {
        check_int_eq(*int_deque_t_at_const(const_deque, (size_t)i), i + 1);
      }

      for (i = 1; i <= 6; ++i) {
        check_true(int_deque_t_pop_front(&deque, &out));
        check_int_eq(out, i);
      }
      check_true(int_deque_t_empty(&deque));
      int_deque_t_destroy(&deque);
    }
  }
}
