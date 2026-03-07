#ifndef turbo_CONFIG_H
#define turbo_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration entry types */
typedef struct config_entry_s {
  char key[64];
  union {
    int64_t int_val;
    uint64_t uint_val;
    double float_val;
    char str_val[256];
  } value;
  enum {
    CONFIG_TYPE_INT,
    CONFIG_TYPE_UINT,
    CONFIG_TYPE_FLOAT,
    CONFIG_TYPE_STRING
  } type;
} config_entry_t;

/* Global configuration management */
/**
 * @brief Initializes the global configuration system.
 *        This function must be called before using any other configuration functions.
 *
 * @return 0 on success, or a non-zero error code on failure.
 */
int turbo_config_init(void);
/**
 * @brief Cleans up and deallocates resources used by the global configuration system.
 *        This function should be called when the configuration system is no longer needed.
 */
void turbo_config_cleanup(void);

/* Configuration setters */
/**
 * @brief Sets an integer value for a given configuration key.
 *
 * @param key The configuration key.
 * @param value The integer value to set.
 * @return 0 on success, or a non-zero error code if the key is invalid or type mismatch.
 */
int turbo_config_set_int(const char *key, int64_t value);
/**
 * @brief Sets an unsigned integer value for a given configuration key.
 *
 * @param key The configuration key.
 * @param value The unsigned integer value to set.
 * @return 0 on success, or a non-zero error code if the key is invalid or type mismatch.
 */
int turbo_config_set_uint(const char *key, uint64_t value);
/**
 * @brief Sets a floating-point value for a given configuration key.
 *
 * @param key The configuration key.
 * @param value The floating-point value to set.
 * @return 0 on success, or a non-zero error code if the key is invalid or type mismatch.
 */
int turbo_config_set_float(const char *key, double value);
/**
 * @brief Sets a string value for a given configuration key.
 *
 * @param key The configuration key.
 * @param value The string value to set.
 * @return 0 on success, or a non-zero error code if the key is invalid or type mismatch.
 */
int turbo_config_set_string(const char *key, const char *value);

/* Configuration getters with defaults */
/**
 * @brief Gets an integer value for a given configuration key, with a default fallback.
 *
 * @param key The configuration key.
 * @param default_val The default value to return if the key is not found or is not an integer.
 * @return The integer value associated with the key, or `default_val` if not found or type mismatch.
 */
int64_t turbo_config_get_int(const char *key, int64_t default_val);
/**
 * @brief Gets an unsigned integer value for a given configuration key, with a default fallback.
 *
 * @param key The configuration key.
 * @param default_val The default value to return if the key is not found or is not an unsigned integer.
 * @return The unsigned integer value associated with the key, or `default_val` if not found or type mismatch.
 */
uint64_t turbo_config_get_uint(const char *key, uint64_t default_val);
/**
 * @brief Gets a floating-point value for a given configuration key, with a default fallback.
 *
 * @param key The configuration key.
 * @param default_val The default value to return if the key is not found or is not a float.
 * @return The floating-point value associated with the key, or `default_val` if not found or type mismatch.
 */
double turbo_config_get_float(const char *key, double default_val);
/**
 * @brief Gets a string value for a given configuration key, with a default fallback.
 *
 * @param key The configuration key.
 * @param default_val The default value to return if the key is not found or is not a string.
 * @return The string value associated with the key, or `default_val` if not found or type mismatch.
 */
const char *turbo_config_get_string(const char *key, const char *default_val);

/* Configuration utility functions */
/**
 * @brief Prints all currently stored configuration entries to standard output.
 */
void turbo_config_print_all(void);
/**
 * @brief Loads configuration entries from a specified file.
 *
 * @param filename The path to the configuration file.
 * @return 0 on success, or a non-zero error code on failure (e.g., file not found, parse error).
 */
int turbo_config_load_from_file(const char *filename);
/**
 * @brief Saves all current configuration entries to a specified file.
 *
 * @param filename The path to the file where configuration will be saved.
 * @return 0 on success, or a non-zero error code on failure.
 */
