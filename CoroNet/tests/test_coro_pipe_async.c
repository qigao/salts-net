#include "CoroNet.h"
#include "turbo_coro.h"
#include "tinytest.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int g_pipe_async_client_rc = TURBO_EBUSY;
static int g_pipe_async_server_rc = TURBO_EBUSY;
static int g_pipe_async_handler_hits = 0;

static void pipe_async_handler(coro_socket_t *client, void *arg) {
  (void)client;
  (void)arg;
  g_pipe_async_handler_hits++;
}

typedef struct pipe_async_state_s {
  coro_context_t *ctx;
  const char *endpoint;
  coro_socket_t *server;
} pipe_async_state_t;

static void delayed_pipe_server(coro_t *co, void *arg) {
  pipe_async_state_t *state = (pipe_async_state_t *)arg;
  (void)co;

  coro_sleep(state->ctx, 100);
  state->server = coro_socket_create(state->ctx, CORO_SOCKET_PIPE);
  if (!state->server) {
    g_pipe_async_server_rc = TURBO_ENOMEM;
    return;
  }

  g_pipe_async_server_rc = coro_socket_listen_on(state->server, state->endpoint, 0,
                                                 pipe_async_handler, NULL);
}

static void pipe_async_client(coro_t *co, void *arg) {
  pipe_async_state_t *state = (pipe_async_state_t *)arg;
  coro_socket_t *client;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_PIPE);
  if (!client) {
    g_pipe_async_client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 1500);
  g_pipe_async_client_rc = coro_socket_connect_pipe(client, state->endpoint);
  coro_socket_destroy(client);
}

spec("Coro Pipe Async") {
#ifdef _WIN32
  it("should let a delayed pipe listener start while connect is pending") {
    coro_context_t *ctx = coro_context_create(NULL);
    pipe_async_state_t state;
    int limit = 4000;

    check(ctx != NULL);

    state.ctx = ctx;
    state.endpoint = "pipe://turbo_async_pipe_connect";
    state.server = NULL;

    g_pipe_async_client_rc = TURBO_EBUSY;
    g_pipe_async_server_rc = TURBO_EBUSY;
    g_pipe_async_handler_hits = 0;

    check_equal(coro_context_spawn(ctx, delayed_pipe_server, &state), 0);
    check_equal(coro_context_spawn(ctx, pipe_async_client, &state), 0);

    while ((g_pipe_async_client_rc == TURBO_EBUSY || g_pipe_async_server_rc == TURBO_EBUSY) &&
           limit-- > 0) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
#ifdef _WIN32
      Sleep(1);
#endif
    }

    check_equal(g_pipe_async_server_rc, 0);
    check_equal(g_pipe_async_client_rc, 0);

    limit = 1000;
    while (g_pipe_async_handler_hits == 0 && limit-- > 0) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
#ifdef _WIN32
      Sleep(1);
#endif
    }

    check_equal(g_pipe_async_handler_hits, 1);

    if (state.server) {
      coro_socket_destroy(state.server);
    }

    limit = 200;
    while (coro_context_alive(ctx) && limit-- > 0) {
      coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }

    coro_context_destroy(ctx);
  }
#endif
}
