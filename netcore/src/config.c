#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include "tlog.h"

#include <stc/cstr.h>
#define i_static
#define i_type ConfigMap
#define i_key_str
#define i_val config_entry_t
#include <stc/hmap.h>

/* Global configuration hashmap */
static ConfigMap g_config_map = {0};
static int g_config_init = 0;

/* Parse size env like 64K/1M/2G or plain integer; returns def on failure */
static size_t parse_env_size(const char *name, size_t def) {
  const char *s = getenv(name);
  if (!s || !*s)
    return def;

  size_t n = 0;
  while (*s && isspace((unsigned char)*s))
    s++;
  while (*s && isdigit((unsigned char)*s)) {
    n = n * 10 + (size_t)(*s - '0');
    s++;
  }
  while (*s && isspace((unsigned char)*s))
    s++;

  if (*s) {
    char c = (char)tolower((unsigned char)*s);
    if (c == 'k')
      n *= (size_t)1024;
    else if (c == 'm')
      n *= (size_t)1024 * 1024;
    else if (c == 'g')
      n *= (size_t)1024 * 1024 * 1024;
  }
  return n > 0 ? n : def;
}

int turbo_config_init(void) {
  if (g_config_init) {
    return 0; /* Already initialized */
  }

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  g_config_map = ConfigMap_init();
  g_config_init = 1;

  /* Initialize TCP defaults */
  turbo_tcp_config_init_defaults();

  /* Initialize UDP defaults */
  turbo_udp_config_init_defaults();

  /* Initialize KCP defaults */
  turbo_kcp_config_init_defaults();

  /* Initialize Pipe defaults */
  turbo_pipe_config_init_defaults();
  /* Initialize MTCP defaults */
  turbo_mtcp_config_init_defaults();

  return 0;
}

void turbo_config_cleanup(void) {
  if (g_config_init) {
    ConfigMap_drop(&g_config_map);
    g_config_init = 0;
  }
}

int turbo_config_set_int(const char *key, int64_t value) {
  if (!g_config_init || !key)
    return -1;

  config_entry_t entry = {0};
  strncpy(entry.key, key, sizeof(entry.key) - 1);
  entry.type = CONFIG_TYPE_INT;
  entry.value.int_val = value;

  ConfigMap_insert_or_assign(&g_config_map, cstr_from(key), entry);
  return 0;
}

int turbo_config_set_uint(const char *key, uint64_t value) {
  if (!g_config_init || !key)
    return -1;

  config_entry_t entry = {0};
  strncpy(entry.key, key, sizeof(entry.key) - 1);
  entry.type = CONFIG_TYPE_UINT;
  entry.value.uint_val = value;

  ConfigMap_insert_or_assign(&g_config_map, cstr_from(key), entry);
  return 0;
}

int turbo_config_set_float(const char *key, double value) {
  if (!g_config_init || !key)
    return -1;

  config_entry_t entry = {0};
  strncpy(entry.key, key, sizeof(entry.key) - 1);
  entry.type = CONFIG_TYPE_FLOAT;
  entry.value.float_val = value;

  ConfigMap_insert_or_assign(&g_config_map, cstr_from(key), entry);
  return 0;
}

int turbo_config_set_string(const char *key, const char *value) {
  if (!g_config_init || !key || !value)
    return -1;

  config_entry_t entry = {0};
  strncpy(entry.key, key, sizeof(entry.key) - 1);
  entry.type = CONFIG_TYPE_STRING;
  strncpy(entry.value.str_val, value, sizeof(entry.value.str_val) - 1);

  ConfigMap_insert_or_assign(&g_config_map, cstr_from(key), entry);
  return 0;
}

int64_t turbo_config_get_int(const char *key, int64_t default_val) {
  if (!g_config_init || !key)
    return default_val;

  const ConfigMap_value *entry = ConfigMap_get(&g_config_map, key);
  if (!entry || entry->second.type != CONFIG_TYPE_INT) {
    return default_val;
  }

  return entry->second.value.int_val;
}

uint64_t turbo_config_get_uint(const char *key, uint64_t default_val) {
  if (!g_config_init || !key)
    return default_val;

  const ConfigMap_value *entry = ConfigMap_get(&g_config_map, key);
  if (!entry || entry->second.type != CONFIG_TYPE_UINT) {
    return default_val;
  }

  return entry->second.value.uint_val;
}