int turbo_config_save_to_file(const char *filename);

/* TCP Configuration Keys (constants) */
#define TURBO_TCP_RECV_BUFFER_SIZE "tcp.recv_buffer_size"
#define TURBO_TCP_SEND_BUFFER_SIZE "tcp.send_buffer_size"
#define TURBO_TCP_BATCH_BYTES "tcp.batch_bytes"
#define TURBO_TCP_ARENA_FREE_MAX "tcp.arena_free_max"
#define TURBO_TCP_ARENA_REGION_HINT "tcp.arena_region_hint"
#define TURBO_TCP_BACKLOG "tcp.backlog"
#define TURBO_TCP_POOL_CHUNK_SIZE "tcp.pool_chunk_size"
#define TURBO_TCP_READ_BUF_MIN "tcp.read_buf_min"
#define TURBO_TCP_READ_BUF_MAX "tcp.read_buf_max"
#define TURBO_TCP_READ_BUF_SIZE "tcp.read_buf_size"
#define TURBO_TCP_CLIENT_BUFFER_SIZE "tcp.client_buffer_size"
#define TURBO_TCP_MAX_WRITE_IOV "tcp.max_write_iov"

/* UDP Configuration Keys */
#define TURBO_UDP_POOL_CHUNK_SIZE "udp.pool_chunk_size"
#define TURBO_UDP_RECV_BUF_SIZE "udp.recv_buf_size"
#define TURBO_UDP_RECV_BUF_MIN "udp.recv_buf_min"
#define TURBO_UDP_RECV_BUF_MAX "udp.recv_buf_max"
/* KCP Configuration Keys */
#define TURBO_KCP_CONV_BASE "kcp.conv_base"
#define TURBO_KCP_NODELAY "kcp.nodelay"
#define TURBO_KCP_INTERVAL "kcp.interval"
#define TURBO_KCP_RESEND "kcp.resend"
#define TURBO_KCP_NC "kcp.nc"
#define TURBO_KCP_MTU "kcp.mtu"
#define TURBO_KCP_MSS "kcp.mss"
#define TURBO_KCP_SND_WND "kcp.snd_wnd"
#define TURBO_KCP_RCV_WND "kcp.rcv_wnd"

#define TURBO_KCP_DEFAULT_MTU 1400
#define TURBO_KCP_DEFAULT_MSS 1376
#define TURBO_KCP_DEFAULT_SND_WND 32
#define TURBO_KCP_DEFAULT_RCV_WND 128

/* MTCP Configuration Keys */
#define TURBO_MTCP_HOST "mtcp.host"
#define TURBO_MTCP_PORT "mtcp.port"
#define TURBO_MTCP_WORKER_PATH "mtcp.worker_path"
#define TURBO_MTCP_WORKER_COUNT "mtcp.worker_count"
#define TURBO_MTCP_HANDSHAKE_TOKEN "mtcp.handshake_token"
#ifdef _WIN32
  #define TURBO_MTCP_WORKER_BASENAME "worker.exe"
#else
  #define TURBO_MTCP_WORKER_BASENAME "worker"
#endif

/* Pipe Configuration Keys */
#define TURBO_PIPE_RECV_BUFFER_SIZE "pipe.recv_buffer_size"
#define TURBO_PIPE_SEND_BUFFER_SIZE "pipe.send_buffer_size"
#define TURBO_PIPE_BATCH_BYTES "pipe.batch_bytes"
#define TURBO_PIPE_ARENA_FREE_MAX "pipe.arena_free_max"
#define TURBO_PIPE_ARENA_REGION_HINT "pipe.arena_region_hint"
#define TURBO_PIPE_BACKLOG "pipe.backlog"
#define TURBO_PIPE_POOL_CHUNK_SIZE "pipe.pool_chunk_size"
#define TURBO_PIPE_READ_BUF_MIN "pipe.read_buf_min"
#define TURBO_PIPE_READ_BUF_MAX "pipe.read_buf_max"
#define TURBO_PIPE_READ_BUF_SIZE "pipe.read_buf_size"
#define TURBO_PIPE_CLIENT_BUFFER_SIZE "pipe.client_buffer_size"
#define TURBO_PIPE_MAX_WRITE_IOV "pipe.max_write_iov"



