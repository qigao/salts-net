#include "turbo_deque.h"

#include <stdlib.h>
#include <string.h>

#define TURBO_DEQUE_MIN_CAPACITY 8U

static int turbo_deque_valid(const turbo_deque_t *deque) {
  return deque != NULL && deque->elem_size > 0;
}

static size_t turbo_deque_physical_index(const turbo_deque_t *deque, size_t logical_index) {
  return (deque->head + logical_index) % deque->capacity;
}

static unsigned char *turbo_deque_slot(turbo_deque_t *deque, size_t physical_index) {
  return (unsigned char *)deque->data + physical_index * deque->elem_size;
}

static const unsigned char *turbo_deque_slot_const(const turbo_deque_t *deque,
                                                   size_t physical_index) {
  return (const unsigned char *)deque->data + physical_index * deque->elem_size;
}

static int turbo_deque_grow_to(turbo_deque_t *deque, size_t min_capacity) {
  size_t new_capacity;
  unsigned char *new_data;
  size_t i;

  if (!turbo_deque_valid(deque)) return TURBO_EINVAL;
  if (min_capacity <= deque->capacity) return TURBO_OK;
  if (min_capacity > SIZE_MAX / deque->elem_size) return TURBO_ENOMEM;

  new_capacity = deque->capacity ? deque->capacity : TURBO_DEQUE_MIN_CAPACITY;
  while (new_capacity < min_capacity) {
    if (new_capacity > SIZE_MAX / 2U) {
      new_capacity = min_capacity;
      break;
    }
    new_capacity *= 2U;
  }
  if (new_capacity > SIZE_MAX / deque->elem_size) return TURBO_ENOMEM;

  new_data = (unsigned char *)malloc(new_capacity * deque->elem_size);
  if (!new_data) return TURBO_ENOMEM;

  for (i = 0; i < deque->size; ++i) {
    size_t old_index = turbo_deque_physical_index(deque, i);
    memcpy(new_data + i * deque->elem_size, turbo_deque_slot_const(deque, old_index),
           deque->elem_size);
  }

  free(deque->data);
  deque->data = new_data;
  deque->capacity = new_capacity;
  deque->head = 0;
  return TURBO_OK;
}

int turbo_deque_init(turbo_deque_t *deque, size_t elem_size) {
  if (!deque || elem_size == 0) return TURBO_EINVAL;
  memset(deque, 0, sizeof(*deque));
  deque->elem_size = elem_size;
  return TURBO_OK;
}

void turbo_deque_destroy(turbo_deque_t *deque) {
  if (!deque) return;
  free(deque->data);
  memset(deque, 0, sizeof(*deque));
}

void turbo_deque_clear(turbo_deque_t *deque) {
  if (!deque) return;
  deque->size = 0;
  deque->head = 0;
}

int turbo_deque_reserve(turbo_deque_t *deque, size_t min_capacity) {
  return turbo_deque_grow_to(deque, min_capacity);
}

int turbo_deque_push_back(turbo_deque_t *deque, const void *elem) {
  int rc;
  size_t index;

  if (!turbo_deque_valid(deque) || !elem) return TURBO_EINVAL;
  if (deque->size == SIZE_MAX) return TURBO_ENOMEM;
  rc = turbo_deque_grow_to(deque, deque->size + 1U);
  if (rc != TURBO_OK) return rc;
  index = turbo_deque_physical_index(deque, deque->size);
  memcpy(turbo_deque_slot(deque, index), elem, deque->elem_size);
  deque->size += 1U;
  return TURBO_OK;
}

int turbo_deque_push_front(turbo_deque_t *deque, const void *elem) {
  int rc;

  if (!turbo_deque_valid(deque) || !elem) return TURBO_EINVAL;
  if (deque->size == SIZE_MAX) return TURBO_ENOMEM;
  rc = turbo_deque_grow_to(deque, deque->size + 1U);
  if (rc != TURBO_OK) return rc;
  deque->head = deque->head == 0 ? deque->capacity - 1U : deque->head - 1U;
  memcpy(turbo_deque_slot(deque, deque->head), elem, deque->elem_size);
  deque->size += 1U;
  return TURBO_OK;
}

int turbo_deque_pop_back(turbo_deque_t *deque, void *out_elem) {
  size_t index;

  if (!turbo_deque_valid(deque)) return TURBO_EINVAL;
  if (deque->size == 0) return TURBO_ENOENT;
  index = turbo_deque_physical_index(deque, deque->size - 1U);
  if (out_elem) memcpy(out_elem, turbo_deque_slot(deque, index), deque->elem_size);
  deque->size -= 1U;
  if (deque->size == 0) deque->head = 0;
  return TURBO_OK;
}

int turbo_deque_pop_front(turbo_deque_t *deque, void *out_elem) {
  if (!turbo_deque_valid(deque)) return TURBO_EINVAL;
  if (deque->size == 0) return TURBO_ENOENT;
  if (out_elem) memcpy(out_elem, turbo_deque_slot(deque, deque->head), deque->elem_size);
  deque->head = (deque->head + 1U) % deque->capacity;
  deque->size -= 1U;
  if (deque->size == 0) deque->head = 0;
  return TURBO_OK;
}

void *turbo_deque_front(turbo_deque_t *deque) {
  if (!turbo_deque_valid(deque) || deque->size == 0) return NULL;
  return turbo_deque_slot(deque, deque->head);
}

const void *turbo_deque_front_const(const turbo_deque_t *deque) {
  if (!turbo_deque_valid(deque) || deque->size == 0) return NULL;
  return turbo_deque_slot_const(deque, deque->head);
}

void *turbo_deque_back(turbo_deque_t *deque) {
  size_t index;
  if (!turbo_deque_valid(deque) || deque->size == 0) return NULL;
  index = turbo_deque_physical_index(deque, deque->size - 1U);
  return turbo_deque_slot(deque, index);
}

const void *turbo_deque_back_const(const turbo_deque_t *deque) {
  size_t index;
  if (!turbo_deque_valid(deque) || deque->size == 0) return NULL;
  index = turbo_deque_physical_index(deque, deque->size - 1U);
  return turbo_deque_slot_const(deque, index);
}

void *turbo_deque_at(turbo_deque_t *deque, size_t index) {
  if (!turbo_deque_valid(deque) || index >= deque->size) return NULL;
  return turbo_deque_slot(deque, turbo_deque_physical_index(deque, index));
}

const void *turbo_deque_at_const(const turbo_deque_t *deque, size_t index) {
  if (!turbo_deque_valid(deque) || index >= deque->size) return NULL;
  return turbo_deque_slot_const(deque, turbo_deque_physical_index(deque, index));
}

size_t turbo_deque_size(const turbo_deque_t *deque) {
  if (!deque) return 0;
  return deque->size;
}

size_t turbo_deque_capacity(const turbo_deque_t *deque) {
  if (!deque) return 0;
  return deque->capacity;
}

bool turbo_deque_empty(const turbo_deque_t *deque) {
  return deque == NULL || deque->size == 0;
}
