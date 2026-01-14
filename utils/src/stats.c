#include "platform.h"
#include "stats.h"

#include <assert.h>
#include <stb_sprintf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include <stc/cstr.h>
#include "turbo_logger.h"

/* Statistics update message packed into the queue */
typedef struct stats_update_msg_s {
  uint32_t slot; /* index into entries array */
  int64_t value; /* payload */
  uint8_t type;  /* turbo_stat_type_t */
  uint8_t op;    /* 0=set, 1=add, 2=sub, 3=min, 4=max */
} stats_update_msg_t;

/* Define STC Deque for the update queue */
#define i_static
#define i_type UpdateQueue
#define i_key stats_update_msg_t
#include <stc/deque.h>

/* Define STC HashMap for name -> slot index */
#define i_static
#define i_type NameIndex
#define i_key_str
#define i_val uint32_t
#include <stc/hmap.h>



/* Statistics collector (internal implementation) */
struct turbo_stats_s {
  uv_loop_t *loop;
  uv_async_t async_handle;
  uv_mutex_t queue_mutex;
  uv_mutex_t map_mutex;
  uv_timer_t rate_timer;
  int queue_mutex_inited;
  int map_mutex_inited;

  /* Update queue (thread-safe) */
  UpdateQueue update_queue;
  size_t update_queue_max_size;

  /* Statistics storage */
  /* Array-backed storage for cache-friendly access */
  turbo_stat_entry_t *entries; /* points to contiguous array */
  size_t entry_capacity;
  size_t entry_count;
  /* Bitmaps to track slot usage and dirtiness */
  uint64_t *used_bitmap;
  uint64_t *dirty_bitmap;
  size_t bitmap_words;
  /* Lookup table: name -> slot index */
  NameIndex name_index;
  /* Head of iteration list (reuses next pointers inside entries) */
  turbo_stat_entry_t *entries_head;

  /* Configuration */
  int initialized;
  int rate_interval_ms;
};

/* Global statistics instance */
turbo_stats_t *g_turbo_stats = NULL;

/* Tunables */
#define STATS_DEFAULT_CAPACITY 512
#define STATS_UPDATE_QUEUE_MAX_SIZE 4096

/* Bitmap helpers */
static inline size_t bitmap_words(size_t cap) { return (cap + 63) / 64; }
static inline void bitmap_set(uint64_t *bm, size_t idx) {
  bm[idx / 64] |= (uint64_t)1 << (idx % 64);
}
static inline void bitmap_clear(uint64_t *bm, size_t idx) {
  bm[idx / 64] &= ~((uint64_t)1 << (idx % 64));
}
static inline int bitmap_test(const uint64_t *bm, size_t idx) {
  return (int)((bm[idx / 64] >> (idx % 64)) & 1U);
}

/* Forward declarations */
static void on_async_update(uv_async_t *handle);
static void on_rate_timer(uv_timer_t *handle);
static turbo_stat_entry_t *find_or_create_entry(const char *name, turbo_stat_type_t type,
                                                uint32_t *slot_out);
static void process_update_queue(void);
static uint64_t get_timestamp_ms(void);
static uint32_t alloc_slot(void);