/* TCP Configuration Helpers */
/**
 * @brief Initializes default configuration values for TCP settings.
 */
void turbo_tcp_config_init_defaults(void);

/* UDP Configuration Helpers */
/**
 * @brief Initializes default configuration values for UDP settings.
 */
void turbo_udp_config_init_defaults(void);

/* KCP Configuration Helpers */
/**
 * @brief Initializes default configuration values for KCP settings.
 */
void turbo_kcp_config_init_defaults(void);

/* Pipe Configuration Helpers */
/**
 * @brief Initializes default configuration values for Pipe settings.
 */
void turbo_pipe_config_init_defaults(void);
/**
 * @brief Initializes default configuration values for MTCP settings.
 */
void turbo_mtcp_config_init_defaults(void);

/* TCP Configuration Setters */
/**
 * @brief Sets the TCP receive buffer size.
 * @param size The desired receive buffer size in bytes.
 */
static inline void turbo_tcp_config_set_recv_buffer_size(int size) {
  turbo_config_set_int(TURBO_TCP_RECV_BUFFER_SIZE, size);
}
/**
 * @brief Sets the TCP send buffer size.
 * @param size The desired send buffer size in bytes.
 */
static inline void turbo_tcp_config_set_send_buffer_size(int size) {
  turbo_config_set_int(TURBO_TCP_SEND_BUFFER_SIZE, size);
}
/**
 * @brief Sets the number of bytes to batch for TCP operations.
 * @param bytes The number of bytes to batch.
 */
static inline void turbo_tcp_config_set_batch_bytes(size_t bytes) {
  turbo_config_set_uint(TURBO_TCP_BATCH_BYTES, bytes);
}
/**
 * @brief Configures the TCP arena memory pool parameters.
 * @param max_free The maximum number of free arena slices to keep.
 * @param region_hint The hint for the size of memory regions to allocate.
 */
static inline void turbo_tcp_config_set_arena_pool(size_t max_free,
                                                  size_t region_hint) {
  turbo_config_set_uint(TURBO_TCP_ARENA_FREE_MAX, max_free);
  turbo_config_set_uint(TURBO_TCP_ARENA_REGION_HINT, region_hint);
}
/**
 * @brief Sets the TCP connection backlog size for listening sockets.
 * @param backlog The maximum length of the queue of pending connections.
 */
static inline void turbo_tcp_config_set_backlog(unsigned int backlog) {
  turbo_config_set_uint(TURBO_TCP_BACKLOG, backlog);
}
/**
 * @brief Sets the chunk size for the TCP memory pool.
 * @param bytes The size of each chunk in bytes.
 */
static inline void turbo_tcp_config_set_pool_chunk_size(size_t bytes) {
  turbo_config_set_uint(TURBO_TCP_POOL_CHUNK_SIZE, bytes);
}
/**
 * @brief Sets the minimum and maximum limits for the TCP read buffer size.
 * @param min_bytes The minimum read buffer size.
 * @param max_bytes The maximum read buffer size.
 */
static inline void turbo_tcp_config_set_read_buffer_limits(size_t min_bytes,
                                                          size_t max_bytes) {
  turbo_config_set_uint(TURBO_TCP_READ_BUF_MIN, min_bytes);
  turbo_config_set_uint(TURBO_TCP_READ_BUF_MAX, max_bytes);
}
/**
 * @brief Sets the default TCP read buffer size.
 * @param bytes The desired read buffer size.
 */
static inline void turbo_tcp_config_set_read_buffer_size(size_t bytes) {
  turbo_config_set_uint(TURBO_TCP_READ_BUF_SIZE, bytes);
}
/**
 * @brief Sets the client-specific TCP buffer size.
 * @param bytes The desired client buffer size.
 */
