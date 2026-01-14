#ifndef TURBO_STATS_H
#define TURBO_STATS_H

#include <platform.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations - implementation details hidden */
typedef struct turbo_stats_s turbo_stats_t;
typedef struct turbo_arena_stats_s turbo_arena_stats_t;
/* Statistics data types */
typedef enum {
  TURBO_STAT_COUNTER,   /* Monotonic counter (bytes, packets, etc.) */
  TURBO_STAT_GAUGE,     /* Current value (connections, memory usage) */
  TURBO_STAT_HISTOGRAM, /* Value distribution (latency, sizes) */
  TURBO_STAT_RATE       /* Rate calculation (per second) */
} turbo_stat_type_t;

/* Statistics entry */
typedef struct turbo_stat_entry_s {
  char name[64];
  turbo_stat_type_t type;
  union {
    uint64_t counter;
    int64_t gauge;
    struct {
      uint64_t sum;
      uint64_t count;
      uint64_t min;
      uint64_t max;
    } histogram;
    struct {
      uint64_t value;
      uint64_t last_value;
      double rate;
      uint64_t last_time;
    } rate;
  } data;
  struct turbo_stat_entry_s *next; /* kept for API compatibility when iterating */
} turbo_stat_entry_t;

/* turbo_stats_t is opaque - implementation in stats.c */
/* Arena statistics */
struct turbo_arena_stats_s {
  size_t region_count;        /* Number of regions */
  size_t total_allocated;     /* Total allocated memory */
  size_t total_used;          /* Total used memory */
  size_t regions_with_refs;   /* Regions with active references */
  size_t empty_regions;       /* Empty regions */
  double fragmentation_ratio; /* Fragmentation (0.0 = no fragmentation) */
  double efficiency;          /* Memory efficiency (0.0-1.0) */
};

/* Generic transport statistics shared by all protocols */
typedef struct turbo_transport_stats_s {
  /* Traffic statistics */
  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t messages_sent;
  uint64_t messages_received;
  uint64_t packets_sent;
  uint64_t packets_received;

  /* Zero-copy statistics */
  uint64_t zero_copy_sends;
  uint64_t zero_copy_receives;
  uint64_t copy_sends;

  /* Connection statistics */
  uint64_t connections_established;
  uint64_t connections_closed;
  uint64_t connections_failed;
  uint64_t active_connections;
  uint64_t clients_active;
  uint64_t handshakes_completed;
  uint64_t handshake_failures;

  /* Protocol-specific counters */
  uint64_t kcp_updates;
  uint64_t retransmissions;

  /* Error statistics */
  uint64_t send_errors;
  uint64_t recv_errors;
  uint64_t connection_errors;
  uint64_t tls_errors;
  uint64_t kcp_errors;

  /* Memory statistics */
  turbo_arena_stats_t arena_stats;
} turbo_transport_stats_t;

/* Protocol-specific aliases */
typedef turbo_transport_stats_t turbo_udp_stats_t;
typedef turbo_transport_stats_t turbo_tls_stats_t;
typedef turbo_transport_stats_t turbo_tcp_stats_t;
typedef turbo_transport_stats_t turbo_kcp_stats_t;
typedef turbo_transport_stats_t turbo_pipe_stats_t;
/* Global statistics instance */
extern turbo_stats_t *g_turbo_stats;

/* Initialize statistics system */
/**
 * @brief Initializes the global statistics system.
 *
 * @param loop Event loop to use for async operations (opaque pointer).
 * @param update_pool_size The initial size of the update pool for thread-safe statistics.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_stats_init(void *loop, size_t update_pool_size);

/* Cleanup statistics system */
/**
 * @brief Cleans up and deallocates resources used by the statistics system.
 */
CXX_C_API void turbo_stats_cleanup(void);

/* Statistics ID for fast updates */
typedef int32_t turbo_stat_id_t;

/* Get or register a statistic ID (thread-safe, invokes lock) */
/**
 * @brief Registers a statistic and returns its ID.
 *        If the statistic already exists, returns the existing ID.
 *        This function is thread-safe but involves a lock.
 *
 * @param name The name of the statistic.
 * @param type The type of the statistic.
 * @return The statistic ID (>= 0), or -1 on error.
 */
CXX_C_API turbo_stat_id_t turbo_stats_register(const char *name, turbo_stat_type_t type);

/* Fast update API using ID (lock-free lookup, only locks queue) */
/**
 * @brief Fast path for counter addition.
 */
CXX_C_API int turbo_stats_counter_add_fast(turbo_stat_id_t id, uint64_t value);
CXX_C_API int turbo_stats_counter_inc_fast(turbo_stat_id_t id);

/**
 * @brief Fast path for gauge updates.
 */