int turbo_stats_init(void *loop, size_t update_pool_size) {
  (void)update_pool_size; /* legacy arg ignored in ringbuffer mode */

  if (g_turbo_stats) {
    return -1; /* Already initialized */
  }

  g_turbo_stats = (turbo_stats_t *)calloc(1, sizeof(turbo_stats_t));
  if (!g_turbo_stats) {
    return -1;
  }

  g_turbo_stats->loop = (uv_loop_t *)loop;
  g_turbo_stats->rate_interval_ms = 1000; /* 1 second default */
  g_turbo_stats->entry_capacity = STATS_DEFAULT_CAPACITY;
  g_turbo_stats->bitmap_words = bitmap_words(g_turbo_stats->entry_capacity);

  g_turbo_stats->entries =
      (turbo_stat_entry_t *)calloc(g_turbo_stats->entry_capacity, sizeof(turbo_stat_entry_t));
  g_turbo_stats->used_bitmap = (uint64_t *)calloc(g_turbo_stats->bitmap_words, sizeof(uint64_t));
  g_turbo_stats->dirty_bitmap = (uint64_t *)calloc(g_turbo_stats->bitmap_words, sizeof(uint64_t));
  g_turbo_stats->update_queue_max_size = STATS_UPDATE_QUEUE_MAX_SIZE;
  g_turbo_stats->update_queue = UpdateQueue_init();
  g_turbo_stats->name_index = NameIndex_init();
  g_turbo_stats->entries_head = NULL;

  /* Initialize mutex */
  int rc = uv_mutex_init(&g_turbo_stats->queue_mutex);
  if (rc != 0) {
    turbo_stats_cleanup();
    return rc;
  }
  g_turbo_stats->queue_mutex_inited = 1;

  rc = uv_mutex_init(&g_turbo_stats->map_mutex);
  if (rc != 0) {
    turbo_stats_cleanup();
    return rc;
  }
  g_turbo_stats->map_mutex_inited = 1;

  /* Initialize async handle */
  rc = uv_async_init(loop, &g_turbo_stats->async_handle, on_async_update);
  if (rc != 0) {
    turbo_stats_cleanup();
    return rc;
  }

  /* Initialize rate calculation timer */
  rc = uv_timer_init(loop, &g_turbo_stats->rate_timer);
  if (rc != 0) {
    turbo_stats_cleanup();
    return rc;
  }

  /* Start rate calculation timer */
  uv_timer_start(&g_turbo_stats->rate_timer, on_rate_timer, g_turbo_stats->rate_interval_ms,
                 g_turbo_stats->rate_interval_ms);

  g_turbo_stats->initialized = 1;
  return 0;
}

void turbo_stats_cleanup(void) {
  if (!g_turbo_stats)
    return;

  g_turbo_stats->initialized = 0;

  /* Stop timer and process remaining updates */
  uv_timer_stop(&g_turbo_stats->rate_timer);
  process_update_queue();

  NameIndex_drop(&g_turbo_stats->name_index);
  UpdateQueue_drop(&g_turbo_stats->update_queue);
  free(g_turbo_stats->entries);
  free(g_turbo_stats->used_bitmap);
  free(g_turbo_stats->dirty_bitmap);

  if (g_turbo_stats->queue_mutex_inited) {
    uv_mutex_destroy(&g_turbo_stats->queue_mutex);
  }
  if (g_turbo_stats->map_mutex_inited) {
    uv_mutex_destroy(&g_turbo_stats->map_mutex);
  }
  free(g_turbo_stats);
  g_turbo_stats = NULL;
} 

/* --------- Update enqueue helpers (ringbuffer based) --------- */

turbo_stat_id_t turbo_stats_register(const char *name, turbo_stat_type_t type) {
  if (!g_turbo_stats || !g_turbo_stats->initialized)
    return -1;

  uv_mutex_lock(&g_turbo_stats->map_mutex);
  uint32_t slot = UINT32_MAX;
  turbo_stat_entry_t *entry = find_or_create_entry(name, type, &slot);
  uv_mutex_unlock(&g_turbo_stats->map_mutex);

  if (!entry)
    return -1;

  return (turbo_stat_id_t)slot;
}

static int post_update_fast(turbo_stat_id_t id, turbo_stat_type_t type, int64_t value,
                            int operation) {
  if (!g_turbo_stats || !g_turbo_stats->initialized)
    return -1;

  /* Basic safety check on ID */
  if (id < 0 || (size_t)id >= g_turbo_stats->entry_capacity)
    return -1;

  stats_update_msg_t msg = {
      .slot = (uint32_t)id, .value = value, .type = (uint8_t)type, .op = (uint8_t)operation};

  uv_mutex_lock(&g_turbo_stats->queue_mutex);

  if (UpdateQueue_size(&g_turbo_stats->update_queue) >=
      (isize)g_turbo_stats->update_queue_max_size) {
    uv_mutex_unlock(&g_turbo_stats->queue_mutex);
    return -1; /* drop if full */
  }

  UpdateQueue_push_back(&g_turbo_stats->update_queue, msg);

  uv_mutex_unlock(&g_turbo_stats->queue_mutex);

  /* Signal async handler */
  uv_async_send(&g_turbo_stats->async_handle);
  return 0;
}

static int post_update(const char *name, turbo_stat_type_t type, int64_t value, int operation) {
  turbo_stat_id_t id = turbo_stats_register(name, type);
  if (id < 0)
    return -1;
  return post_update_fast(id, type, value, operation);
}

/* Fast API */

int turbo_stats_counter_add_fast(turbo_stat_id_t id, uint64_t value) {
  return post_update_fast(id, TURBO_STAT_COUNTER, (int64_t)value, 1);
}