static inline void turbo_tcp_config_set_client_buffer_size(size_t bytes) {
  turbo_config_set_uint(TURBO_TCP_CLIENT_BUFFER_SIZE, bytes);
}
/**
 * @brief Sets the maximum number of scatter/gather write buffers (iov) for TCP.
 * @param count The maximum count of iovs.
 */
static inline void turbo_tcp_config_set_max_write_iov(size_t count) {
  turbo_config_set_uint(TURBO_TCP_MAX_WRITE_IOV, count);
}

/* KCP Configuration Setters */
/**
 * @brief Sets the KCP conversation ID base.
 * @param base The base value for KCP conversation IDs.
 */
static inline void turbo_kcp_config_set_conv_base(uint32_t base) {
  turbo_config_set_uint(TURBO_KCP_CONV_BASE, base);
}
/**
 * @brief Sets the KCP nodelay mode.
 * @param nodelay 0: normal mode, 1: no delay mode.
 */
static inline void turbo_kcp_config_set_nodelay(int nodelay) {
  turbo_config_set_int(TURBO_KCP_NODELAY, nodelay);
}
/**
 * @brief Sets the KCP update interval.
 * @param interval The update interval in milliseconds.
 */
static inline void turbo_kcp_config_set_interval(int interval) {
  turbo_config_set_int(TURBO_KCP_INTERVAL, interval);
}
/**
 * @brief Sets the KCP fast resend mode.
 * @param resend 0: normal, 1: enable fast resend.
 */
static inline void turbo_kcp_config_set_resend(int resend) {
  turbo_config_set_int(TURBO_KCP_RESEND, resend);
}
/**
 * @brief Sets the KCP no-congestion-control mode.
 * @param nc 0: normal, 1: disable congestion control.
 */
static inline void turbo_kcp_config_set_nc(int nc) {
  turbo_config_set_int(TURBO_KCP_NC, nc);
}
/**
 * @brief Sets the KCP maximum transmission unit.
 * @param mtu The MTU value in bytes.
 */
static inline void turbo_kcp_config_set_mtu(int mtu) {
  turbo_config_set_int(TURBO_KCP_MTU, mtu);
}
/**
 * @brief Sets the KCP maximum segment size.
 * @param mss The MSS value in bytes.
 */
static inline void turbo_kcp_config_set_mss(int mss) {
  turbo_config_set_int(TURBO_KCP_MSS, mss);
}
/**
 * @brief Sets the KCP send window size.
 * @param wnd The send window size.
 */
static inline void turbo_kcp_config_set_snd_wnd(int wnd) {
  turbo_config_set_int(TURBO_KCP_SND_WND, wnd);
}
/**
 * @brief Sets the KCP receive window size.
 * @param wnd The receive window size.
 */
static inline void turbo_kcp_config_set_rcv_wnd(int wnd) {
  turbo_config_set_int(TURBO_KCP_RCV_WND, wnd);
}

/* Pipe Configuration Setters */
/**
 * @brief Sets the Pipe receive buffer size.
 * @param size The desired receive buffer size in bytes.
 */
static inline void turbo_pipe_config_set_recv_buffer_size(int size) {
  turbo_config_set_int(TURBO_PIPE_RECV_BUFFER_SIZE, size);
}
/**
 * @brief Sets the Pipe send buffer size.
 * @param size The desired send buffer size in bytes.
 */
static inline void turbo_pipe_config_set_send_buffer_size(int size) {
  turbo_config_set_int(TURBO_PIPE_SEND_BUFFER_SIZE, size);
}
/**
 * @brief Sets the number of bytes to batch for Pipe operations.
 * @param bytes The number of bytes to batch.
 */
static inline void turbo_pipe_config_set_batch_bytes(size_t bytes) {
  turbo_config_set_uint(TURBO_PIPE_BATCH_BYTES, bytes);
}
/**
 * @brief Configures the Pipe arena memory pool parameters.
 * @param max_free The maximum number of free arena slices to keep.
 * @param region_hint The hint for the size of memory regions to allocate.
 */
