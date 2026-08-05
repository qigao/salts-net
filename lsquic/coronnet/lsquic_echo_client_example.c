#include "CoroNet/turbo_dns.h"
#include "CoroNet/turbo_lsquic.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/ssl.h>

#define LSQUIC_ECHO_ALPN "echo"
#define LSQUIC_ECHO_BIND_HOST_IPV4 "0.0.0.0"
#define LSQUIC_ECHO_BIND_HOST_IPV6 "::"
#define LSQUIC_ECHO_HANDSHAKE_TIMEOUT_US 10000000UL
#define LSQUIC_ECHO_IDLE_TIMEOUT_SECONDS 15U
#define LSQUIC_ECHO_MESSAGE_CAPACITY 4096U
#define LSQUIC_ECHO_READ_CAPACITY 1024U

typedef enum lsquic_example_mode_e {
  LSQUIC_EXAMPLE_ECHO,
  LSQUIC_EXAMPLE_HTTP3_HANDSHAKE,
} lsquic_example_mode_t;

typedef struct lsquic_echo_app_s {
  coro_context_t *context;
  turbo_lsquic_t *adapter;
  SSL_CTX *ssl_context;
  lsquic_conn_t *connection;
  lsquic_example_mode_t mode;
  char message[LSQUIC_ECHO_MESSAGE_CAPACITY];
  size_t message_size;
  size_t message_offset;
  size_t received_size;
  int result;
} lsquic_echo_app_t;

static void lsquic_echo_close_stream(lsquic_echo_app_t *app, lsquic_stream_t *stream,
                                     const char *operation) {
  fprintf(stderr, "%s failed: %s\n", operation, strerror(errno));
  app->result = EXIT_FAILURE;
  (void)lsquic_stream_close(stream);
}

static lsquic_conn_ctx_t *lsquic_echo_on_new_conn(void *stream_if_ctx, lsquic_conn_t *connection) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)stream_if_ctx;

  app->connection = connection;
  if (app->mode == LSQUIC_EXAMPLE_ECHO) {
    lsquic_conn_make_stream(connection);
  }
  return (lsquic_conn_ctx_t *)app;
}

static void lsquic_echo_on_conn_closed(lsquic_conn_t *connection) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)lsquic_conn_get_ctx(connection);

  if (!app) {
    return;
  }
  app->connection = NULL;
  coro_context_stop(app->context);
}

static lsquic_stream_ctx_t *lsquic_echo_on_new_stream(void *stream_if_ctx,
                                                      lsquic_stream_t *stream) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)stream_if_ctx;

  if (!stream) {
    fprintf(stderr, "LSQUIC rejected the outgoing stream\n");
    app->result = EXIT_FAILURE;
    if (app->connection) {
      lsquic_conn_close(app->connection);
    }
    return NULL;
  }

  (void)lsquic_stream_wantwrite(stream, 1);
  return (lsquic_stream_ctx_t *)app;
}

static void lsquic_echo_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *stream_ctx) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)stream_ctx;
  char buffer[LSQUIC_ECHO_READ_CAPACITY];
  ssize_t read_size;

  do {
    read_size = lsquic_stream_read(stream, buffer, sizeof(buffer));
    if (read_size > 0) {
      size_t size = (size_t)read_size;
      app->received_size += size;
      (void)fwrite(buffer, 1, size, stdout);
      (void)fflush(stdout);
      if (memchr(buffer, '\n', size)) {
        app->result = EXIT_SUCCESS;
        (void)lsquic_stream_close(stream);
        return;
      }
    }
  } while (read_size > 0);

  if (read_size == 0) {
    app->result = app->received_size > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    (void)lsquic_stream_close(stream);
  } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
    lsquic_echo_close_stream(app, stream, "reading the echo");
  }
}

static void lsquic_echo_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *stream_ctx) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)stream_ctx;
  size_t remaining = app->message_size - app->message_offset;
  ssize_t written;

  written = lsquic_stream_write(stream, app->message + app->message_offset, remaining);
  if (written < 0) {
    lsquic_echo_close_stream(app, stream, "writing the request");
    return;
  }

  app->message_offset += (size_t)written;
  if (app->message_offset != app->message_size) {
    return;
  }

  if (lsquic_stream_flush(stream) != 0 || lsquic_stream_shutdown(stream, 1) != 0) {
    lsquic_echo_close_stream(app, stream, "finishing the request");
    return;
  }

  (void)lsquic_stream_wantwrite(stream, 0);
  (void)lsquic_stream_wantread(stream, 1);
}