int turbo_stats_counter_inc_fast(turbo_stat_id_t id) {
  return post_update_fast(id, TURBO_STAT_COUNTER, 1, 1);
}

int turbo_stats_gauge_set_fast(turbo_stat_id_t id, int64_t value) {
  return post_update_fast(id, TURBO_STAT_GAUGE, value, 0);
}

int turbo_stats_gauge_add_fast(turbo_stat_id_t id, int64_t delta) {
  return post_update_fast(id, TURBO_STAT_GAUGE, delta, 1);
}

int turbo_stats_histogram_record_fast(turbo_stat_id_t id, uint64_t value) {
  return post_update_fast(id, TURBO_STAT_HISTOGRAM, (int64_t)value, 0);
}

int turbo_stats_rate_record_fast(turbo_stat_id_t id, uint64_t value) {
  return post_update_fast(id, TURBO_STAT_RATE, (int64_t)value, 0);
}

/* Legacy String API (now wrappers) */

int turbo_stats_counter_add(const char *name, uint64_t value) {
  return post_update(name, TURBO_STAT_COUNTER, (int64_t)value, 1);
}

int turbo_stats_counter_inc(const char *name) {
  return post_update(name, TURBO_STAT_COUNTER, 1, 1);
}

int turbo_stats_gauge_set(const char *name, int64_t value) {
  return post_update(name, TURBO_STAT_GAUGE, value, 0);
}

int turbo_stats_gauge_add(const char *name, int64_t delta) {
  return post_update(name, TURBO_STAT_GAUGE, delta, 1);
}

int turbo_stats_histogram_record(const char *name, uint64_t value) {
  return post_update(name, TURBO_STAT_HISTOGRAM, (int64_t)value, 0);
}

int turbo_stats_rate_record(const char *name, uint64_t value) {
  return post_update(name, TURBO_STAT_RATE, (int64_t)value, 0);
}

static void on_async_update(uv_async_t *handle) {
  (void)handle;
  process_update_queue();
}

static void process_update_queue(void) {
  if (!g_turbo_stats)
    return;

  stats_update_msg_t batch[128];
  size_t batch_count;

  for (;;) {
    /* Pop a batch of updates */
    batch_count = 0;
    uv_mutex_lock(&g_turbo_stats->queue_mutex);
    while (batch_count < 128 && !UpdateQueue_is_empty(&g_turbo_stats->update_queue)) {
      batch[batch_count++] = *UpdateQueue_front(&g_turbo_stats->update_queue);
      UpdateQueue_pop_front(&g_turbo_stats->update_queue);
    }
    uv_mutex_unlock(&g_turbo_stats->queue_mutex);

    if (batch_count == 0) {
      break;
    }

    /* Process batch under map lock to protect entry data and metadata */
    uv_mutex_lock(&g_turbo_stats->map_mutex);
    
    for (size_t i = 0; i < batch_count; ++i) {
      stats_update_msg_t *msg = &batch[i];

      if (msg->slot >= g_turbo_stats->entry_capacity ||
          !bitmap_test(g_turbo_stats->used_bitmap, msg->slot)) {
        continue; /* stale slot */
      }

      turbo_stat_entry_t *entry = &g_turbo_stats->entries[msg->slot];

      switch (entry->type) {
      case TURBO_STAT_COUNTER:
        if (msg->op == 1) {
          entry->data.counter += (uint64_t)msg->value;
        } else if (msg->op == 0) {
          entry->data.counter = (uint64_t)msg->value;
        }
        break;

      case TURBO_STAT_GAUGE:
        if (msg->op == 0) {
          entry->data.gauge = msg->value;
        } else if (msg->op == 1) {
          entry->data.gauge += msg->value;
        } else if (msg->op == 2) {
          entry->data.gauge -= msg->value;
        }
        break;

      case TURBO_STAT_HISTOGRAM: {
        uint64_t v = (uint64_t)msg->value;
        entry->data.histogram.sum += v;
        entry->data.histogram.count += 1;
        if (entry->data.histogram.count == 1) {
          entry->data.histogram.min = v;
          entry->data.histogram.max = v;
        } else {
          if (v < entry->data.histogram.min)
            entry->data.histogram.min = v;
          if (v > entry->data.histogram.max)
            entry->data.histogram.max = v;
        }
        break;
      }

      case TURBO_STAT_RATE: {
        uint64_t now = get_timestamp_ms();
        uint64_t delta_time = now - entry->data.rate.last_time;
        uint64_t delta_value = (uint64_t)msg->value - entry->data.rate.last_value;
        entry->data.rate.value = (uint64_t)msg->value;
        entry->data.rate.last_time = now;
        entry->data.rate.last_value = (uint64_t)msg->value;
        if (delta_time > 0) {
          entry->data.rate.rate = (double)delta_value / ((double)delta_time / 1000.0);
        }
        break;
      }
      }

      bitmap_set(g_turbo_stats->dirty_bitmap, msg->slot);
    }
    
    uv_mutex_unlock(&g_turbo_stats->map_mutex);
  }
}