static inline void turbo_pipe_config_set_arena_pool(size_t max_free,
                                                   size_t region_hint) {
  turbo_config_set_uint(TURBO_PIPE_ARENA_FREE_MAX, max_free);
  turbo_config_set_uint(TURBO_PIPE_ARENA_REGION_HINT, region_hint);
}
/**
 * @brief Sets the Pipe connection backlog size for listening sockets.
 * @param backlog The maximum length of the queue of pending connections.
 */
static inline void turbo_pipe_config_set_backlog(unsigned int backlog) {
  turbo_config_set_uint(TURBO_PIPE_BACKLOG, backlog);
}
/**
 * @brief Sets the chunk size for the Pipe memory pool.
 * @param bytes The size of each chunk in bytes.
 */
static inline void turbo_pipe_config_set_pool_chunk_size(size_t bytes) {
  turbo_config_set_uint(TURBO_PIPE_POOL_CHUNK_SIZE, bytes);
}
/**
 * @brief Sets the minimum and maximum limits for the Pipe read buffer size.
 * @param min_bytes The minimum read buffer size.
 * @param max_bytes The maximum read buffer size.
 */
static inline void turbo_pipe_config_set_read_buffer_limits(size_t min_bytes,
                                                           size_t max_bytes) {
  turbo_config_set_uint(TURBO_PIPE_READ_BUF_MIN, min_bytes);
  turbo_config_set_uint(TURBO_PIPE_READ_BUF_MAX, max_bytes);
}
/**
 * @brief Sets the default Pipe read buffer size.
 * @param bytes The desired read buffer size.
 */
static inline void turbo_pipe_config_set_read_buffer_size(size_t bytes) {
  turbo_config_set_uint(TURBO_PIPE_READ_BUF_SIZE, bytes);
}
/**
 * @brief Sets the client-specific Pipe buffer size.
 * @param bytes The desired client buffer size.
 */
static inline void turbo_pipe_config_set_client_buffer_size(size_t bytes) {
  turbo_config_set_uint(TURBO_PIPE_CLIENT_BUFFER_SIZE, bytes);
}
/**
 * @brief Sets the maximum number of scatter/gather write buffers (iov) for Pipe.
 * @param count The maximum count of iovs.
 */
static inline void turbo_pipe_config_set_max_write_iov(size_t count) {
  turbo_config_set_uint(TURBO_PIPE_MAX_WRITE_IOV, count);
}

/* MTCP Configuration Setters */
/**
 * @brief Sets the host address for MTCP.
 * @param host The host address string.
 */
static inline void turbo_mtcp_config_set_host(const char *host) {
  if (host)
    turbo_config_set_string(TURBO_MTCP_HOST, host);
}
/**
 * @brief Sets the port number for MTCP.
 * @param port The port number.
 */
static inline void turbo_mtcp_config_set_port(unsigned int port) {
  turbo_config_set_uint(TURBO_MTCP_PORT, port);
}
/**
 * @brief Sets the path to the MTCP worker executable.
 * @param path The path to the worker executable.
 */
static inline void turbo_mtcp_config_set_worker_path(const char *path) {
  turbo_config_set_string(TURBO_MTCP_WORKER_PATH, (path && path[0]) ? path : "");
}
/**
 * @brief Sets the number of MTCP worker processes to spawn.
 * @param count The number of worker processes.
 */
static inline void turbo_mtcp_config_set_worker_count(unsigned int count) {
  turbo_config_set_uint(TURBO_MTCP_WORKER_COUNT, count);
}
/**
 * @brief Sets the handshake token character for MTCP.
 * @param token The handshake token character.
 */
static inline void turbo_mtcp_config_set_handshake_token(char token) {
  char buf[2] = {token ? token : ' ', '\0'};
  turbo_config_set_string(TURBO_MTCP_HANDSHAKE_TOKEN, buf);
}