static void lsquic_echo_on_stream_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *stream_ctx) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)stream_ctx;
  lsquic_conn_t *connection = lsquic_stream_conn(stream);

  if (app && connection) {
    lsquic_conn_close(connection);
  }
}

static void lsquic_echo_on_handshake(lsquic_conn_t *connection, enum lsquic_hsk_status status) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)lsquic_conn_get_ctx(connection);

  if (status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK) {
    if (app && app->mode == LSQUIC_EXAMPLE_HTTP3_HANDSHAKE) {
      printf("HTTP/3 QUIC handshake succeeded\n");
      app->result = EXIT_SUCCESS;
      lsquic_conn_close(connection);
    }
    return;
  }

  fprintf(stderr, "QUIC handshake failed with status %d\n", (int)status);
  if (app) {
    app->result = EXIT_FAILURE;
  }
  lsquic_conn_close(connection);
}

static SSL_CTX *lsquic_echo_get_ssl_context(void *peer_ctx, const struct sockaddr *local_address) {
  lsquic_echo_app_t *app = (lsquic_echo_app_t *)peer_ctx;
  (void)local_address;
  return app ? app->ssl_context : NULL;
}

static const struct lsquic_stream_if s_lsquic_echo_stream_if = {
    .on_new_conn = lsquic_echo_on_new_conn,
    .on_conn_closed = lsquic_echo_on_conn_closed,
    .on_new_stream = lsquic_echo_on_new_stream,
    .on_read = lsquic_echo_on_read,
    .on_write = lsquic_echo_on_write,
    .on_close = lsquic_echo_on_stream_close,
    .on_hsk_done = lsquic_echo_on_handshake,
};

static int lsquic_echo_parse_port(const char *text, unsigned short *out_port) {
  char *end = NULL;
  unsigned long value;

  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value == 0 || value > USHRT_MAX) {
    return -1;
  }

  *out_port = (unsigned short)value;
  return 0;
}

static int lsquic_echo_set_message(lsquic_echo_app_t *app, const char *message) {
  size_t size = strlen(message);
  int append_newline = size == 0 || message[size - 1] != '\n';

  if (size + (size_t)append_newline >= sizeof(app->message)) {
    return -1;
  }

  memcpy(app->message, message, size);
  if (append_newline) {
    app->message[size++] = '\n';
  }
  app->message[size] = '\0';
  app->message_size = size;
  return 0;
}

static void lsquic_echo_usage(const char *program) {
  fprintf(stderr, "Usage: %s <server-ip> <port> <server-name> <message>\n", program);
  fprintf(stderr, "       %s --handshake <server-ip> <port> <server-name>\n", program);
}