/* --------- Entry management --------- */

static uint32_t alloc_slot(void) {
  for (uint32_t i = 0; i < g_turbo_stats->entry_capacity; ++i) {
    if (!bitmap_test(g_turbo_stats->used_bitmap, i)) {
      bitmap_set(g_turbo_stats->used_bitmap, i);
      return i;
    }
  }
  return UINT32_MAX;
}

static turbo_stat_entry_t *find_or_create_entry(const char *name, turbo_stat_type_t type,
                                                uint32_t *slot_out) {
  if (!name || !g_turbo_stats)
    return NULL;

  const NameIndex_value *val = NameIndex_get(&g_turbo_stats->name_index, name);
  uint32_t slot = UINT32_MAX;
  if (val) {
    slot = val->second;
  } else {
    slot = alloc_slot();
    if (slot == UINT32_MAX)
      return NULL;

    /* copy name into entry and hash table key */
    turbo_stat_entry_t *entry = &g_turbo_stats->entries[slot];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->type = type;
    entry->next = g_turbo_stats->entries_head;
    g_turbo_stats->entries_head = entry;
    g_turbo_stats->entry_count++;

    NameIndex_insert(&g_turbo_stats->name_index, cstr_from(name), slot);
  }

  turbo_stat_entry_t *entry = &g_turbo_stats->entries[slot];
  entry->type = type; /* keep latest type */
  if (slot_out)
    *slot_out = slot;
  return entry;
}

/* --------- Public getters / utils --------- */

turbo_stat_entry_t *turbo_stats_get(const char *name) {
  if (!g_turbo_stats || !name)
    return NULL;

  uv_mutex_lock(&g_turbo_stats->map_mutex);
  const NameIndex_value *val = NameIndex_get(&g_turbo_stats->name_index, name);
  if (!val) {
    uv_mutex_unlock(&g_turbo_stats->map_mutex);
    return NULL;
  }
  uint32_t slot = val->second;
  if (slot >= g_turbo_stats->entry_capacity || !bitmap_test(g_turbo_stats->used_bitmap, slot)) {
    uv_mutex_unlock(&g_turbo_stats->map_mutex);
    return NULL;
  }
  turbo_stat_entry_t *entry = &g_turbo_stats->entries[slot];
  uv_mutex_unlock(&g_turbo_stats->map_mutex);
  return entry;
}

turbo_stat_entry_t *turbo_stats_get_all(void) {
  return g_turbo_stats ? g_turbo_stats->entries_head : NULL;
}

void turbo_stats_foreach(turbo_stats_iter_cb callback, void *user_data) {
  if (!g_turbo_stats || !callback)
    return;

  uv_mutex_lock(&g_turbo_stats->map_mutex);
  turbo_stat_entry_t *cur = g_turbo_stats->entries_head;
  while (cur) {
    callback(cur, user_data);
    cur = cur->next;
  }
  uv_mutex_unlock(&g_turbo_stats->map_mutex);
}

void turbo_stats_reset(const char *name) {
  turbo_stat_entry_t *entry = turbo_stats_get(name);
  if (!entry)
    return;

  switch (entry->type) {
  case TURBO_STAT_COUNTER:
    entry->data.counter = 0;
    break;
  case TURBO_STAT_GAUGE:
    entry->data.gauge = 0;
    break;
  case TURBO_STAT_HISTOGRAM:
    memset(&entry->data.histogram, 0, sizeof(entry->data.histogram));
    break;
  case TURBO_STAT_RATE:
    entry->data.rate.value = 0;
    entry->data.rate.last_value = 0;
    entry->data.rate.rate = 0.0;
    entry->data.rate.last_time = get_timestamp_ms();
    break;
  }
}

