#include "tinytest.h"
#include "http_client.h"
#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
typedef SOCKET native_sock_t;
typedef HANDLE native_thread_t;
typedef unsigned(__stdcall *native_thread_entry_t)(void *);
#define THREAD_RET unsigned __stdcall
#define THREAD_RETURN(value) return (value)
#define THREAD_CALL __stdcall
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
typedef int native_sock_t;
typedef pthread_t native_thread_t;
typedef void *(*native_thread_entry_t)(void *);
#define THREAD_RET void *
#define THREAD_RETURN(value) return (value)
#define THREAD_CALL
#endif

typedef struct {
  int auth_required;
  int ready;
  int failed;
  int port;
  int saw_http_request;
  int fail_step;
  int fail_code;
} mock_proxy_state_t;

static void ensure_native_sockets_ready(void) {
#ifdef _WIN32
  static int initialized = 0;
  static WSADATA wsa_data;

  if (!initialized) {
    check_int_eq(WSAStartup(MAKEWORD(2, 2), &wsa_data), 0);
    initialized = 1;
  }
#endif
}

static int recv_exact_sock(native_sock_t sock, unsigned char *buf, size_t want) {
  size_t filled = 0;

  while (filled < want) {
    int n = recv(sock, (char *)buf + filled, (int)(want - filled), 0);
    if (n <= 0)
      return -1;
    filled += (size_t)n;
  }

  return 0;
}

static int send_all_sock(native_sock_t sock, const unsigned char *buf, size_t len) {
  size_t sent = 0;

  while (sent < len) {
    int n = send(sock, (const char *)buf + sent, (int)(len - sent), 0);
    if (n <= 0)
      return -1;
    sent += (size_t)n;
  }

  return 0;
}

static int recv_http_headers(native_sock_t sock, char *buf, size_t cap) {
  size_t len = 0;

  while (len + 1 < cap) {
    int n = recv(sock, buf + len, (int)(cap - len - 1), 0);
    if (n <= 0)
      return -1;
    len += (size_t)n;
    buf[len] = '\0';
    if (strstr(buf, "\r\n\r\n") != NULL)
      return 0;
  }

  return -1;
}

static int native_thread_create(native_thread_t *thread, native_thread_entry_t entry, void *arg) {
#ifdef _WIN32
  uintptr_t h = _beginthreadex(NULL, 0, entry, arg, 0, NULL);
  if (h == 0)
    return -1;
  *thread = (HANDLE)h;
  return 0;
#else
  return pthread_create(thread, NULL, entry, arg);
#endif
}

static void native_thread_join(native_thread_t *thread) {
#ifdef _WIN32
  WaitForSingleObject(*thread, INFINITE);
  CloseHandle(*thread);
  *thread = NULL;
#else
  pthread_join(*thread, NULL);
#endif
}

