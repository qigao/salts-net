#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET email_test_socket_t;
  #define EMAIL_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int email_test_socket_t;
  #define EMAIL_TEST_INVALID_SOCKET (-1)
#endif

#include "email/email_imap.h"
#include "email/email_pop3.h"
#include "email/email_smtp.h"
#include "../src/email_cnet_transport.h"
#include "tinytest.h"

#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  EMAIL_TEST_TIMEOUT_MS = 2000,
  EMAIL_TEST_CLIENT_TIMEOUT_MS = 1000,
  EMAIL_TEST_LINE_BYTES = 2048,
  EMAIL_TEST_DATA_BYTES = 8192
};

typedef enum email_test_behavior_e {
  EMAIL_TEST_SMTP_PLAINTEXT = 0,
  EMAIL_TEST_SMTP_STARTTLS_FAILURE,
  EMAIL_TEST_POP3_PLAINTEXT,
  EMAIL_TEST_IMAP_PLAINTEXT,
  EMAIL_TEST_SMTP_STALL
} email_test_behavior_t;

typedef struct email_test_server_s {
  email_test_socket_t listener;
  email_test_behavior_t behavior;
  int status;
  int saw_tls_client_hello;
  atomic_int accepted;
  atomic_int release_peer;
  char message_data[EMAIL_TEST_DATA_BYTES];
} email_test_server_t;

typedef struct email_test_smtp_owner_s {
  smtp_client_t *client;
  int connect_status;
} email_test_smtp_owner_t;

static void email_test_close_socket(email_test_socket_t socket_value) {
  if (socket_value == EMAIL_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int email_test_set_timeout(email_test_socket_t socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = EMAIL_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms));
#else
  const struct timeval timeout = {EMAIL_TEST_TIMEOUT_MS / 1000,
                                  (EMAIL_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout));
#endif
}

static int email_test_send_all(email_test_socket_t peer, const char *data, size_t size) {
  size_t sent = 0u;
  while (sent < size) {
    const int result = send(peer, data + sent, (int)(size - sent), 0);
    if (result <= 0) return -1;
    sent += (size_t)result;
  }
  return 0;
}

static int email_test_send_text(email_test_socket_t peer, const char *text) {
  return email_test_send_all(peer, text, strlen(text));
}

static int email_test_read_line(email_test_socket_t peer, char *line, size_t capacity) {
  size_t used = 0u;
  while (used + 1u < capacity) {
    const int result = recv(peer, line + used, 1, 0);
    if (result != 1) return -1;
    used++;
    if (line[used - 1u] == '\n') {
      line[used] = '\0';
      return 0;
    }
  }
  return -1;
}

static int email_test_expect_line(email_test_socket_t peer, const char *expected) {
  char line[EMAIL_TEST_LINE_BYTES];
  if (email_test_read_line(peer, line, sizeof(line)) != 0) return -1;
  return strcmp(line, expected) == 0 ? 0 : -1;
}

static int email_test_read_data(email_test_socket_t peer, char *data, size_t capacity) {
  static const char terminator[] = "\r\n.\r\n";
  size_t used = 0u;
  while (used + 1u < capacity) {
    const int result = recv(peer, data + used, 1, 0);
    if (result != 1) return -1;
    used++;
    if (used >= sizeof(terminator) - 1u &&
        memcmp(data + used - (sizeof(terminator) - 1u), terminator, sizeof(terminator) - 1u) == 0) {
      data[used] = '\0';
      return 0;
    }
  }
  return -1;
}