/* TCP Configuration Getters */
/**
 * @brief Gets the TCP receive buffer size.
 * @return The TCP receive buffer size in bytes.
 */
static inline int turbo_tcp_config_get_recv_buffer_size(void) {
  return (int)turbo_config_get_int(TURBO_TCP_RECV_BUFFER_SIZE, 256 * 1024);
}
/**
 * @brief Gets the TCP send buffer size.
 * @return The TCP send buffer size in bytes.
 */
static inline int turbo_tcp_config_get_send_buffer_size(void) {
  return (int)turbo_config_get_int(TURBO_TCP_SEND_BUFFER_SIZE, 256 * 1024);
}
/**
 * @brief Gets the number of bytes to batch for TCP operations.
 * @return The number of bytes to batch.
 */
static inline size_t turbo_tcp_config_get_batch_bytes(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_BATCH_BYTES, 0);
}
/**
 * @brief Gets the maximum number of free arena slices to keep for TCP.
 * @return The maximum number of free arena slices.
 */
static inline size_t turbo_tcp_config_get_arena_free_max(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_ARENA_FREE_MAX, 16);
}
/**
 * @brief Gets the hint for the size of memory regions to allocate for TCP arena.
 * @return The region hint size in bytes.
 */
static inline size_t turbo_tcp_config_get_arena_region_hint(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_ARENA_REGION_HINT, 1024 * 1024);
}
/**
 * @brief Gets the TCP connection backlog size.
 * @return The backlog size.
 */
static inline unsigned int turbo_tcp_config_get_backlog(void) {
  return (unsigned int)turbo_config_get_uint(TURBO_TCP_BACKLOG, 128);
}
/**
 * @brief Gets the chunk size for the TCP memory pool.
 * @return The chunk size in bytes.
 */
static inline size_t turbo_tcp_config_get_pool_chunk_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_POOL_CHUNK_SIZE, 64 * 1024);
}
/**
 * @brief Gets the minimum TCP read buffer size.
 * @return The minimum read buffer size in bytes.
 */
static inline size_t turbo_tcp_config_get_read_buf_min(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_READ_BUF_MIN, 16 * 1024);
}
/**
 * @brief Gets the maximum TCP read buffer size.
 * @return The maximum read buffer size in bytes.
 */
static inline size_t turbo_tcp_config_get_read_buf_max(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_READ_BUF_MAX, 4 * 1024 * 1024);
}
/**
 * @brief Gets the default TCP read buffer size.
 * @return The default read buffer size in bytes.
 */
static inline size_t turbo_tcp_config_get_read_buf_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_READ_BUF_SIZE, 64 * 1024);
}
/**
 * @brief Gets the client-specific TCP buffer size.
 * @return The client buffer size in bytes.
 */
static inline size_t turbo_tcp_config_get_client_buffer_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_CLIENT_BUFFER_SIZE, 8192);
}
/**
 * @brief Gets the maximum number of scatter/gather write buffers (iov) for TCP.
 * @return The maximum count of iovs.
 */
static inline size_t turbo_tcp_config_get_max_write_iov(void) {
  return (size_t)turbo_config_get_uint(TURBO_TCP_MAX_WRITE_IOV, 64);
}

/* KCP Configuration Getters */
/**
 * @brief Gets the KCP conversation ID base.
 * @return The base value for KCP conversation IDs.
 */
static inline uint32_t turbo_kcp_config_get_conv_base(void) {
  return (uint32_t)turbo_config_get_uint(TURBO_KCP_CONV_BASE, 0x10000000u);
}
/**
 * @brief Gets the KCP nodelay mode.
 * @return 0: normal mode, 1: no delay mode.
 */
static inline int turbo_kcp_config_get_nodelay(void) {
  return (int)turbo_config_get_int(TURBO_KCP_NODELAY, 1);
}
/**
 * @brief Gets the KCP update interval.
 * @return The update interval in milliseconds.
 */
