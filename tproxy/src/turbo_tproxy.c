#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#if defined(_MSC_VER) && !defined(__clang__)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
 

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include "turbo_async_client.h"
#include "turbo_async_server.h"
#include "turbo_tproxy.h"
#define TPROXY_BUFFER_SIZE (64 * 1024)
#define TPROXY_DEFAULT_MAX_CONNECTIONS 1024
#define TPROXY_SOCKS5_VERSION 0x05
#define TPROXY_SOCKS5_CMD_CONNECT 0x01
#define TPROXY_SOCKS5_ADDR_TYPE_IPV4 0x01
#define TPROXY_SOCKS5_ADDR_TYPE_DOMAIN 0x03
#define TPROXY_SOCKS5_ADDR_TYPE_IPV6 0x04
#define TPROXY_SOCKS5_MIN_REQUEST 10
#define TPROXY_SOCKS5_MAX_REQUEST 512

typedef enum {
  TPROXY_CONN_STATE_READING_SOCKS5,
  TPROXY_CONN_STATE_CONNECTING,
  TPROXY_CONN_STATE_CONNECTED,
  TPROXY_CONN_STATE_CLOSING
} tproxy_conn_state_t;

typedef struct tproxy_upstream_s {
  async_client_t *client;
  struct tproxy_upstream_s *next;
} tproxy_upstream_t;

typedef struct tproxy_connection_s {
  tproxy_server_t *server;
  async_server_connection_t *server_conn;
  tproxy_conn_state_t state;
  char target_host[256];
  int target_port;
  char client_host[64];
  int client_port;
  unsigned char socks5_buf[TPROXY_SOCKS5_MAX_REQUEST];
  size_t socks5_len;
  tproxy_upstream_t *upstream;
  uint64_t bytes_received;
  uint64_t bytes_sent;
  struct tproxy_connection_s *next;
} tproxy_connection_t;

typedef struct tproxy_server_s {
  tproxy_config_t config;
  tproxy_event_cb callback;
  void *user_data;
  uv_loop_t *loop;
  async_server_t *server;
  tproxy_connection_t *connections;
  int active_connections;
  uint64_t total_connections;
  uint64_t total_bytes_received;
  uint64_t total_bytes_sent;
  int stopping;
} tproxy_server_t;

/* Forward declarations */
static void on_server_event(async_server_t *srv, const async_server_event_t *event,
                            void *user_data);
static void on_upstream_event(async_client_t *client, const async_client_event_t *event,
                               void *user_data);
static void close_connection(tproxy_server_t *server, tproxy_connection_t *conn);
static void free_connection(tproxy_connection_t *conn);
static tproxy_connection_t *create_connection(tproxy_server_t *server,
                                              async_server_connection_t *server_conn);
static void remove_connection(tproxy_server_t *server, tproxy_connection_t *conn);
static int parse_socks5_request(tproxy_connection_t *conn, const char *data, size_t len);
static void emit_event(tproxy_server_t *server, tproxy_event_type_t type,
                       tproxy_connection_t *conn, int status,
                       const char *message, const char *data, size_t length);
static int connect_upstream(tproxy_connection_t *conn);
static void forward_data(tproxy_connection_t *conn, const char *data, size_t len);

/* String conversion */
const char *tproxy_event_type_to_string(tproxy_event_type_t type) {
  switch (type) {
    case TPROXY_EVENT_LISTENING: return "listening";
    case TPROXY_EVENT_CONNECTION: return "connection";
    case TPROXY_EVENT_DATA: return "data";
    case TPROXY_EVENT_DISCONNECT: return "disconnect";
    case TPROXY_EVENT_ERROR: return "error";
    case TPROXY_EVENT_UPSTREAM_CONNECTED: return "upstream_connected";
    case TPROXY_EVENT_CLOSED: return "closed";
    default: return "unknown";
  }
}

/* Create server */
tproxy_server_t *tproxy_server_create(const tproxy_config_t *config,
                                      tproxy_event_cb callback,
                                      void *user_data) {
  if (!config || !callback) {
    return NULL;
  }

  tproxy_server_t *server = (tproxy_server_t *)calloc(1, sizeof(tproxy_server_t));
  if (!server) {
    return NULL;
  }

  server->config = *config;
  server->callback = callback;
  server->user_data = user_data;
  server->loop = uv_default_loop();

  if (server->config.buffer_size == 0) {
    server->config.buffer_size = TPROXY_BUFFER_SIZE;
  }

  if (server->config.max_connections == 0) {
    server->config.max_connections = TPROXY_DEFAULT_MAX_CONNECTIONS;
  }

  server->server = async_server_create(on_server_event, server);
  if (!server->server) {
    free(server);
    return NULL;
  }

  return server;
}