int main(int argc, char **argv) {
  struct lsquic_engine_settings settings;
  struct lsquic_engine_api engine_api;
  turbo_lsquic_config_t config;
  struct sockaddr_storage local_address;
  struct sockaddr_storage peer_address;
  lsquic_echo_app_t app;
  const char *server_ip;
  const char *server_name;
  const char *port_text;
  unsigned short port;
  int rc;

  memset(&app, 0, sizeof(app));
  app.result = EXIT_FAILURE;
  if (argc == 5 && strcmp(argv[1], "--handshake") == 0) {
    app.mode = LSQUIC_EXAMPLE_HTTP3_HANDSHAKE;
    server_ip = argv[2];
    port_text = argv[3];
    server_name = argv[4];
  } else if (argc == 5) {
    app.mode = LSQUIC_EXAMPLE_ECHO;
    server_ip = argv[1];
    port_text = argv[2];
    server_name = argv[3];
  } else {
    lsquic_echo_usage(argv[0]);
    return EXIT_FAILURE;
  }
  if (lsquic_echo_parse_port(port_text, &port) != 0) {
    fprintf(stderr, "Invalid UDP port: %s\n", port_text);
    return EXIT_FAILURE;
  }

  if (turbo_dns_parse_address(server_ip, port, &peer_address) != 0) {
    fprintf(stderr, "Invalid IP address: %s\n", server_ip);
    return EXIT_FAILURE;
  }

  if (app.mode == LSQUIC_EXAMPLE_ECHO && lsquic_echo_set_message(&app, argv[4]) != 0) {
    fprintf(stderr, "Message must be shorter than %u bytes\n", LSQUIC_ECHO_MESSAGE_CAPACITY - 1U);
    return EXIT_FAILURE;
  }

  app.context = coro_context_create(NULL);
  if (!app.context) {
    fprintf(stderr, "Failed to create the CoroNet context\n");
    return EXIT_FAILURE;
  }

  app.ssl_context = SSL_CTX_new(TLS_method());
  if (!app.ssl_context || SSL_CTX_set_min_proto_version(app.ssl_context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(app.ssl_context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_default_verify_paths(app.ssl_context) != 1) {
    fprintf(stderr, "Failed to initialize the TLS context\n");
    SSL_CTX_free(app.ssl_context);
    coro_context_destroy(app.context);
    return EXIT_FAILURE;
  }

  lsquic_engine_init_settings(&settings, 0);
  settings.es_handshake_to = LSQUIC_ECHO_HANDSHAKE_TIMEOUT_US;
  settings.es_idle_timeout = LSQUIC_ECHO_IDLE_TIMEOUT_SECONDS;

  memset(&engine_api, 0, sizeof(engine_api));
  engine_api.ea_settings = &settings;
  engine_api.ea_stream_if = &s_lsquic_echo_stream_if;
  engine_api.ea_stream_if_ctx = &app;
  engine_api.ea_get_ssl_ctx = lsquic_echo_get_ssl_context;
  engine_api.ea_alpn = app.mode == LSQUIC_EXAMPLE_ECHO ? LSQUIC_ECHO_ALPN : NULL;

  memset(&config, 0, sizeof(config));
  config.context = app.context;
  config.datagram_kind =
      peer_address.ss_family == AF_INET6 ? TURBO_DATAGRAM_UDP6 : TURBO_DATAGRAM_UDP4;
  config.bind_host =
      peer_address.ss_family == AF_INET6 ? LSQUIC_ECHO_BIND_HOST_IPV6 : LSQUIC_ECHO_BIND_HOST_IPV4;
  config.engine_flags = app.mode == LSQUIC_EXAMPLE_HTTP3_HANDSHAKE ? LSENG_HTTP : 0;
  config.engine_api = &engine_api;
  config.peer_ctx = &app;

  rc = turbo_lsquic_create(&config, &app.adapter);
  if (rc != 0) {
    fprintf(stderr, "Failed to create the CoroNet LSQUIC adapter: %d\n", rc);
    SSL_CTX_free(app.ssl_context);
    coro_context_destroy(app.context);
    return EXIT_FAILURE;
  }

  rc = turbo_datagram_get_local_addr(turbo_lsquic_datagram(app.adapter), &local_address);
  if (rc != 0) {
    fprintf(stderr, "Failed to query the local UDP address: %d\n", rc);
    turbo_lsquic_destroy(app.adapter);
    SSL_CTX_free(app.ssl_context);
    coro_context_destroy(app.context);
    return EXIT_FAILURE;
  }

  app.connection = lsquic_engine_connect(
      turbo_lsquic_engine(app.adapter), N_LSQVER, (const struct sockaddr *)&local_address,
      (const struct sockaddr *)&peer_address, &app, NULL, server_name, 0, NULL, 0, NULL, 0);
  if (!app.connection) {
    fprintf(stderr, "Failed to create the LSQUIC connection\n");
    turbo_lsquic_destroy(app.adapter);
    SSL_CTX_free(app.ssl_context);
    coro_context_destroy(app.context);
    return EXIT_FAILURE;
  }

  printf("Connecting to %s:%u over LSQUIC and CoroNet UDP\n", server_ip, (unsigned)port);
  turbo_lsquic_process(app.adapter);
  (void)coro_context_run(app.context, TURBO_RUN_DEFAULT);

  turbo_lsquic_destroy(app.adapter);
  SSL_CTX_free(app.ssl_context);
  coro_context_destroy(app.context);
  return app.result;
}