static inline int turbo_kcp_config_get_interval(void) {
  return (int)turbo_config_get_int(TURBO_KCP_INTERVAL, 10);
}
/**
 * @brief Gets the KCP fast resend mode.
 * @return 0: normal, 1: enable fast resend.
 */
static inline int turbo_kcp_config_get_resend(void) {
  return (int)turbo_config_get_int(TURBO_KCP_RESEND, 2);
}
/**
 * @brief Gets the KCP no-congestion-control mode.
 * @return 0: normal, 1: disable congestion control.
 */
static inline int turbo_kcp_config_get_nc(void) {
  return (int)turbo_config_get_int(TURBO_KCP_NC, 1);
}
/**
 * @brief Gets the KCP maximum transmission unit.
 * @return The MTU value in bytes.
 */
static inline int turbo_kcp_config_get_mtu(void) {
  return (int)turbo_config_get_int(TURBO_KCP_MTU, TURBO_KCP_DEFAULT_MTU);
}
/**
 * @brief Gets the KCP maximum segment size.
 * @return The MSS value in bytes.
 */
static inline int turbo_kcp_config_get_mss(void) {
  return (int)turbo_config_get_int(TURBO_KCP_MSS, TURBO_KCP_DEFAULT_MSS);
}
/**
 * @brief Gets the KCP send window size.
 * @return The send window size.
 */
static inline int turbo_kcp_config_get_snd_wnd(void) {
  return (int)turbo_config_get_int(TURBO_KCP_SND_WND, TURBO_KCP_DEFAULT_SND_WND);
}
/**
 * @brief Gets the KCP receive window size.
 * @return The receive window size.
 */
static inline int turbo_kcp_config_get_rcv_wnd(void) {
  return (int)turbo_config_get_int(TURBO_KCP_RCV_WND, TURBO_KCP_DEFAULT_RCV_WND);
}

/* Pipe Configuration Getters */
/**
 * @brief Gets the Pipe receive buffer size.
 * @return The Pipe receive buffer size in bytes.
 */
static inline int turbo_pipe_config_get_recv_buffer_size(void) {
  return (int)turbo_config_get_int(TURBO_PIPE_RECV_BUFFER_SIZE, 256 * 1024);
}
/**
 * @brief Gets the Pipe send buffer size.
 * @return The Pipe send buffer size in bytes.
 */
static inline int turbo_pipe_config_get_send_buffer_size(void) {
  return (int)turbo_config_get_int(TURBO_PIPE_SEND_BUFFER_SIZE, 256 * 1024);
}
/**
 * @brief Gets the number of bytes to batch for Pipe operations.
 * @return The number of bytes to batch.
 */
static inline size_t turbo_pipe_config_get_batch_bytes(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_BATCH_BYTES, 0);
}
/**
 * @brief Gets the maximum number of free arena slices to keep for Pipe.
 * @return The maximum number of free arena slices.
 */
static inline size_t turbo_pipe_config_get_arena_free_max(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_ARENA_FREE_MAX, 16);
}
/**
 * @brief Gets the hint for the size of memory regions to allocate for Pipe arena.
 * @return The region hint size in bytes.
 */
static inline size_t turbo_pipe_config_get_arena_region_hint(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_ARENA_REGION_HINT, 1024 * 1024);
}
/**
 * @brief Gets the Pipe connection backlog size.
 * @return The backlog size.
 */
static inline unsigned int turbo_pipe_config_get_backlog(void) {
  return (unsigned int)turbo_config_get_uint(TURBO_PIPE_BACKLOG, 128);
}
/**
 * @brief Gets the chunk size for the Pipe memory pool.
 * @return The chunk size in bytes.
 */
static inline size_t turbo_pipe_config_get_pool_chunk_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_POOL_CHUNK_SIZE, 64 * 1024);
}
/**
 * @brief Gets the minimum Pipe read buffer size.
 * @return The minimum read buffer size in bytes.
 */