/* Destroy server */
void tproxy_server_destroy(tproxy_server_t *server) {
  if (!server) return;

  tproxy_server_stop(server);

  if (server->server) {
    async_server_destroy(server->server);
    server->server = NULL;
  }

  /* Free all connections */
  tproxy_connection_t *conn = server->connections;
  while (conn) {
    tproxy_connection_t *next = conn->next;
    free_connection(conn);
    conn = next;
  }

  free(server);
}

/* Start server */
turbo_client_status_t tproxy_server_start(tproxy_server_t *server) {
  if (!server) {
    return TURBO_CLIENT_STATUS_INVALID_PARAM;
  }

  char listen_url[256];
  if (server->config.listen_host) {
    snprintf(listen_url, sizeof(listen_url), "tcp://%s:%d", server->config.listen_host, server->config.listen_port);
  } else {
    snprintf(listen_url, sizeof(listen_url), "tcp://0.0.0.0:%d", server->config.listen_port);
  }

  async_server_status_t status = async_server_listen(
      server->server,
      listen_url,
      server->config.max_connections
  );

  return (turbo_client_status_t)status;
}

/* Stop server */
void tproxy_server_stop(tproxy_server_t *server) {
  if (!server || server->stopping) return;

  server->stopping = 1;

  /* Close all connections */
  tproxy_connection_t *conn = server->connections;
  while (conn) {
    tproxy_connection_t *next = conn->next;
    close_connection(server, conn);
    conn = next;
  }

  /* Stop server - this will trigger on_server_event with CLOSED */
  async_server_destroy(server->server);
  server->server = NULL;
}

/* Send data to connection */
turbo_client_status_t tproxy_server_send(tproxy_server_t *server,
                                         tproxy_connection_t *connection,
                                         const char *data,
                                         size_t length) {
  if (!server || !connection || !data) {
    return TURBO_CLIENT_STATUS_INVALID_PARAM;
  }

  async_server_status_t status = async_server_send(server->server, connection->server_conn, data, length);
  if (status == ASYNC_SERVER_STATUS_OK) {
    connection->bytes_sent += length;
    server->total_bytes_sent += length;
  }

  return (turbo_client_status_t)status;
}

/* Close connection */
void tproxy_connection_close(tproxy_server_t *server, tproxy_connection_t *connection) {
  if (!server || !connection) return;
  close_connection(server, connection);
}

/* Get original destination */
int tproxy_connection_get_original_dest(tproxy_server_t *server,
                                         tproxy_connection_t *connection,
                                         char *host,
                                         size_t host_len,
                                         int *port) {
  (void)server;

  if (!connection) return -1;

  if (host && host_len > 0) {
    strncpy(host, connection->target_host, host_len - 1);
    host[host_len - 1] = '\0';
  }

  if (port) {
    *port = connection->target_port;
  }

  return 0;
}

/* Get client address */
int tproxy_connection_get_client_addr(tproxy_server_t *server,
                                      tproxy_connection_t *connection,
                                      char *host,
                                      size_t host_len,
                                      int *port) {
  (void)server;

  if (!connection) return -1;

  if (host && host_len > 0) {
    strncpy(host, connection->client_host, host_len - 1);
    host[host_len - 1] = '\0';
  }

  if (port) {
    *port = connection->client_port;
  }

  return 0;
}

/* Get statistics */
void tproxy_server_get_stats(tproxy_server_t *server,
                             int *active_connections,
                             uint64_t *total_connections,
                             uint64_t *bytes_received,
                             uint64_t *bytes_sent) {
  if (!server) {
    if (active_connections) *active_connections = -1;
    if (total_connections) *total_connections = 0;
    if (bytes_received) *bytes_received = 0;
    if (bytes_sent) *bytes_sent = 0;
    return;
  }

  if (active_connections) *active_connections = server->active_connections;
  if (total_connections) *total_connections = server->total_connections;
  if (bytes_received) *bytes_received = server->total_bytes_received;
  if (bytes_sent) *bytes_sent = server->total_bytes_sent;
}

