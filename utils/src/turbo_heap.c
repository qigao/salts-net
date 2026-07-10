#include "turbo_heap.h"

#include <stdlib.h>
#include <string.h>

#define TURBO_HEAP_MIN_CAPACITY 8U

static int turbo_heap_valid(const turbo_heap_t *heap) {
  return heap != NULL && heap->elem_size > 0 && heap->compare != NULL;
}

static unsigned char *turbo_heap_elem(turbo_heap_t *heap, size_t index) {
  return (unsigned char *)heap->data + index * heap->elem_size;
}

static const unsigned char *turbo_heap_elem_const(const turbo_heap_t *heap, size_t index) {
  return (const unsigned char *)heap->data + index * heap->elem_size;
}

static int turbo_heap_less(const turbo_heap_t *heap, size_t left, size_t right) {
  return heap->compare(turbo_heap_elem_const(heap, left), turbo_heap_elem_const(heap, right),
                       heap->compare_ctx) < 0;
}

static int turbo_heap_grow_to(turbo_heap_t *heap, size_t min_capacity) {
  size_t new_capacity;
  void *new_data;

  if (!turbo_heap_valid(heap)) return TURBO_EINVAL;
  if (min_capacity <= heap->capacity) return TURBO_OK;
  if (min_capacity > SIZE_MAX / heap->elem_size) return TURBO_ENOMEM;

  new_capacity = heap->capacity ? heap->capacity : TURBO_HEAP_MIN_CAPACITY;
  while (new_capacity < min_capacity) {
    if (new_capacity > SIZE_MAX / 2U) {
      new_capacity = min_capacity;
      break;
    }
    new_capacity *= 2U;
  }
  if (new_capacity > SIZE_MAX / heap->elem_size) return TURBO_ENOMEM;

  new_data = realloc(heap->data, new_capacity * heap->elem_size);
  if (!new_data) return TURBO_ENOMEM;
  heap->data = new_data;
  heap->capacity = new_capacity;
  return TURBO_OK;
}

static int turbo_heap_swap(turbo_heap_t *heap, size_t a, size_t b) {
  unsigned char stack_tmp[256];
  unsigned char *tmp = stack_tmp;
  int heap_allocated = 0;

  if (a == b) return TURBO_OK;
  if (heap->elem_size > sizeof(stack_tmp)) {
    tmp = (unsigned char *)malloc(heap->elem_size);
    if (!tmp) return TURBO_ENOMEM;
    heap_allocated = 1;
  }

  memcpy(tmp, turbo_heap_elem(heap, a), heap->elem_size);
  memcpy(turbo_heap_elem(heap, a), turbo_heap_elem(heap, b), heap->elem_size);
  memcpy(turbo_heap_elem(heap, b), tmp, heap->elem_size);
  if (heap_allocated) free(tmp);
  return TURBO_OK;
}

static int turbo_heap_sift_up(turbo_heap_t *heap, size_t index) {
  while (index > 0) {
    size_t parent = (index - 1U) / 2U;
    int rc;
    if (!turbo_heap_less(heap, index, parent)) break;
    rc = turbo_heap_swap(heap, index, parent);
    if (rc != TURBO_OK) return rc;
    index = parent;
  }
  return TURBO_OK;
}

static int turbo_heap_sift_down(turbo_heap_t *heap, size_t index) {
  for (;;) {
    size_t left = index * 2U + 1U;
    size_t right = left + 1U;
    size_t best = index;
    int rc;

    if (left < heap->size && turbo_heap_less(heap, left, best)) best = left;
    if (right < heap->size && turbo_heap_less(heap, right, best)) best = right;
    if (best == index) break;
    rc = turbo_heap_swap(heap, index, best);
    if (rc != TURBO_OK) return rc;
    index = best;
  }
  return TURBO_OK;
}

int turbo_heap_init(turbo_heap_t *heap, size_t elem_size, turbo_heap_compare_fn compare,
                    void *compare_ctx) {
  if (!heap || elem_size == 0 || !compare) return TURBO_EINVAL;
  memset(heap, 0, sizeof(*heap));
  heap->elem_size = elem_size;
  heap->compare = compare;
  heap->compare_ctx = compare_ctx;
  return TURBO_OK;
}

void turbo_heap_destroy(turbo_heap_t *heap) {
  if (!heap) return;
  free(heap->data);
  memset(heap, 0, sizeof(*heap));
}

void turbo_heap_clear(turbo_heap_t *heap) {
  if (!heap) return;
  heap->size = 0;
}

int turbo_heap_reserve(turbo_heap_t *heap, size_t min_capacity) {
  return turbo_heap_grow_to(heap, min_capacity);
}

int turbo_heap_push(turbo_heap_t *heap, const void *elem) {
  int rc;

  if (!turbo_heap_valid(heap) || !elem) return TURBO_EINVAL;
  if (heap->size == SIZE_MAX) return TURBO_ENOMEM;
  rc = turbo_heap_grow_to(heap, heap->size + 1U);
  if (rc != TURBO_OK) return rc;
  memcpy(turbo_heap_elem(heap, heap->size), elem, heap->elem_size);
  heap->size += 1U;
  return turbo_heap_sift_up(heap, heap->size - 1U);
}

int turbo_heap_pop(turbo_heap_t *heap, void *out_elem) {
  if (!turbo_heap_valid(heap)) return TURBO_EINVAL;
  if (heap->size == 0) return TURBO_ENOENT;

  if (out_elem) memcpy(out_elem, heap->data, heap->elem_size);
  heap->size -= 1U;
  if (heap->size > 0) {
    memcpy(heap->data, turbo_heap_elem(heap, heap->size), heap->elem_size);
    return turbo_heap_sift_down(heap, 0);
  }
  return TURBO_OK;
}

const void *turbo_heap_peek(const turbo_heap_t *heap) {
  if (!turbo_heap_valid(heap) || heap->size == 0) return NULL;
  return heap->data;
}

size_t turbo_heap_size(const turbo_heap_t *heap) {
  if (!heap) return 0;
  return heap->size;
}

size_t turbo_heap_capacity(const turbo_heap_t *heap) {
  if (!heap) return 0;
  return heap->capacity;
}

bool turbo_heap_empty(const turbo_heap_t *heap) {
  return heap == NULL || heap->size == 0;
}