static inline size_t turbo_pipe_config_get_read_buf_min(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_READ_BUF_MIN, 16 * 1024);
}
/**
 * @brief Gets the maximum Pipe read buffer size.
 * @return The maximum read buffer size in bytes.
 */
static inline size_t turbo_pipe_config_get_read_buf_max(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_READ_BUF_MAX, 4 * 1024 * 1024);
}
/**
 * @brief Gets the default Pipe read buffer size.
 * @return The default read buffer size in bytes.
 */
static inline size_t turbo_pipe_config_get_read_buf_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_READ_BUF_SIZE, 64 * 1024);
}
/**
 * @brief Gets the client-specific Pipe buffer size.
 * @return The client buffer size in bytes.
 */
static inline size_t turbo_pipe_config_get_client_buffer_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_CLIENT_BUFFER_SIZE, 8192);
}
/**
 * @brief Gets the maximum number of scatter/gather write buffers (iov) for Pipe.
 * @return The maximum count of iovs.
 */
static inline size_t turbo_pipe_config_get_max_write_iov(void) {
  return (size_t)turbo_config_get_uint(TURBO_PIPE_MAX_WRITE_IOV, 64);
}

/* MTCP Configuration Getters */
/**
 * @brief Gets the host address for MTCP.
 * @return The host address string.
 */
static inline const char *turbo_mtcp_config_get_host(void) {
  return turbo_config_get_string(TURBO_MTCP_HOST, "0.0.0.0");
}
/**
 * @brief Gets the port number for MTCP.
 * @return The port number.
 */
static inline unsigned short turbo_mtcp_config_get_port(void) {
  return (unsigned short)turbo_config_get_uint(TURBO_MTCP_PORT, 7000);
}
/**
 * @brief Gets the path to the MTCP worker executable.
 * @return The path to the worker executable.
 */
static inline const char *turbo_mtcp_config_get_worker_path(void) {
  return turbo_config_get_string(TURBO_MTCP_WORKER_PATH, "");
}
/**
 * @brief Gets the number of MTCP worker processes to spawn.
 * @return The number of worker processes.
 */
static inline unsigned int turbo_mtcp_config_get_worker_count(void) {
  return (unsigned int)turbo_config_get_uint(TURBO_MTCP_WORKER_COUNT, 0);
}
/**
 * @brief Gets the handshake token character for MTCP.
 * @return The handshake token character.
 */
static inline char turbo_mtcp_config_get_handshake_token(void) {
  const char *token = turbo_config_get_string(TURBO_MTCP_HANDSHAKE_TOKEN, " ");
  return (token && token[0]) ? token[0] : ' ';
}

/* UDP Configuration Getters */
/**
 * @brief Gets the chunk size for the UDP memory pool.
 * @return The chunk size in bytes.
 */
static inline size_t turbo_udp_config_get_pool_chunk_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_UDP_POOL_CHUNK_SIZE, 32 * 1024);
}
/**
 * @brief Gets the UDP receive buffer size.
 * @return The UDP receive buffer size in bytes.
 */
static inline size_t turbo_udp_config_get_recv_buf_size(void) {
  return (size_t)turbo_config_get_uint(TURBO_UDP_RECV_BUF_SIZE, 64 * 1024);
}
/**
 * @brief Gets the minimum UDP receive buffer size.
 * @return The minimum UDP receive buffer size in bytes.
 */
static inline size_t turbo_udp_config_get_recv_buf_min(void) {
  return (size_t)turbo_config_get_uint(TURBO_UDP_RECV_BUF_MIN, 8 * 1024);
}
/**
 * @brief Gets the maximum UDP receive buffer size.
 * @return The maximum UDP receive buffer size in bytes.
 */
static inline size_t turbo_udp_config_get_recv_buf_max(void) {
  return (size_t)turbo_config_get_uint(TURBO_UDP_RECV_BUF_MAX, 2 * 1024 * 1024);
}



#ifdef __cplusplus
}
#endif

#endif /* turbo_CONFIG_H */