/* Server event handler */
static void on_server_event(async_server_t *srv, const async_server_event_t *event,
                            void *user_data) {
  tproxy_server_t *server = (tproxy_server_t *)user_data;
  (void)srv;

  switch (event->type) {
    case ASYNC_SERVER_EVENT_LISTENING: {
      emit_event(server, TPROXY_EVENT_LISTENING, NULL, 0, NULL, NULL, 0);
      break;
    }

    case ASYNC_SERVER_EVENT_CONNECTION: {
      tproxy_connection_t *conn = create_connection(server, event->connection);
      if (!conn)
        return;

      /* Send event */
      emit_event(server, TPROXY_EVENT_CONNECTION, conn, 0, NULL, NULL, 0);

      /* If not using SOCKS5, connect immediately */
      if (!server->config.enable_socks5) {
        connect_upstream(conn);
      }
      break;
    }

    case ASYNC_SERVER_EVENT_DATA: {
      tproxy_connection_t *conn = (tproxy_connection_t *)async_server_connection_get_user_data(event->connection);
      if (!conn) return;

      size_t len = event->length;
      conn->bytes_received += len;
      server->total_bytes_received += len;

      if (conn->state == TPROXY_CONN_STATE_READING_SOCKS5) {
        /* Parse SOCKS5 request */
        int rc = parse_socks5_request(conn, event->data, len);
        if (rc == 0) {
          conn->state = TPROXY_CONN_STATE_CONNECTING;
          connect_upstream(conn);
        } else if (rc < 0) {
          close_connection(server, conn);
        }
      } else if (conn->state == TPROXY_CONN_STATE_CONNECTED) {
        /* Forward data to upstream */
        if (conn->upstream && conn->upstream->client) {
          async_client_send(conn->upstream->client, event->data, len);
        }
      }
      break;
    }

    case ASYNC_SERVER_EVENT_DISCONNECTION: {
      tproxy_connection_t *conn = (tproxy_connection_t *)async_server_connection_get_user_data(event->connection);
      if (conn) {
        close_connection(server, conn);
      }
      break;
    }

    case ASYNC_SERVER_EVENT_ERROR: {
      emit_event(server, TPROXY_EVENT_ERROR, NULL, event->status, "Server error", NULL, 0);
      break;
    }

    case ASYNC_SERVER_EVENT_CLOSED: {
      emit_event(server, TPROXY_EVENT_CLOSED, NULL, 0, NULL, NULL, 0);
      break;
    }
  }
}

/* Upstream event handler */
static void on_upstream_event(async_client_t *client, const async_client_event_t *event,
                              void *user_data) {
  tproxy_connection_t *conn = (tproxy_connection_t *)user_data;
  (void)client;
  if (!conn) return;

  switch (event->type) {
    case ASYNC_CLIENT_EVENT_CONNECTED: {
      conn->state = TPROXY_CONN_STATE_CONNECTED;

      /* Send SOCKS5 reply if needed */
      if (conn->server->config.enable_socks5) {
        /* SOCKS5 success reply */
        unsigned char reply[] = {
          0x05, /* Version */
          0x00, /* Success */
          0x00, /* Reserved */
          0x01, /* IPv4 address */
          0x00, 0x00, 0x00, 0x00, /* IP address */
          0x00, 0x00 /* Port */
        };
        async_server_send(conn->server->server, conn->server_conn, (char *)reply, sizeof(reply));
      }

      /* Send event */
      emit_event(conn->server, TPROXY_EVENT_UPSTREAM_CONNECTED, conn, 0, NULL, NULL, 0);
      break;
    }

    case ASYNC_CLIENT_EVENT_DATA: {
      if (conn->state == TPROXY_CONN_STATE_CONNECTED) {
        forward_data(conn, event->data, event->length);
      }
      break;
    }

    case ASYNC_CLIENT_EVENT_ERROR:
    case ASYNC_CLIENT_EVENT_CLOSED: {
      close_connection(conn->server, conn);
      break;
    }
  }
}

/* Close connection */
static void close_connection(tproxy_server_t *server, tproxy_connection_t *conn) {
  if (!conn || conn->state == TPROXY_CONN_STATE_CLOSING) return;

  conn->state = TPROXY_CONN_STATE_CLOSING;

  /* Close upstream */
  if (conn->upstream) {
    if (conn->upstream->client) {
      async_client_close(conn->upstream->client);
    }
    free(conn->upstream);
    conn->upstream = NULL;
  }

  /* Send disconnect event */
  emit_event(server, TPROXY_EVENT_DISCONNECT, conn, 0, NULL, NULL, 0);
  remove_connection(server, conn);

  server->active_connections--;
  free_connection(conn);
}

static tproxy_connection_t *create_connection(tproxy_server_t *server,
                                              async_server_connection_t *server_conn) {
  tproxy_connection_t *conn = (tproxy_connection_t *)calloc(1, sizeof(*conn));
  if (!conn)
    return NULL;

  conn->server = server;
  conn->server_conn = server_conn;
  conn->state =
      server->config.enable_socks5 ? TPROXY_CONN_STATE_READING_SOCKS5 : TPROXY_CONN_STATE_CONNECTING;

  conn->next = server->connections;
  server->connections = conn;
  server->active_connections++;
  server->total_connections++;

  async_server_connection_set_user_data(server_conn, conn);

  return conn;
}