double turbo_config_get_float(const char *key, double default_val) {
  if (!g_config_init || !key)
    return default_val;

  const ConfigMap_value *entry = ConfigMap_get(&g_config_map, key);
  if (!entry || entry->second.type != CONFIG_TYPE_FLOAT) {
    return default_val;
  }

  return entry->second.value.float_val;
}

const char *turbo_config_get_string(const char *key, const char *default_val) {
  if (!g_config_init || !key)
    return default_val;

  const ConfigMap_value *entry = ConfigMap_get(&g_config_map, key);
  if (!entry || entry->second.type != CONFIG_TYPE_STRING) {
    return default_val;
  }

  return entry->second.value.str_val;
}

void turbo_tcp_config_init_defaults(void) {
  /* Initialize TCP configuration with defaults and environment overrides */

  /* Socket buffer sizes */
  turbo_config_set_int(TURBO_TCP_RECV_BUFFER_SIZE, 256 * 1024);
  turbo_config_set_int(TURBO_TCP_SEND_BUFFER_SIZE, 256 * 1024);

  /* Batching (disabled by default) */
  turbo_config_set_uint(TURBO_TCP_BATCH_BYTES, 0);

  /* Arena pool configuration */
  turbo_config_set_uint(TURBO_TCP_ARENA_FREE_MAX, 16);
  turbo_config_set_uint(TURBO_TCP_ARENA_REGION_HINT, 1024 * 1024);

  /* Connection backlog */
  turbo_config_set_uint(TURBO_TCP_BACKLOG, 128);

  /* Pool and buffer sizes with environment variable support */
  turbo_config_set_uint(TURBO_TCP_POOL_CHUNK_SIZE,
                       parse_env_size("turbo_TCP_POOL_CHUNK", 64 * 1024));

  turbo_config_set_uint(TURBO_TCP_READ_BUF_MIN,
                       parse_env_size("turbo_TCP_READ_BUF_MIN", 16 * 1024));

  turbo_config_set_uint(
      TURBO_TCP_READ_BUF_MAX,
      parse_env_size("turbo_TCP_READ_BUF_MAX", 4 * 1024 * 1024));

  turbo_config_set_uint(TURBO_TCP_READ_BUF_SIZE,
                       parse_env_size("turbo_TCP_READ_BUF", 64 * 1024));

  /* Client buffer and IOV configuration */
  turbo_config_set_uint(TURBO_TCP_CLIENT_BUFFER_SIZE, 8192);
  turbo_config_set_uint(TURBO_TCP_MAX_WRITE_IOV, 64);

  /* Validate and fix read buffer limits */
  size_t read_min = turbo_tcp_config_get_read_buf_min();
  size_t read_max = turbo_tcp_config_get_read_buf_max();
  size_t read_size = turbo_tcp_config_get_read_buf_size();

  if (read_min == 0) {
    turbo_config_set_uint(TURBO_TCP_READ_BUF_MIN, 16 * 1024);
    read_min = 16 * 1024;
  }
  if (read_max == 0) {
    turbo_config_set_uint(TURBO_TCP_READ_BUF_MAX, 4 * 1024 * 1024);
    read_max = 4 * 1024 * 1024;
  }
  if (read_max < read_min) {
    turbo_config_set_uint(TURBO_TCP_READ_BUF_MIN, read_max);
    turbo_config_set_uint(TURBO_TCP_READ_BUF_MAX, read_min);
  }
  if (read_size < read_min) {
    turbo_config_set_uint(TURBO_TCP_READ_BUF_SIZE, read_min);
  }
  if (read_size > read_max) {
    turbo_config_set_uint(TURBO_TCP_READ_BUF_SIZE, read_max);
  }
}