static int email_test_run_plaintext(email_test_server_t *server, email_test_socket_t peer) {
  if (email_test_send_text(peer, "220 loopback ESMTP\r\n") != 0 ||
      email_test_expect_line(peer, "HELO [127.0.0.1]\r\n") != 0 ||
      email_test_send_text(peer, "250 loopback\r\n") != 0 ||
      email_test_expect_line(peer, "MAIL FROM:<sender@example.test>\r\n") != 0 ||
      email_test_send_text(peer, "250 sender accepted\r\n") != 0 ||
      email_test_expect_line(peer, "RCPT TO:<receiver@example.test>\r\n") != 0 ||
      email_test_send_text(peer, "250 recipient accepted\r\n") != 0 ||
      email_test_expect_line(peer, "DATA\r\n") != 0 ||
      email_test_send_text(peer, "354 send data\r\n") != 0 ||
      email_test_read_data(peer, server->message_data, sizeof(server->message_data)) != 0 ||
      email_test_send_text(peer, "250 queued\r\n") != 0 ||
      email_test_expect_line(peer, "QUIT\r\n") != 0 ||
      email_test_send_text(peer, "221 bye\r\n") != 0) {
    return -1;
  }
  return 0;
}

static int email_test_run_starttls_failure(email_test_server_t *server, email_test_socket_t peer) {
  unsigned char record_header[5];
  int received;
  if (email_test_send_text(peer, "220 loopback ESMTP\r\n") != 0 ||
      email_test_expect_line(peer, "EHLO [127.0.0.1]\r\n") != 0 ||
      email_test_send_text(peer, "250-STARTTLS\r\n250 OK\r\n") != 0 ||
      email_test_expect_line(peer, "STARTTLS\r\n") != 0 ||
      email_test_send_text(peer, "220 begin TLS\r\n") != 0) {
    return -1;
  }
  received = recv(peer, (char *)record_header, (int)sizeof(record_header), MSG_WAITALL);
  server->saw_tls_client_hello = received == (int)sizeof(record_header) && record_header[0] == 0x16;
  return server->saw_tls_client_hello ? 0 : -1;
}

static int email_test_run_pop3(email_test_socket_t peer) {
  if (email_test_send_text(peer, "+OK loopback ready\r\n") != 0 ||
      email_test_expect_line(peer, "USER user\r\n") != 0 ||
      email_test_send_text(peer, "+OK user accepted\r\n") != 0 ||
      email_test_expect_line(peer, "PASS password\r\n") != 0 ||
      email_test_send_text(peer, "+OK mailbox ready\r\n") != 0 ||
      email_test_expect_line(peer, "STAT\r\n") != 0 ||
      email_test_send_text(peer, "+OK 2 42\r\n") != 0 ||
      email_test_expect_line(peer, "QUIT\r\n") != 0 ||
      email_test_send_text(peer, "+OK bye\r\n") != 0) {
    return -1;
  }
  return 0;
}

static int email_test_run_imap(email_test_socket_t peer) {
  if (email_test_send_text(peer, "* OK loopback ready\r\n") != 0 ||
      email_test_expect_line(peer, "A0001 LOGIN user password\r\n") != 0 ||
      email_test_send_text(peer, "A0001 OK logged in\r\n") != 0 ||
      email_test_expect_line(peer, "A0002 LOGOUT\r\n") != 0 ||
      email_test_send_text(peer, "* BYE closing\r\nA0002 OK logout complete\r\n") != 0) {
    return -1;
  }
  return 0;
}

static int email_test_run_stall(email_test_server_t *server) {
  while (atomic_load_explicit(&server->release_peer, memory_order_acquire) == 0) {
    cmeta_sleep_ms(1u);
  }
  return 0;
}

static void email_test_server_run(void *user) {
  email_test_server_t *server = (email_test_server_t *)user;
  email_test_socket_t peer = accept(server->listener, NULL, NULL);
  if (peer == EMAIL_TEST_INVALID_SOCKET) {
    server->status = -1;
    return;
  }
  atomic_store_explicit(&server->accepted, 1, memory_order_release);
  if (email_test_set_timeout(peer) != 0) {
    server->status = -1;
    email_test_close_socket(peer);
    return;
  }
  switch (server->behavior) {
  case EMAIL_TEST_SMTP_PLAINTEXT:
    server->status = email_test_run_plaintext(server, peer);
    break;
  case EMAIL_TEST_SMTP_STARTTLS_FAILURE:
    server->status = email_test_run_starttls_failure(server, peer);
    break;
  case EMAIL_TEST_POP3_PLAINTEXT:
    server->status = email_test_run_pop3(peer);
    break;
  case EMAIL_TEST_IMAP_PLAINTEXT:
    server->status = email_test_run_imap(peer);
    break;
  case EMAIL_TEST_SMTP_STALL:
    server->status = email_test_run_stall(server);
    break;
  default:
    server->status = -1;
    break;
  }
  email_test_close_socket(peer);
}