void turbo_stats_reset_all(void) {
  if (!g_turbo_stats)
    return;

  turbo_stat_entry_t *entry = g_turbo_stats->entries_head;
  while (entry) {
    turbo_stats_reset(entry->name);
    entry = entry->next;
  }
}

void turbo_stats_print(void) {
  if (!g_turbo_stats) {
    log_info(NULL, "Stats", "Statistics not initialized");
    return;
  }

  uv_mutex_lock(&g_turbo_stats->map_mutex);
  turbo_stat_entry_t *entry = g_turbo_stats->entries_head;
  while (entry) {
    switch (entry->type) {
    case TURBO_STAT_COUNTER:
      log_info(NULL, "Stats", "{:30} counter: {}", entry->name,
               (unsigned long long)entry->data.counter);
      break;
    case TURBO_STAT_GAUGE:
      log_info(NULL, "Stats", "{:30} gauge: {}", entry->name, (long long)entry->data.gauge);
      break;
    case TURBO_STAT_HISTOGRAM:
      if (entry->data.histogram.count > 0) {
        double avg = (double)entry->data.histogram.sum / (double)entry->data.histogram.count;
        log_info(NULL, "Stats", "{:30} histogram: count={}, avg={:.2f}, min={}, max={}",
                 entry->name, (unsigned long long)entry->data.histogram.count, avg,
                 (unsigned long long)entry->data.histogram.min,
                 (unsigned long long)entry->data.histogram.max);
      } else {
        log_info(NULL, "Stats", "{:30} histogram: no data", entry->name);
      }
      break;
    case TURBO_STAT_RATE:
      log_info(NULL, "Stats", "{:30} rate: {:.2f}/sec (value={})", entry->name,
               entry->data.rate.rate, (unsigned long long)entry->data.rate.value);
      break;
    }

    entry = entry->next;
  }
  uv_mutex_unlock(&g_turbo_stats->map_mutex);

  log_info(NULL, "Stats", "Total entries: {}", g_turbo_stats->entry_count);
}

char *turbo_stats_to_json(void) {
  if (!g_turbo_stats)
    return NULL;

  /* Simple JSON generation - in production, use a proper JSON library */
  size_t buffer_size = 4096;
  char *json = (char *)malloc(buffer_size);
  if (!json)
    return NULL;

  size_t pos = 0;
  pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos), "{\n");

  uv_mutex_lock(&g_turbo_stats->map_mutex);
  turbo_stat_entry_t *entry = g_turbo_stats->entries_head;
  int first = 1;

  while (entry && pos < buffer_size - 256) {
    if (!first) {
      pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos), ",\n");
    }
    first = 0;

    pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos), "  \"%s\": {", entry->name);

    switch (entry->type) {
    case TURBO_STAT_COUNTER:
      pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos),
                            "\"type\":\"counter\",\"value\":%llu",
                            (unsigned long long)entry->data.counter);
      break;
    case TURBO_STAT_GAUGE:
      pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos),
                            "\"type\":\"gauge\",\"value\":%lld", (long long)entry->data.gauge);
      break;
    case TURBO_STAT_HISTOGRAM:
      pos += stbsp_snprintf(
          json + pos, (int)(buffer_size - pos),
          "\"type\":\"histogram\",\"count\":%llu,\"sum\":%llu,\"min\":%llu,\"max\":%llu",
          (unsigned long long)entry->data.histogram.count,
          (unsigned long long)entry->data.histogram.sum,
          (unsigned long long)entry->data.histogram.min,
          (unsigned long long)entry->data.histogram.max);
      break;
    case TURBO_STAT_RATE:
      pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos),
                            "\"type\":\"rate\",\"rate\":%.2f,\"value\":%llu", entry->data.rate.rate,
                            (unsigned long long)entry->data.rate.value);
      break;
    }

    pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos), "}");
    entry = entry->next;
  }
  uv_mutex_unlock(&g_turbo_stats->map_mutex);

  pos += stbsp_snprintf(json + pos, (int)(buffer_size - pos), "\n}\n");

  return json;
}

/* --------- Timers --------- */

static void on_rate_timer(uv_timer_t *handle) {
  (void)handle;
  /* Rate is computed on update; nothing required here beyond keeping timer alive */
}

/* --------- Utility --------- */

static uint64_t get_timestamp_ms(void) {
  struct timespec ts;
#ifdef _WIN32
  timespec_get(&ts, TIME_UTC);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}