void turbo_config_print_all(void) {
  if (!g_config_init) {
    TLOG_ERROR("Configuration system not initialized");
    return;
  }

  TLOG_INFO("=== Iris Configuration ===");
  TLOG_INFO("TCP Configuration:");
  TLOG_INFO("  recv_buffer_size: {:d}", turbo_tcp_config_get_recv_buffer_size());
  TLOG_INFO("  send_buffer_size: {:d}", turbo_tcp_config_get_send_buffer_size());
  TLOG_INFO("  batch_bytes: {}", turbo_tcp_config_get_batch_bytes());
  TLOG_INFO("  arena_free_max: {}", turbo_tcp_config_get_arena_free_max());
  TLOG_INFO("  arena_region_hint: {}", turbo_tcp_config_get_arena_region_hint());
  TLOG_INFO("  backlog: {:u}", turbo_tcp_config_get_backlog());
  TLOG_INFO("  pool_chunk_size: {}", turbo_tcp_config_get_pool_chunk_size());
  TLOG_INFO("  read_buf_min: {}", turbo_tcp_config_get_read_buf_min());
  TLOG_INFO("  read_buf_max: {}", turbo_tcp_config_get_read_buf_max());
  TLOG_INFO("  read_buf_size: {}", turbo_tcp_config_get_read_buf_size());

  TLOG_INFO("UDP Configuration:");
  TLOG_INFO("  pool_chunk_size: {}", turbo_udp_config_get_pool_chunk_size());
  TLOG_INFO("  recv_buf_size: {}", turbo_udp_config_get_recv_buf_size());
  TLOG_INFO("  recv_buf_min: {}", turbo_udp_config_get_recv_buf_min());
  TLOG_INFO("  recv_buf_max: {}", turbo_udp_config_get_recv_buf_max());

  TLOG_INFO("Statistics Configuration:");
  TLOG_INFO("  update_pool_size: {}", turbo_stats_config_get_update_pool_size());
  TLOG_INFO("  rate_interval_ms: {:d}", turbo_stats_config_get_rate_interval_ms());
  TLOG_INFO("==========================");
}

int turbo_config_load_from_file(const char *filename) {
  if (!filename)
    return -1;

  FILE *file = fopen(filename, "r");
  if (!file)
    return -1;

  char line[512];
  int line_num = 0;

  while (fgets(line, sizeof(line), file)) {
    line_num++;

    /* Skip comments and empty lines */
    char *p = line;
    while (*p && isspace(*p))
      p++;
    if (*p == '#' || *p == '\0')
      continue;

    /* Parse key=value */
    char *eq = strchr(p, '=');
    if (!eq)
      continue;

    *eq = '\0';
    char *key = p;
    char *value = eq + 1;

    /* Trim whitespace */
    while (*key && isspace(*key))
      key++;
    char *key_end = key + strlen(key) - 1;
    while (key_end > key && isspace(*key_end))
      *key_end-- = '\0';

    while (*value && isspace(*value))
      value++;
    char *value_end = value + strlen(value) - 1;
    while (value_end > value && isspace(*value_end))
      *value_end-- = '\0';

    /* Try to parse as number first */
    char *endptr;
    long long int_val = strtoll(value, &endptr, 10);
    if (*endptr == '\0') {
      turbo_config_set_int(key, int_val);
    } else {
      /* Parse as string */
      turbo_config_set_string(key, value);
    }
  }

  fclose(file);
  return 0;
}

