#include "CoroNet.h"
#include "turbo_stream.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

static int s_connected = -1;
static int s_closed = 0;
static turbo_stream_t *s_accepted_client = NULL;
static int s_accepted_count = 0;

static void on_connect(void *s, int status, void *arg) {
    (void)arg;
    (void)s;
    s_connected = status;
}

static void on_close(void *s) {
    (void)s;
    s_closed = 1;
}

static void on_accept_local(void *server, void *client, void *peer) {
    (void)server; (void)peer;
    s_accepted_client = (turbo_stream_t *)client;
    s_accepted_count++;
}

static int s_recv_count = 0;
static char s_recv_buf[1024];

static int on_recv(void *stream, const mem_slice_t *slice, void *peer) {
    (void)peer;
    (void)stream;
    if (slice && slice->length > 0) {
        if (s_recv_count + slice->length < sizeof(s_recv_buf)) {
            memcpy(s_recv_buf + s_recv_count, slice->data, slice->length);
            s_recv_count += (int)slice->length;
        }
    }
    return 0;
}

spec("Stream Pipe") {
    it("should listen and accept connections via named pipe") {
        coro_context_t *ctx = coro_context_create(NULL);
        check(ctx != NULL);

#ifdef _WIN32
        const char* pipe_name = "\\\\.\\pipe\\turbo_test_pipe";
#else
        const char* pipe_name = "/tmp/turbo_test_pipe.sock";
#endif

        s_accepted_client = NULL;
        s_accepted_count = 0;
        s_connected = -1;
        s_closed = 0;
        s_recv_count = 0;
        memset(s_recv_buf, 0, sizeof(s_recv_buf));

        turbo_stream_listener_t *listener = turbo_stream_listen_pipe(ctx, pipe_name, 128, on_accept_local);
        check(listener != NULL);
        
#ifdef _WIN32
        Sleep(100); // Give worker thread a moment to call ConnectNamedPipe
#else
        usleep(100000);
#endif

        turbo_stream_t *client = turbo_stream_create(ctx, TURBO_STREAM_PIPE);
        check(client != NULL);

        int r = turbo_stream_connect_pipe(client, pipe_name, on_connect, on_close);
        check_int_eq(r, 0);

        int limit = 1000;
        while ((s_connected == -1 || s_accepted_count == 0) && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_connected, 0);
        check_int_eq(s_accepted_count, 1);
        check(s_accepted_client != NULL);

        // Test receiving on the accepted client
        r = turbo_stream_recv_start(s_accepted_client, on_recv);
        check_int_eq(r, 0);

        const char* test_msg = "Hello Pipe!";
        size_t msg_len = strlen(test_msg);
        r = turbo_stream_send(client, test_msg, msg_len);
        check_int_eq(r, 0);

        limit = 1000;
        while (s_recv_count < (int)msg_len && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }

        check_int_eq(s_recv_count, (int)msg_len);
        check_int_eq(strncmp(s_recv_buf, test_msg, msg_len), 0);

        /* Cleanup */
        turbo_stream_destroy(client);
        if (s_accepted_client) turbo_stream_destroy(s_accepted_client);
        turbo_stream_listener_close(listener);

        limit = 1000;
        while ((s_closed == 0) && limit-- > 0) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check(s_closed > 0);
        coro_context_destroy(ctx);
    }
}
