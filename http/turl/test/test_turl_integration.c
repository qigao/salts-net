#include <tinytest.h>
#include <errno.h>
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_http.h"
#include "turl_common.h"
#include "collection/turl_collection.h"
#include "platform.h"

#ifdef _WIN32
#include <process.h>
#define CLOSESOCK closesocket
typedef SOCKET native_sock_t;
typedef HANDLE native_thread_t;
typedef unsigned(__stdcall *native_thread_entry_t)(void *);
#define THREAD_RET unsigned __stdcall
#define THREAD_RETURN(value) return (value)
#define THREAD_CALL __stdcall
#else
#include <pthread.h>
#define CLOSESOCK close
typedef int native_sock_t;
typedef pthread_t native_thread_t;
typedef void *(*native_thread_entry_t)(void *);
#define THREAD_RET void *
#define THREAD_RETURN(value) return (value)
#define THREAD_CALL
#endif

typedef struct {
  int ready;
  int failed;
  int port;
  int saw_expected_request;
  int fail_step;
  int fail_code;
} turl_mock_server_state_t;

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

static int native_thread_create(native_thread_t *thread, native_thread_entry_t entry, void *arg) {
#ifdef _WIN32
  uintptr_t handle = _beginthreadex(NULL, 0, entry, arg, 0, NULL);
  if (handle == 0) {
    return -1;
  }
  *thread = (HANDLE)handle;
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

static int recv_http_request(native_sock_t sock, char *buf, size_t cap) {
  size_t len = 0;
  size_t total_needed = 0;

  while (len + 1 < cap) {
    int n = recv(sock, buf + len, (int)(cap - len - 1), 0);
    char *headers_end;
    char *content_length;
    size_t header_len;
    size_t body_have;

    if (n <= 0) {
      return -1;
    }

    len += (size_t)n;
    buf[len] = '\0';

    headers_end = strstr(buf, "\r\n\r\n");
    if (headers_end == NULL) {
      continue;
    }

    header_len = (size_t)(headers_end - buf) + 4U;
    content_length = strstr(buf, "Content-Length:");
    if (content_length == NULL) {
      return -1;
    }

    total_needed = header_len + (size_t)strtoul(content_length + strlen("Content-Length:"), NULL, 10);
    body_have = len - header_len;
    if (body_have >= total_needed - header_len) {
      return 0;
    }
  }

  return -1;
}

static THREAD_RET THREAD_CALL turl_mock_http_server_thread(void *arg) {
  turl_mock_server_state_t *state = (turl_mock_server_state_t *)arg;
  native_sock_t listener;
  native_sock_t client;
  struct sockaddr_in addr;
  struct sockaddr_in bound_addr;
  char request[4096];
  static const char expected_body[] =
      "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"broker.control\",\"params\":{}}";
  static const char response[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 11\r\n"
      "Connection: close\r\n"
      "\r\n"
      "{\"ok\":true}";
#ifdef _WIN32
  int bound_len = (int)sizeof(bound_addr);
#else
  socklen_t bound_len = (socklen_t)sizeof(bound_addr);
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
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

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
    state->failed = 1;
    THREAD_RETURN(0);
  }

  if (recv_http_request(client, request, sizeof(request)) != 0) {
    state->fail_step = 5;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  if (strstr(request, "POST /rpc HTTP/1.1\r\n") == NULL ||
      strstr(request, "Content-Type: application/json\r\n") == NULL ||
      strstr(request, expected_body) == NULL) {
    state->fail_step = 6;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  state->saw_expected_request = 1;
  if (send(client, response, (int)strlen(response), 0) <= 0) {
    state->fail_step = 7;
    state->failed = 1;
  }

  CLOSESOCK(client);
  THREAD_RETURN(0);
}

spec("turl_integration") {
    it("should perform GET request to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/get";
        config.method_str = "GET";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform POST request to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/post";
        config.method_str = "POST";
        config.body = "{\"greeting\":\"hello\"}";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform request with headers to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/headers";
        config.method_str = "GET";
        char *headers[] = {"X-Test-Header: integration-test"};
        config.headers = headers;
        config.header_count = 1;
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform request with basic auth to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/basic-auth/user/pass";
        config.method_str = "GET";
        config.user_pass = "user:pass";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should perform request with JWT generation to httpbin") {
        turl_http_config_t config = {0};
        config.url = "http://httpbin.org/bearer";
        config.method_str = "GET";
        config.jwt_secret = "secret";
        config.jwt_claims = "{\"sub\":\"1234567890\",\"name\":\"John Doe\",\"admin\":true}";
        config.verbose = 1;
        check_int_eq(turl_execute_http_request(&config), 0);
    }

    it("should run collection integration test") {
        const char *collection_json =
            "{\"name\": \"httpbin_coll\", \"requests\": ["
            "  {\"name\": \"GET_REQ\", \"url\": \"http://httpbin.org/get\"},"
            "  {\"name\": \"POST_REQ\", \"url\": \"http://httpbin.org/post\", \"method\": \"POST\", \"body\": \"hello collection\"}"
            "]}";

        FILE *f = fopen("it_collection.json", "w");
        if (f) {
            fputs(collection_json, f);
            fclose(f);
        }

        turl_http_config_t global_cfg = {0};
        global_cfg.verbose = 1;
        int ret = turl_run_collection("it_collection.json", &global_cfg);
        remove("it_collection.json");
        check_int_eq(ret, 0);
    }

    it("should run collection post with literal rpc json body when context exists") {
        char collection_json[1024];
        json_value_t *ctx = turbo_json_create_object();
        turl_http_config_t global_cfg = {0};
        turl_mock_server_state_t state = {0};
        native_thread_t thread;
        int ret;

        ensure_native_sockets_ready();
        check_int_eq(native_thread_create(&thread, turl_mock_http_server_thread, &state), 0);
        while (!state.ready && !state.failed) {
            native_sleep_ms(10);
        }

        check_int_eq(state.failed, 0);
        check_not_null(ctx);
        turbo_json_object_set_string(ctx, "unused", "value");
        fmt(collection_json, sizeof(collection_json),
            "{{\"name\": \"local_rpc_literal\", \"requests\": ["
            "  {{\"name\": \"POST_RPC_LITERAL\","
            "   \"url\": \"http://127.0.0.1:{}/rpc\","
            "   \"method\": \"POST\","
            "   \"body\": \"{{\\\"jsonrpc\\\":\\\"2.0\\\",\\\"id\\\":\\\"1\\\",\\\"method\\\":\\\"broker.control\\\",\\\"params\\\":{{}}}}\","
            "   \"headers\": {{\"Content-Type\": \"application/json\"}}}}"
            "]}}",
            state.port);

        FILE *f = fopen("it_collection.json", "w");
        if (f) {
            fputs(collection_json, f);
            fclose(f);
        }

        global_cfg.verbose = 1;
        global_cfg.mustache_context = ctx;
        ret = turl_run_collection("it_collection.json", &global_cfg);

        remove("it_collection.json");
        turbo_free_json(&ctx);
        native_thread_join(&thread);
        check_int_eq(state.failed, 0);
        check_int_eq(state.saw_expected_request, 1);
        check_int_eq(ret, 0);
    }
}