int turbo_config_save_to_file(const char *filename) {
  if (!filename)
    return -1;

  FILE *file = fopen(filename, "w");
  if (!file)
    return -1;

  fprintf(file, "# Iris Configuration File\n");
  fprintf(file, "# Generated automatically\n\n");

  fprintf(file, "# TCP Configuration\n");
  fprintf(file, "%s=%d\n", TURBO_TCP_RECV_BUFFER_SIZE,
          turbo_tcp_config_get_recv_buffer_size());
  fprintf(file, "%s=%d\n", TURBO_TCP_SEND_BUFFER_SIZE,
          turbo_tcp_config_get_send_buffer_size());
  fprintf(file, "%s=%zu\n", TURBO_TCP_BATCH_BYTES,
          turbo_tcp_config_get_batch_bytes());
  fprintf(file, "%s=%zu\n", TURBO_TCP_ARENA_FREE_MAX,
          turbo_tcp_config_get_arena_free_max());
  fprintf(file, "%s=%zu\n", TURBO_TCP_ARENA_REGION_HINT,
          turbo_tcp_config_get_arena_region_hint());
  fprintf(file, "%s=%u\n", TURBO_TCP_BACKLOG, turbo_tcp_config_get_backlog());
  fprintf(file, "%s=%zu\n", TURBO_TCP_POOL_CHUNK_SIZE,
          turbo_tcp_config_get_pool_chunk_size());
  fprintf(file, "%s=%zu\n", TURBO_TCP_READ_BUF_MIN,
          turbo_tcp_config_get_read_buf_min());
  fprintf(file, "%s=%zu\n", TURBO_TCP_READ_BUF_MAX,
          turbo_tcp_config_get_read_buf_max());
  fprintf(file, "%s=%zu\n", TURBO_TCP_READ_BUF_SIZE,
          turbo_tcp_config_get_read_buf_size());

  fprintf(file, "\n# UDP Configuration\n");
  fprintf(file, "%s=%zu\n", TURBO_UDP_POOL_CHUNK_SIZE,
          turbo_udp_config_get_pool_chunk_size());
  fprintf(file, "%s=%zu\n", TURBO_UDP_RECV_BUF_SIZE,
          turbo_udp_config_get_recv_buf_size());
  fprintf(file, "%s=%zu\n", TURBO_UDP_RECV_BUF_MIN,
          turbo_udp_config_get_recv_buf_min());
  fprintf(file, "%s=%zu\n", TURBO_UDP_RECV_BUF_MAX,
          turbo_udp_config_get_recv_buf_max());

  fprintf(file, "\n# KCP Configuration\n");
  fprintf(file, "%s=%u\n", TURBO_KCP_CONV_BASE,
          turbo_kcp_config_get_conv_base());
  fprintf(file, "%s=%d\n", TURBO_KCP_NODELAY,
          turbo_kcp_config_get_nodelay());
  fprintf(file, "%s=%d\n", TURBO_KCP_INTERVAL,
          turbo_kcp_config_get_interval());
  fprintf(file, "%s=%d\n", TURBO_KCP_RESEND,
          turbo_kcp_config_get_resend());
  fprintf(file, "%s=%d\n", TURBO_KCP_NC,
          turbo_kcp_config_get_nc());
  fprintf(file, "%s=%d\n", TURBO_KCP_MTU,
          turbo_kcp_config_get_mtu());
  fprintf(file, "%s=%d\n", TURBO_KCP_MSS,
          turbo_kcp_config_get_mss());
  fprintf(file, "%s=%d\n", TURBO_KCP_SND_WND,
          turbo_kcp_config_get_snd_wnd());
  fprintf(file, "%s=%d\n", TURBO_KCP_RCV_WND,
          turbo_kcp_config_get_rcv_wnd());

  fprintf(file, "\n# MTCP Configuration\n");
  fprintf(file, "%s=%s\n", TURBO_MTCP_HOST, turbo_mtcp_config_get_host());
  fprintf(file, "%s=%u\n", TURBO_MTCP_PORT, turbo_mtcp_config_get_port());
  fprintf(file, "%s=%s\n", TURBO_MTCP_WORKER_PATH,
          turbo_mtcp_config_get_worker_path());
  fprintf(file, "%s=%u\n", TURBO_MTCP_WORKER_COUNT,
          turbo_mtcp_config_get_worker_count());
  fprintf(file, "%s=%c\n", TURBO_MTCP_HANDSHAKE_TOKEN,
          turbo_mtcp_config_get_handshake_token());

  fprintf(file, "\n# Statistics Configuration\n");
  fprintf(file, "%s=%zu\n", TURBO_STATS_UPDATE_POOL_SIZE,
          turbo_stats_config_get_update_pool_size());
  fprintf(file, "%s=%d\n", TURBO_STATS_RATE_INTERVAL_MS,
          turbo_stats_config_get_rate_interval_ms());

  fclose(file);
  return 0;
}
/* Initialize Pipe configuration defaults */
void turbo_pipe_config_init_defaults(void) {
  /* Initialize Pipe configuration with defaults and environment overrides */

  /* Socket buffer sizes */
  turbo_config_set_int(TURBO_PIPE_RECV_BUFFER_SIZE, 256 * 1024);
  turbo_config_set_int(TURBO_PIPE_SEND_BUFFER_SIZE, 256 * 1024);

  /* Batching (disabled by default) */
  turbo_config_set_uint(TURBO_PIPE_BATCH_BYTES, 0);

  /* Arena pool configuration */
  turbo_config_set_uint(TURBO_PIPE_ARENA_FREE_MAX, 16);
  turbo_config_set_uint(TURBO_PIPE_ARENA_REGION_HINT, 1024 * 1024);

  /* Connection backlog */
  turbo_config_set_uint(TURBO_PIPE_BACKLOG, 128);

  /* Pool and buffer sizes with environment variable support */
  turbo_config_set_uint(TURBO_PIPE_POOL_CHUNK_SIZE,
                       parse_env_size("turbo_PIPE_POOL_CHUNK", 64 * 1024));

  turbo_config_set_uint(TURBO_PIPE_READ_BUF_MIN,
                       parse_env_size("turbo_PIPE_READ_BUF_MIN", 16 * 1024));

  turbo_config_set_uint(
      TURBO_PIPE_READ_BUF_MAX,
      parse_env_size("TURBO_PIPE_READ_BUF_MAX", 4 * 1024 * 1024));

  turbo_config_set_uint(TURBO_PIPE_READ_BUF_SIZE,
                       parse_env_size("TURBO_PIPE_READ_BUF", 64 * 1024));

  /* Client buffer and IOV configuration */
  turbo_config_set_uint(TURBO_PIPE_CLIENT_BUFFER_SIZE, 8192);
  turbo_config_set_uint(TURBO_PIPE_MAX_WRITE_IOV, 64);

  /* Validate and fix read buffer limits */
  size_t read_min = turbo_pipe_config_get_read_buf_min();
  size_t read_max = turbo_pipe_config_get_read_buf_max();
  size_t read_size = turbo_pipe_config_get_read_buf_size();

  if (read_min > read_max) {
    turbo_config_set_uint(TURBO_PIPE_READ_BUF_MIN, read_max);
  }

  if (read_size < read_min) {
    turbo_config_set_uint(TURBO_PIPE_READ_BUF_SIZE, read_min);
  } else if (read_size > read_max) {
    turbo_config_set_uint(TURBO_PIPE_READ_BUF_SIZE, read_max);
  }
}

