#include "CoroNet.h"
#include "turbo_stream.h"
#include "tinytest.h"
#include <stdio.h>

static int s_connected = -1;
static int s_closed = 0;

static void on_connect(turbo_stream_t *s, int status, void *arg) {
    (void)arg;
    s_connected = status;
}

static void on_close(turbo_stream_t *s) {
    (void)s;
    s_closed = 1;
}

static turbo_stream_t *s_accepted_client = NULL;
static int s_accepted_count = 0;

static void on_accept_local(void *server, void *client, void *peer) {
    (void)server; (void)peer;
    s_accepted_client = (turbo_stream_t *)client;
    s_accepted_count++;
}

spec("Stream") {
    it("should create and destroy stream") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_stream_t *stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(stream != NULL);

        turbo_stream_destroy(stream);
        coro_context_destroy(ctx);
    }

    it("should fail to connect to missing server and close") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        turbo_stream_t *stream = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(stream != NULL);

        s_connected = -1;
        s_closed = 0;

        int r = turbo_stream_connect(stream, "127.0.0.1", 49200, on_connect, on_close);
        check_int_eq(r, 0); // Submission success

        // Connection might hang for 21 seconds due to Windows Firewall silent dropping on closed ports.
        // We explicitly close it to trigger CancelIo, ensuring an immediate ERROR_OPERATION_ABORTED completion.
        turbo_stream_close(stream);

        int limit = 100;
        while (s_connected == -1 && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check(s_connected != 0); // Should fail to connect (connection refused)
        check(s_connected != -1); // Callback must have fired

        turbo_stream_destroy(stream);
        coro_context_run(ctx, TURBO_RUN_DEFAULT); // Let close callback run
        check_int_eq(s_closed, 1);
        coro_context_destroy(ctx);
    }
    it("should listen and accept connections") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(49201); // Use a fixed port for test

        s_accepted_client = NULL;
        s_accepted_count = 0;

        turbo_stream_listener_t *listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&addr, 128, on_accept_local);
        check(listener != NULL);

        /* Connect a client */
        turbo_stream_t *client = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
        check(client != NULL);

        s_connected = -1;
        int r = turbo_stream_connect_addr(client, (struct sockaddr *)&addr, on_connect, on_close);
        check_int_eq(r, 0);

        /* Run loop until connected and accepted */
        int limit = 100;
        while ((s_connected == -1 || s_accepted_count == 0) && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_connected, 0);
        check_int_eq(s_accepted_count, 1);
        check(s_accepted_client != NULL);

        /* Cleanup */
        turbo_stream_destroy(client);
        if (s_accepted_client) turbo_stream_destroy(s_accepted_client);
        turbo_stream_listener_close(listener);

        coro_context_run(ctx, TURBO_RUN_DEFAULT);
        coro_context_destroy(ctx);
    }
}