static int email_test_server_open(email_test_server_t *server, uint16_t *port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  WSADATA data;
  int address_size = (int)sizeof(address);
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif

  memset(server, 0, sizeof(*server));
  atomic_init(&server->accepted, 0);
  atomic_init(&server->release_peer, 0);
  server->listener = EMAIL_TEST_INVALID_SOCKET;
  server->status = -1;
  server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server->listener == EMAIL_TEST_INVALID_SOCKET) return -1;
  if (email_test_set_timeout(server->listener) != 0) return -1;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server->listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      listen(server->listener, 1) != 0 ||
      getsockname(server->listener, (struct sockaddr *)&address, &address_size) != 0) {
    return -1;
  }
  *port = ntohs(address.sin_port);
  return 0;
}

static void email_test_smtp_connect(void *user) {
  email_test_smtp_owner_t *owner = (email_test_smtp_owner_t *)user;
  owner->connect_status = smtp_connect(owner->client);
}

static void email_test_server_close(email_test_server_t *server) {
  email_test_close_socket(server->listener);
  server->listener = EMAIL_TEST_INVALID_SOCKET;
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

spec("email CNet transport") {
  group("CNet 2.3 managed connection episodes") {
    static email_cnet_transport_t transport;
    static email_test_server_t server;
    static cmeta_thread_t thread;
    static uint16_t port;
    before_each() {
      memset(&transport, 0, sizeof(transport));
      thread = NULL;
      check_equal(email_test_server_open(&server, &port), 0);
      server.behavior = EMAIL_TEST_SMTP_STALL;
      check_equal(email_cnet_transport_init(&transport, EMAIL_TEST_CLIENT_TIMEOUT_MS), SALTS_OK);
    }
    after_each() {
      check_warn(email_cnet_transport_destroy(&transport) == SALTS_OK);
      atomic_store_explicit(&server.release_peer, 1, memory_order_release);
      if (thread) {
        check_warn(cmeta_thread_join(&thread) == SALTS_OK);
        cmeta_thread_destroy(&thread);
      }
      email_test_server_close(&server);
    }

    it("retires invalid connect admission before a later real connection") {
      cnet_manager_snapshot snapshot;
      /* Brackets require a numeric IPv6 address, not a DNS name. */
      check_equal(email_cnet_transport_connect(&transport, "[localhost]", port, 0), SALTS_EINVAL);
      check_null(transport.manager.impl);
      check_equal(atomic_load(&transport.active), 0);
      check_equal(cmeta_thread_create(&thread, email_test_server_run, &server), SALTS_OK);
      check_equal(email_cnet_transport_connect(&transport, "127.0.0.1", port, 0), SALTS_OK);
      check_equal(cnet_manager_get_snapshot(&transport.manager, &snapshot), SALTS_OK);
      check_equal(snapshot.record_capacity, 1u);
      check_equal(snapshot.bound, 1u);
      check_equal(snapshot.reserved + snapshot.retired, 0u);
      check_equal(email_cnet_transport_close(&transport), SALTS_OK);
      check_null(transport.manager.impl);
      check_equal(transport.managed.manager, (uintptr_t)0u);
      check_equal(atomic_load(&transport.active), 0);
    }

    it("reuses the one connection slot only after terminal and manager recycle") {
      cnet_connection previous = {0};
      for (size_t episode = 0u; episode < 3u; ++episode) {
        atomic_store(&server.release_peer, 0);
        check_equal(cmeta_thread_create(&thread, email_test_server_run, &server), SALTS_OK);
        check_equal(email_cnet_transport_connect(&transport, "127.0.0.1", port, 0), SALTS_OK);
        check(transport.connection.slot != previous.slot ||
              transport.connection.generation != previous.generation);
        previous = transport.connection;
        check_equal(email_cnet_transport_connect(&transport, "127.0.0.1", port, 0), SALTS_EALREADY);
        check_equal(email_cnet_transport_close(&transport), SALTS_OK);
        check_null(transport.manager.impl);
        check_equal(atomic_load(&transport.active), 0);
        atomic_store_explicit(&server.release_peer, 1, memory_order_release);
        check_equal(cmeta_thread_join(&thread), SALTS_OK);
        cmeta_thread_destroy(&thread);
        thread = NULL;
        check_equal(server.status, 0);
      }
    }
  }

  it("runs an SMTP transaction without a coroutine context") {
    static const char message[] = "Subject: loopback\r\n\r\n.first\r\n..second\r\n";
    static const char *recipients[] = {"receiver@example.test"};
    email_test_server_t server;
    cmeta_thread_t thread = NULL;
    smtp_client_t *client = NULL;
    uint16_t port = 0u;
    int connect_status = -1;
    int send_status = -1;

    check_equal(email_test_server_open(&server, &port), 0);
    server.behavior = EMAIL_TEST_SMTP_PLAINTEXT;
    check_equal(cmeta_thread_create(&thread, email_test_server_run, &server), 0);

    const smtp_config_t config = {
        .host = "127.0.0.1", .port = (int)port, .timeout_ms = EMAIL_TEST_CLIENT_TIMEOUT_MS};
    client = smtp_client_create(&config);
    check_not_null(client);
    if (client != NULL) connect_status = smtp_connect(client);
    if (connect_status == 0) {
      send_status = smtp_send_raw(client, "sender@example.test", recipients, 1, message,
                                  sizeof(message) - 1u);
      smtp_disconnect(client);
    }
    smtp_client_free(client);

    email_test_close_socket(server.listener);
    server.listener = EMAIL_TEST_INVALID_SOCKET;
    check_equal(cmeta_thread_join(&thread), 0);
    cmeta_thread_destroy(&thread);
    email_test_server_close(&server);

    check_equal(connect_status, 0);
    check_equal(send_status, 0);
    check_equal(server.status, 0);
    check_not_null(strstr(server.message_data, "\r\n..first\r\n...second\r\n.\r\n"));
  }

  it("upgrades the existing SMTP socket and fails closed on invalid TLS") {
    email_test_server_t server;
    cmeta_thread_t thread = NULL;
    smtp_client_t *client = NULL;
    uint16_t port = 0u;
    int connect_status = 0;

    check_equal(email_test_server_open(&server, &port), 0);
    server.behavior = EMAIL_TEST_SMTP_STARTTLS_FAILURE;
    check_equal(cmeta_thread_create(&thread, email_test_server_run, &server), 0);

    const smtp_config_t config = {.host = "127.0.0.1",
                                  .port = (int)port,
                                  .use_starttls = 1,
                                  .timeout_ms = EMAIL_TEST_CLIENT_TIMEOUT_MS};
    client = smtp_client_create(&config);
    check_not_null(client);
    if (client != NULL) connect_status = smtp_connect(client);
    smtp_client_free(client);

    email_test_close_socket(server.listener);
    server.listener = EMAIL_TEST_INVALID_SOCKET;
    check_equal(cmeta_thread_join(&thread), 0);
    cmeta_thread_destroy(&thread);
    email_test_server_close(&server);

    check_not_equal(connect_status, 0);
    check_equal(server.status, 0);
    check(server.saw_tls_client_hello);
  }

  it("runs POP3 authentication and STAT without a coroutine context") {
    email_test_server_t server;
    cmeta_thread_t thread = NULL;
    pop3_client_t *client = NULL;
    uint16_t port = 0u;
    int connect_status = -1;
    int total_size = 0;
    int message_count = -1;

    check_equal(email_test_server_open(&server, &port), 0);
    server.behavior = EMAIL_TEST_POP3_PLAINTEXT;
    check_equal(cmeta_thread_create(&thread, email_test_server_run, &server), 0);

    const pop3_config_t config = {.host = "127.0.0.1",
                                  .port = (int)port,
                                  .username = "user",
                                  .password = "password",
                                  .timeout_ms = EMAIL_TEST_CLIENT_TIMEOUT_MS};
    client = pop3_client_create(&config);
    check_not_null(client);
    if (client != NULL) connect_status = pop3_connect(client);
    if (connect_status == 0) {
      message_count = pop3_stat(client, &total_size);
      pop3_disconnect(client);
    }
    pop3_client_free(client);

    email_test_close_socket(server.listener);
    server.listener = EMAIL_TEST_INVALID_SOCKET;
    check_equal(cmeta_thread_join(&thread), 0);
    cmeta_thread_destroy(&thread);
    email_test_server_close(&server);

    check_equal(connect_status, 0);
    check_equal(message_count, 2);
    check_equal(total_size, 42);
    check_equal(server.status, 0);
  }

  it("runs IMAP login and logout without a coroutine context") {
    email_test_server_t server;
    cmeta_thread_t thread = NULL;
    imap_client_t *client = NULL;
    uint16_t port = 0u;
    int connect_status = -1;

    check_equal(email_test_server_open(&server, &port), 0);
    server.behavior = EMAIL_TEST_IMAP_PLAINTEXT;
    check_equal(cmeta_thread_create(&thread, email_test_server_run, &server), 0);

    const imap_config_t config = {.host = "127.0.0.1",
                                  .port = (int)port,
                                  .username = "user",
                                  .password = "password",
                                  .timeout_ms = EMAIL_TEST_CLIENT_TIMEOUT_MS};
    client = imap_client_create(&config);
    check_not_null(client);
    if (client != NULL) connect_status = imap_connect(client);
    if (connect_status == 0) imap_disconnect(client);
    imap_client_free(client);

    email_test_close_socket(server.listener);
    server.listener = EMAIL_TEST_INVALID_SOCKET;
    check_equal(cmeta_thread_join(&thread), 0);
    cmeta_thread_destroy(&thread);
    email_test_server_close(&server);

    check_equal(connect_status, 0);
    check_equal(server.status, 0);
  }

  it("interrupts a blocked SMTP receive through CNet wake") {
    email_test_server_t server;
    email_test_smtp_owner_t owner;
    cmeta_thread_t server_thread = NULL;
    cmeta_thread_t owner_thread = NULL;
    smtp_client_t *client = NULL;
    uint16_t port = 0u;
    int interrupt_status = SALTS_EIO;

    check_equal(email_test_server_open(&server, &port), 0);
    server.behavior = EMAIL_TEST_SMTP_STALL;
    check_equal(cmeta_thread_create(&server_thread, email_test_server_run, &server), 0);

    const smtp_config_t config = {
        .host = "127.0.0.1", .port = (int)port, .timeout_ms = EMAIL_TEST_TIMEOUT_MS};
    client = smtp_client_create(&config);
    check_not_null(client);
    owner = (email_test_smtp_owner_t){.client = client, .connect_status = 0};
    check_equal(cmeta_thread_create(&owner_thread, email_test_smtp_connect, &owner), 0);

    while (atomic_load_explicit(&server.accepted, memory_order_acquire) == 0) {
      cmeta_sleep_ms(1u);
    }
    interrupt_status = smtp_interrupt(client, SALTS_ESHUTDOWN);
    check_equal(cmeta_thread_join(&owner_thread), 0);
    cmeta_thread_destroy(&owner_thread);
    atomic_store_explicit(&server.release_peer, 1, memory_order_release);
    check_equal(cmeta_thread_join(&server_thread), 0);
    cmeta_thread_destroy(&server_thread);
    smtp_client_free(client);

    email_test_close_socket(server.listener);
    server.listener = EMAIL_TEST_INVALID_SOCKET;
    email_test_server_close(&server);

    check_equal(interrupt_status, SALTS_OK);
    check_not_equal(owner.connect_status, 0);
    check_equal(server.status, 0);
  }
}