CXX_C_API int turbo_stats_gauge_set_fast(turbo_stat_id_t id, int64_t value);
CXX_C_API int turbo_stats_gauge_add_fast(turbo_stat_id_t id, int64_t delta);

/**
 * @brief Fast path for histogram/rate.
 */
CXX_C_API int turbo_stats_histogram_record_fast(turbo_stat_id_t id, uint64_t value);
CXX_C_API int turbo_stats_rate_record_fast(turbo_stat_id_t id, uint64_t value);

/* Thread-safe statistics updates (String-based, slower due to lookup lock) */
/**
 * @brief Atomically adds a value to a counter statistic.
 *
 * @param name The name of the counter statistic.
 * @param value The value to add.
 * @return 0 on success, or a non-zero error code if the statistic is not found or type mismatch.
 */
CXX_C_API int turbo_stats_counter_add(const char *name, uint64_t value);
/**
 * @brief Atomically increments a counter statistic by 1.
 *
 * @param name The name of the counter statistic.
 * @return 0 on success, or a non-zero error code if the statistic is not found or type mismatch.
 */
CXX_C_API int turbo_stats_counter_inc(const char *name);
/**
 * @brief Sets the value of a gauge statistic.
 *
 * @param name The name of the gauge statistic.
 * @param value The value to set.
 * @return 0 on success, or a non-zero error code if the statistic is not found or type mismatch.
 */
CXX_C_API int turbo_stats_gauge_set(const char *name, int64_t value);
/**
 * @brief Adds a delta to a gauge statistic.
 *
 * @param name The name of the gauge statistic.
 * @param delta The value to add (can be negative).
 * @return 0 on success, or a non-zero error code if the statistic is not found or type mismatch.
 */
CXX_C_API int turbo_stats_gauge_add(const char *name, int64_t delta);
/**
 * @brief Records a value for a histogram statistic.
 *
 * @param name The name of the histogram statistic.
 * @param value The value to record.
 * @return 0 on success, or a non-zero error code if the statistic is not found or type mismatch.
 */
CXX_C_API int turbo_stats_histogram_record(const char *name, uint64_t value);
/**
 * @brief Records a value for a rate statistic.
 *
 * @param name The name of the rate statistic.
 * @param value The current value to calculate the rate from.
 * @return 0 on success, or a non-zero error code if the statistic is not found or type mismatch.
 */
CXX_C_API int turbo_stats_rate_record(const char *name, uint64_t value);

/* Get statistics (main thread only) */
/**
 * @brief Retrieves a specific statistic entry by name (main thread only).
 *
 * @param name The name of the statistic to retrieve.
 * @return A pointer to the `turbo_stat_entry_t` if found, or NULL otherwise.
 */
CXX_C_API turbo_stat_entry_t *turbo_stats_get(const char *name);
/**
 * @brief Retrieves the head of the linked list of all statistic entries (main thread only).
 *
 * @return A pointer to the first `turbo_stat_entry_t` in the list, or NULL if no statistics.
 */
CXX_C_API turbo_stat_entry_t *turbo_stats_get_all(void);

/* Statistics iteration */
typedef void (*turbo_stats_iter_cb)(const turbo_stat_entry_t *entry, void *user_data);
/**
 * @brief Iterates over all registered statistics, calling a callback for each entry.
 *
 * @param callback The callback function to invoke for each statistic entry.
 * @param user_data User-defined data to pass to the callback.
 */
CXX_C_API void turbo_stats_foreach(turbo_stats_iter_cb callback, void *user_data);

/* Reset statistics */
/**
 * @brief Resets a specific statistic entry to its initial state.
 *
 * @param name The name of the statistic to reset.
 */
CXX_C_API void turbo_stats_reset(const char *name);
/**
 * @brief Resets all statistic entries to their initial state.
 */
CXX_C_API void turbo_stats_reset_all(void);

/* Export statistics as JSON string (caller must free) */
/**
 * @brief Exports all statistics as a JSON string.
 *        The caller is responsible for freeing the returned string.
 *
 * @return A dynamically allocated JSON string containing all statistics, or NULL on error.
 */
CXX_C_API char *turbo_stats_to_json(void);

/* Print statistics to stdout */
/**
 * @brief Prints all statistics to standard output in a human-readable format.
 */
CXX_C_API void turbo_stats_print(void);

/* Convenience macros */
#define TURBO_STATS_INC(name) turbo_stats_counter_inc(name)
#define TURBO_STATS_ADD(name, val) turbo_stats_counter_add(name, val)
#define TURBO_STATS_SET(name, val) turbo_stats_gauge_set(name, val)
#define TURBO_STATS_RECORD(name, val) turbo_stats_histogram_record(name, val)
#define TURBO_STATS_RATE(name, val) turbo_stats_rate_record(name, val)

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STATS_H */