static void remove_connection(tproxy_server_t *server, tproxy_connection_t *conn) {
  if (server->connections == conn) {
    server->connections = conn->next;
  } else {
    tproxy_connection_t *prev = server->connections;
    while (prev && prev->next != conn) {
      prev = prev->next;
    }
    if (prev) {
      prev->next = conn->next;
    }
  }
  async_server_connection_set_user_data(conn->server_conn, NULL);
}

/* Free connection */
static void free_connection(tproxy_connection_t *conn) {
  if (!conn) return;

  free(conn);
}

/* Parse SOCKS5 request */
static int parse_socks5_request(tproxy_connection_t *conn, const char *data, size_t len) {
  if (!conn || !data || len == 0)
    return -1;

  if (conn->socks5_len + len > sizeof(conn->socks5_buf))
    return -1;

  memcpy(conn->socks5_buf + conn->socks5_len, data, len);
  conn->socks5_len += len;

  if (conn->socks5_len < 4)
    return 1;

  if (conn->socks5_buf[0] != TPROXY_SOCKS5_VERSION)
    return -1;

  if (conn->socks5_buf[1] != TPROXY_SOCKS5_CMD_CONNECT)
    return -1;

  unsigned char addr_type = conn->socks5_buf[3];
  size_t offset = 4;
  size_t need = 0;

  if (addr_type == TPROXY_SOCKS5_ADDR_TYPE_IPV4) {
    need = TPROXY_SOCKS5_MIN_REQUEST;
    if (conn->socks5_len < need)
      return 1;
    struct in_addr addr;
    memcpy(&addr, conn->socks5_buf + offset, 4);
    inet_ntop(AF_INET, &addr, conn->target_host, sizeof(conn->target_host));
    offset += 4;
  } else if (addr_type == TPROXY_SOCKS5_ADDR_TYPE_IPV6) {
    need = 22;
    if (conn->socks5_len < need)
      return 1;
    struct in6_addr addr;
    memcpy(&addr, conn->socks5_buf + offset, 16);
    inet_ntop(AF_INET6, &addr, conn->target_host, sizeof(conn->target_host));
    offset += 16;
  } else if (addr_type == TPROXY_SOCKS5_ADDR_TYPE_DOMAIN) {
    if (conn->socks5_len < 5)
      return 1;
    unsigned char domain_len = conn->socks5_buf[offset];
    offset++;
    need = 4 + 1 + domain_len + 2;
    if (need > sizeof(conn->socks5_buf))
      return -1;
    if (conn->socks5_len < need)
      return 1;
    memcpy(conn->target_host, conn->socks5_buf + offset, domain_len);
    conn->target_host[domain_len] = '\0';
    offset += domain_len;
  } else {
    return -1;
  }

  if (offset + 2 > conn->socks5_len)
    return 1;

  conn->target_port = ((unsigned char)conn->socks5_buf[offset] << 8) |
                      (unsigned char)conn->socks5_buf[offset + 1];
  return 0;
}

/* Connect to upstream */
static int connect_upstream(tproxy_connection_t *conn) {
  tproxy_upstream_t *upstream = (tproxy_upstream_t *)malloc(sizeof(tproxy_upstream_t));
  if (!upstream) {
    return -1;
  }

  /* Create upstream client */
  upstream->client = async_client_create(on_upstream_event, conn);
  if (!upstream->client) {
    free(upstream);
    return -1;
  }

  conn->upstream = upstream;

  /* Connect to target */
  const char *host = conn->server->config.upstream_host ? conn->server->config.upstream_host : conn->target_host;
  int port = conn->server->config.upstream_host ? conn->server->config.upstream_port : conn->target_port;
  
  char connect_url[256];
  snprintf(connect_url, sizeof(connect_url), "tcp://%s:%d", host, port);

  async_client_connect(upstream->client, connect_url);

  return 0;
}

/* Forward data to client */
static void forward_data(tproxy_connection_t *conn, const char *data, size_t len) {
  async_server_send(conn->server->server, conn->server_conn, data, len);
  conn->bytes_sent += len;
  conn->server->total_bytes_sent += len;
}

static void emit_event(tproxy_server_t *server, tproxy_event_type_t type,
                       tproxy_connection_t *conn, int status,
                       const char *message, const char *data, size_t length) {
  if (!server || !server->callback)
    return;

  tproxy_event_t ev = {
    .type = type,
    .connection = conn,
    .data = data,
    .length = length,
    .status = status,
    .message = message
  };
  server->callback(server, &ev, server->user_data);
}