static void native_sleep_ms(unsigned ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

static THREAD_RET THREAD_CALL mock_http_socks5_proxy_thread(void *arg) {
  mock_proxy_state_t *state = (mock_proxy_state_t *)arg;
  native_sock_t listener;
  native_sock_t client;
  struct sockaddr_in addr;
  struct sockaddr_in bound_addr;
#ifdef _WIN32
  int bound_len;
#else
  socklen_t bound_len;
#endif
  unsigned char head[4];
  unsigned char auth_req[512];
  unsigned char connect_req[512];
  unsigned char resp[32];
  char http_req[2048];
  size_t host_len;
  int rc = 0;
  static const char http_resp[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 5\r\n"
      "Connection: close\r\n"
      "\r\n"
      "HELLO";

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == (native_sock_t)-1) {
    state->fail_step = 1;
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    state->failed = 1;
    THREAD_RETURN(0);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)state->port);

  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    state->fail_step = 2;
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  bound_len = (int)sizeof(bound_addr);
  if (getsockname(listener, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
    state->fail_step = 3;
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  state->port = ntohs(bound_addr.sin_port);
  state->ready = 1;
  client = accept(listener, NULL, NULL);
  CLOSESOCK(listener);
  if (client == (native_sock_t)-1) {
    state->fail_step = 4;
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    state->failed = 1;
    THREAD_RETURN(0);
  }

  rc = recv_exact_sock(client, head, 2);
  if (rc != 0 || head[0] != 0x05)
    goto fail;
  rc = recv_exact_sock(client, head + 2, head[1]);
  if (rc != 0)
    goto fail;

  resp[0] = 0x05;
  resp[1] = state->auth_required ? 0x02 : 0x00;
  if (send_all_sock(client, resp, 2) != 0)
    goto fail;

  if (state->auth_required) {
    rc = recv_exact_sock(client, auth_req, 2);
    if (rc != 0 || auth_req[0] != 0x01)
      goto fail;
    rc = recv_exact_sock(client, auth_req + 2, auth_req[1] + 1);
    if (rc != 0)
      goto fail;
    rc = recv_exact_sock(client, auth_req + 3 + auth_req[1], auth_req[2 + auth_req[1]]);
    if (rc != 0)
      goto fail;
    resp[0] = 0x01;
    resp[1] = 0x00;
    if (send_all_sock(client, resp, 2) != 0)
      goto fail;
  }

  rc = recv_exact_sock(client, connect_req, 4);
  if (rc != 0 || connect_req[0] != 0x05 || connect_req[1] != 0x01)
    goto fail;

  switch (connect_req[3]) {
  case 0x01:
    rc = recv_exact_sock(client, connect_req + 4, 6);
    break;
  case 0x03:
    rc = recv_exact_sock(client, connect_req + 4, 1);
    if (rc != 0)
      goto fail;
    host_len = connect_req[4];
    rc = recv_exact_sock(client, connect_req + 5, host_len + 2);
    break;
  case 0x04:
    rc = recv_exact_sock(client, connect_req + 4, 18);
    break;
  default:
    goto fail;
  }
  if (rc != 0)
    goto fail;

  resp[0] = 0x05;
  resp[1] = 0x00;
  resp[2] = 0x00;
  resp[3] = 0x01;
  resp[4] = 127;
  resp[5] = 0;
  resp[6] = 0;
  resp[7] = 1;
  resp[8] = 0x1F;
  resp[9] = 0x90;
  if (send_all_sock(client, resp, 10) != 0)
    goto fail;

  if (recv_http_headers(client, http_req, sizeof(http_req)) != 0)
    goto fail;
  if (strstr(http_req, "GET /proxy-test HTTP/1.1\r\n") == NULL)
    goto fail;
  if (strstr(http_req, "Host: example.com:8088\r\n") == NULL)
    goto fail;

  state->saw_http_request = 1;
  if (send_all_sock(client, (const unsigned char *)http_resp, strlen(http_resp)) != 0)
    goto fail;

  CLOSESOCK(client);
  THREAD_RETURN(0);

fail:
  state->fail_step = state->fail_step == 0 ? 5 : state->fail_step;
  state->failed = 1;
  CLOSESOCK(client);
  THREAD_RETURN(0);
}

static THREAD_RET THREAD_CALL mock_http_socks5_proxy_drop_thread(void *arg) {
  mock_proxy_state_t *state = (mock_proxy_state_t *)arg;
  native_sock_t listener;
  native_sock_t client;
  struct sockaddr_in addr;
  struct sockaddr_in bound_addr;
#ifdef _WIN32
  int bound_len;
#else
  socklen_t bound_len;
#endif

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == (native_sock_t)-1) {
    state->fail_step = 1;
    state->failed = 1;
    THREAD_RETURN(0);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)state->port);

  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    state->fail_step = 2;
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  bound_len = (int)sizeof(bound_addr);
  if (getsockname(listener, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
    state->fail_step = 3;
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  state->port = ntohs(bound_addr.sin_port);
  state->ready = 1;

  client = accept(listener, NULL, NULL);
  CLOSESOCK(listener);
  if (client == (native_sock_t)-1) {
    state->fail_step = 4;
    state->failed = 1;
    THREAD_RETURN(0);
  }

  CLOSESOCK(client);
  THREAD_RETURN(0);
}

static void run_proxy_case(int auth_required) {
  mock_proxy_state_t state = {0};
  native_thread_t thread;
  http_client_t *client;
  http_response_t *resp;
  int rc;

  ensure_native_sockets_ready();
  state.auth_required = auth_required;
  state.port = 0;
  rc = native_thread_create(&thread, mock_http_socks5_proxy_thread, &state);
  check_int_eq(rc, 0);

  while (!state.ready && !state.failed) {
    native_sleep_ms(10);
  }
  if (state.failed) {
    printf("mock proxy bootstrap failed at step %d code %d\n", state.fail_step, state.fail_code);
  }
  check_int_eq(state.failed, 0);

  client = http_client_create(NULL);
  check_not_null(client);
  http_client_set_connect_timeout(client, 2000);
  http_client_set_timeout(client, 2000);
  if (auth_required) {
    http_client_set_proxy(client, "127.0.0.1", (uint16_t)state.port, "user", "pass");
  } else {
    http_client_set_proxy(client, "127.0.0.1", (uint16_t)state.port, NULL, NULL);
  }

  resp = http_get(client, "http://example.com:8088/proxy-test");
  check_not_null(resp);
  check_int_eq(resp->error_code, HTTP_ERROR_NONE);
  check_int_eq(resp->status_code, 200);
  check_size_eq(resp->body_len, 5);
  check_mem_eq(resp->body, "HELLO", 5);

  http_response_free(resp);
  http_client_destroy(client);
  native_thread_join(&thread);
  check_int_eq(state.failed, 0);
  check_int_eq(state.saw_http_request, 1);
}

static void run_proxy_drop_case(void) {
  mock_proxy_state_t state = {0};
  native_thread_t thread;
  http_client_t *client;
  http_response_t *resp;
  int rc;

  ensure_native_sockets_ready();
  state.port = 0;
  rc = native_thread_create(&thread, mock_http_socks5_proxy_drop_thread, &state);
  check_int_eq(rc, 0);

  while (!state.ready && !state.failed) {
    native_sleep_ms(10);
  }
  check_int_eq(state.failed, 0);

  client = http_client_create(NULL);
  check_not_null(client);
  http_client_set_connect_timeout(client, 2000);
  http_client_set_timeout(client, 2000);
  http_client_set_proxy(client, "127.0.0.1", (uint16_t)state.port, NULL, NULL);

  resp = http_get(client, "http://example.com:8088/proxy-test");
  check_not_null(resp);
  check_int_ne(resp->error_code, HTTP_ERROR_NONE);
  check_int_eq(resp->status_code, 0);

  http_response_free(resp);
  http_client_destroy(client);
  native_thread_join(&thread);
}

spec("http proxy") {
  describe("SOCKS5 proxy") {
    it("should fetch through SOCKS5 proxy without auth") { run_proxy_case(0); }

    it("should fetch through SOCKS5 proxy with username password auth") { run_proxy_case(1); }

    it("should fail when proxy closes during handshake") { run_proxy_drop_case(); }
  }
}