/* Initialize UDP configuration defaults */
void turbo_udp_config_init_defaults(void) {
  /* Initialize UDP configuration with defaults and environment overrides */

  /* Pool configuration */
  turbo_config_set_uint(TURBO_UDP_POOL_CHUNK_SIZE,
                       parse_env_size("turbo_UDP_POOL_CHUNK", 32 * 1024));

  /* Receive buffer configuration */
  turbo_config_set_uint(TURBO_UDP_RECV_BUF_SIZE,
                       parse_env_size("turbo_UDP_RECV_BUF", 64 * 1024));

  turbo_config_set_uint(TURBO_UDP_RECV_BUF_MIN,
                       parse_env_size("turbo_UDP_RECV_BUF_MIN", 8 * 1024));

  turbo_config_set_uint(
      TURBO_UDP_RECV_BUF_MAX,
      parse_env_size("TURBO_UDP_RECV_BUF_MAX", 2 * 1024 * 1024));

  /* Validate and fix buffer limits */
  size_t recv_min = turbo_udp_config_get_recv_buf_min();
  size_t recv_max = turbo_udp_config_get_recv_buf_max();
  size_t recv_size = turbo_udp_config_get_recv_buf_size();

  if (recv_min == 0) {
    turbo_config_set_uint(TURBO_UDP_RECV_BUF_MIN, 8 * 1024);
    recv_min = 8 * 1024;
  }
  if (recv_max == 0) {
    turbo_config_set_uint(TURBO_UDP_RECV_BUF_MAX, 2 * 1024 * 1024);
    recv_max = 2 * 1024 * 1024;
  }
  if (recv_max < recv_min) {
    turbo_config_set_uint(TURBO_UDP_RECV_BUF_MIN, recv_max);
    turbo_config_set_uint(TURBO_UDP_RECV_BUF_MAX, recv_min);
  }
  if (recv_size < recv_min) {
    turbo_config_set_uint(TURBO_UDP_RECV_BUF_SIZE, recv_min);
  }
  if (recv_size > recv_max) {
    turbo_config_set_uint(TURBO_UDP_RECV_BUF_SIZE, recv_max);
  }
}

void turbo_kcp_config_init_defaults(void) {
  turbo_kcp_config_set_conv_base(0x10000000u);
  turbo_kcp_config_set_nodelay(1);
  turbo_kcp_config_set_interval(10);
  turbo_kcp_config_set_resend(2);
  turbo_kcp_config_set_nc(1);
  turbo_kcp_config_set_mtu(TURBO_KCP_DEFAULT_MTU);
  turbo_kcp_config_set_mss(TURBO_KCP_DEFAULT_MSS);
  turbo_kcp_config_set_snd_wnd(TURBO_KCP_DEFAULT_SND_WND);
  turbo_kcp_config_set_rcv_wnd(TURBO_KCP_DEFAULT_RCV_WND);
}

void turbo_mtcp_config_init_defaults(void) {
  turbo_config_set_string(TURBO_MTCP_HOST, "0.0.0.0");
  turbo_config_set_uint(TURBO_MTCP_PORT, 7000);
  turbo_config_set_string(TURBO_MTCP_WORKER_PATH, "");
  turbo_config_set_uint(TURBO_MTCP_WORKER_COUNT, 0);
  turbo_mtcp_config_set_handshake_token(' ');
}
