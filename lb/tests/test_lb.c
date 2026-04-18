/**
 * @file test_lb.c
 * @brief Tests for coro_lb — lifecycle, L4 echo, L7 routing.
 */

#include "turbo_coro_lb.h"
#include "turbo_coro_bidi_pump.h"
#include <CoroNet.h> 
#include "turbo_coro.h"
#include "tinytest.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void spawn_test_coro(coro_context_t *ctx, coro_fn fn, void *arg) {
    check(coro_context_spawn(ctx, fn, arg) == 0);
}

static void run_until_flag(coro_context_t *ctx, int *flag) {
    uint64_t deadline = turbo_monotonic_ms() + 3000;
    while (!*flag && turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        turbo_sleep_ms(1);
    }
}

static void run_until_coro_count_at_most(coro_context_t *ctx, int max_count, uint64_t timeout_ms) {
    uint64_t deadline = turbo_monotonic_ms() + timeout_ms;
    while (coro_context_coro_count(ctx) > max_count && turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        turbo_sleep_ms(1);
    }
}

static void drain_after_stop(coro_context_t *ctx) {
    uint64_t deadline = turbo_monotonic_ms() + 200;
    while (turbo_monotonic_ms() < deadline) {
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        turbo_sleep_ms(1);
    }
}

static void drain_until_context_quiescent(coro_context_t *ctx, uint64_t timeout_ms, uint64_t stable_ms) {
    uint64_t deadline;
    uint64_t stable_since = 0;

    if (!ctx) return;

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (turbo_monotonic_ms() < deadline) {
        int alive_before = coro_context_alive(ctx);

        coro_context_run(ctx, TURBO_RUN_NOWAIT);

        if (!alive_before && !coro_context_alive(ctx)) {
            if (stable_since == 0) {
                stable_since = turbo_monotonic_ms();
            } else if (turbo_monotonic_ms() - stable_since >= stable_ms) {
                break;
            }
        } else {
            stable_since = 0;
        }

        turbo_sleep_ms(1);
    }
}

static void robust_context_destroy(coro_context_t *ctx) {
    int max_drain = 500;

    if (!ctx) return;

    while (max_drain-- > 0) {
        if (!coro_context_alive(ctx)) {
            break;
        }
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }

    drain_until_context_quiescent(ctx, 3000, 100);

    coro_context_destroy(ctx);
}

static coro_context_t *create_lb_test_context(void) {
    return coro_context_create(NULL);
}

static void destroy_lb_and_context(coro_lb_t *lb, coro_context_t *ctx) {
    uint64_t deadline;

    if (!ctx) return;

    if (lb) {
        coro_lb_stop(lb);
        deadline = turbo_monotonic_ms() + 3000;
        while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
            turbo_sleep_ms(1);
        }
        drain_until_context_quiescent(ctx, 3000, 100);
        coro_lb_destroy(lb);
        drain_until_context_quiescent(ctx, 3000, 100);
    }

    robust_context_destroy(ctx);
}

static void destroy_stopped_lb_and_context(coro_lb_t *lb, coro_context_t *ctx) {
    if (!ctx) return;

    if (lb) {
        drain_until_context_quiescent(ctx, 3000, 100);
        coro_lb_destroy(lb);
        drain_until_context_quiescent(ctx, 3000, 100);
    }

    robust_context_destroy(ctx);
}

/* ── L4 Helpers ───────────────────────────────────────────── */

typedef struct {
    coro_context_t *ctx;
    const char *backend_host;
    int backend_port;
    int sessions_served;
} worker_ctx_t;

static void echo_worker_coro(coro_t *co, void *arg) {
    (void)co;
    worker_ctx_t *wctx = (worker_ctx_t *)arg;

    coro_socket_t *c = coro_socket_create(wctx->ctx, CORO_SOCKET_TCP_V4);
    if (!c) return;
    if (coro_socket_connect(c, wctx->backend_host, wctx->backend_port) != 0) {
        coro_socket_destroy(c);
        return;
    }

    char *data = NULL;
    size_t len = 0;
    while (coro_socket_recv(c, &data, &len) == 0) {
        coro_socket_send(c, data, len);
        coro_socket_free_recv(data);
        data = NULL;
    }

    wctx->sessions_served++;
    coro_socket_destroy(c);
}

typedef struct {
    coro_context_t *ctx;
    const char *frontend_host;
    int frontend_port;
    const char *send_data;
    char recv_buf[256];
    size_t recv_len;
    int success;
} client_ctx_t;

static void test_client_coro(coro_t *co, void *arg) {
    (void)co;
    client_ctx_t *cctx = (client_ctx_t *)arg;

    /* Small delay to let worker register */
    coro_sleep(cctx->ctx, 50);

    coro_socket_t *c = coro_socket_create(cctx->ctx, CORO_SOCKET_TCP_V4);
    if (!c) return;
    if (coro_socket_connect(c, cctx->frontend_host, cctx->frontend_port) != 0) {
        coro_socket_destroy(c);
        return;
    }

    size_t slen = strlen(cctx->send_data);
    coro_socket_send(c, cctx->send_data, slen);

    char *data = NULL;
    size_t len = 0;
    coro_socket_set_timeout(c, 2000);
    if (coro_socket_recv(c, &data, &len) == 0 && data) {
        size_t copy = len < sizeof(cctx->recv_buf) - 1
                          ? len
                          : sizeof(cctx->recv_buf) - 1;
        memcpy(cctx->recv_buf, data, copy);
        cctx->recv_buf[copy] = '\0';
        cctx->recv_len = copy;
        cctx->success = 1;
        coro_socket_free_recv(data);
    }

    coro_socket_destroy(c);

}

/* ── L7 Helpers ───────────────────────────────────────────── */

typedef struct {
    coro_context_t *ctx;
    const char *backend_host;
    int backend_port;
    const char *group;       /* group name to register */
    const char *tag;         /* prefix added to echo response */
    int sessions_served;
} l7_worker_ctx_t;

static void l7_echo_worker_coro(coro_t *co, void *arg) {
    (void)co;
    l7_worker_ctx_t *wctx = (l7_worker_ctx_t *)arg;

    coro_socket_t *c = coro_socket_create(wctx->ctx, CORO_SOCKET_TCP_V4);
    if (!c) return;
    if (coro_socket_connect(c, wctx->backend_host, wctx->backend_port) != 0) {
        coro_socket_destroy(c);
        return;
    }

    /* First message = group name */
    coro_socket_send(c, wctx->group, strlen(wctx->group));

    /* Echo with tag prefix so test can verify which worker handled it */
    char *data = NULL;
    size_t len = 0;
    while (coro_socket_recv(c, &data, &len) == 0) {
        size_t tag_len = strlen(wctx->tag);
        size_t total = tag_len + len;
        char *resp = (char *)malloc(total);
        if (resp) {
            memcpy(resp, wctx->tag, tag_len);
            memcpy(resp + tag_len, data, len);
            coro_socket_send(c, resp, total);
            free(resp);
        }
        coro_socket_free_recv(data);
        data = NULL;
    }

    wctx->sessions_served++;
    coro_socket_destroy(c);
}

/* Route callback: "API:..." → "api", "WEB:..." → "web" */
static const char *test_route_cb(const char *data, size_t len, void *arg) {
    (void)arg;
    if (len >= 4 && memcmp(data, "API:", 4) == 0) return "api";
    if (len >= 4 && memcmp(data, "WEB:", 4) == 0) return "web";
    return NULL;
}

/* L7 client — sends data, receives tagged response, then stops loop */
typedef struct {
    coro_context_t *ctx;
    const char *frontend_host;
    int frontend_port;
    const char *send_data;
    char recv_buf[256];
    int success;
    int stop_after;  /* 1 = stop loop after this client */
} l7_client_ctx_t;

static void l7_test_client_coro(coro_t *co, void *arg) {
    (void)co;
    l7_client_ctx_t *cctx = (l7_client_ctx_t *)arg;

    coro_sleep(cctx->ctx, 100);

    coro_socket_t *c = coro_socket_create(cctx->ctx, CORO_SOCKET_TCP_V4);
    if (!c) return;
    if (coro_socket_connect(c, cctx->frontend_host, cctx->frontend_port) != 0) {
        coro_socket_destroy(c);
        return;
    }

    coro_socket_send(c, cctx->send_data, strlen(cctx->send_data));

    char *data = NULL;
    size_t len = 0;
    coro_socket_set_timeout(c, 2000);
    if (coro_socket_recv(c, &data, &len) == 0 && data) {
        size_t copy = len < sizeof(cctx->recv_buf) - 1 ? len : sizeof(cctx->recv_buf) - 1;
        memcpy(cctx->recv_buf, data, copy);
        cctx->recv_buf[copy] = '\0';
        cctx->success = 1;
        coro_socket_free_recv(data);
    }

    coro_socket_destroy(c);

    (void)cctx->stop_after;
}

/* ── Tests ────────────────────────────────────────────────── */

/*
 * Simple TLV format for tests: [type:1][len:2 big-endian][payload:len]
 * Total frame = 3 + payload_len bytes.
 */

/* TLV frame callback */
static ssize_t tlv_frame_cb(const char *data, size_t len, void *arg) {
    (void)arg;
    if (len < 3) return 0;
    uint16_t payload_len =
        (uint16_t)((unsigned char)data[1] << 8 | (unsigned char)data[2]);
    size_t total = 3 + payload_len;
    return len >= total ? (ssize_t)total : 0;
}

/* Build a TLV message: type(1) + len(2) + payload */
static char *make_tlv(uint8_t type, const char *payload, size_t plen,
                       size_t *out_len) {
    *out_len = 3 + plen;
    char *buf = (char *)malloc(*out_len);
    buf[0] = (char)type;
    buf[1] = (char)((plen >> 8) & 0xFF);
    buf[2] = (char)(plen & 0xFF);
    if (plen > 0) memcpy(buf + 3, payload, plen);
    return buf;
}

/* REQUEST mode worker: stays connected, handles multiple TLV messages.
 * Echoes each TLV back with type incremented by 0x80 (response marker). */
typedef struct {
    coro_context_t *ctx;
    const char *backend_host;
    int backend_port;
    int messages_handled;
} req_worker_ctx_t;

static void req_echo_worker_coro(coro_t *co, void *arg) {
    (void)co;
    req_worker_ctx_t *wctx = (req_worker_ctx_t *)arg;

    coro_socket_t *c = coro_socket_create(wctx->ctx, CORO_SOCKET_TCP_V4);
    if (!c) return;
    if (coro_socket_connect(c, wctx->backend_host, wctx->backend_port) != 0) {
        coro_socket_destroy(c);
        return;
    }

    char *data = NULL;
    size_t len = 0;
    while (coro_socket_recv(c, &data, &len) == 0) {
        /* Flip type byte high bit as response marker */
        if (len >= 1) data[0] = (char)((unsigned char)data[0] | 0x80);
        coro_socket_send(c, data, len);
        wctx->messages_handled++;
        coro_socket_free_recv(data);
        data = NULL;
    }

    coro_socket_destroy(c);
}

/* REQUEST mode client: sends TLV messages, receives responses */
typedef struct {
    coro_context_t *ctx;
    const char *frontend_host;
    int frontend_port;
    uint8_t send_type;
    const char *send_payload;
    uint8_t recv_type;
    char recv_payload[256];
    size_t recv_payload_len;
    int success;
    int stop_after;
} tlv_client_ctx_t;

static void tlv_test_client_coro(coro_t *co, void *arg) {
    (void)co;
    tlv_client_ctx_t *cctx = (tlv_client_ctx_t *)arg;

    coro_sleep(cctx->ctx, 100);

    coro_socket_t *c = coro_socket_create(cctx->ctx, CORO_SOCKET_TCP_V4);
    if (!c) return;
    if (coro_socket_connect(c, cctx->frontend_host, cctx->frontend_port) != 0) {
        coro_socket_destroy(c);
        return;
    }

    size_t msg_len = 0;
    char *msg = make_tlv(cctx->send_type, cctx->send_payload,
                          strlen(cctx->send_payload), &msg_len);
    coro_socket_send(c, msg, msg_len);
    free(msg);

    char *data = NULL;
    size_t len = 0;
    coro_socket_set_timeout(c, 2000);
    if (coro_socket_recv(c, &data, &len) == 0 && data && len >= 3) {
        cctx->recv_type = (uint8_t)data[0];
        uint16_t plen =
            (uint16_t)((unsigned char)data[1] << 8 | (unsigned char)data[2]);
        if (plen <= len - 3 && plen < sizeof(cctx->recv_payload)) {
            memcpy(cctx->recv_payload, data + 3, plen);
            cctx->recv_payload[plen] = '\0';
            cctx->recv_payload_len = plen;
        }
        cctx->success = 1;
        coro_socket_free_recv(data);
    }

    coro_socket_destroy(c);

    (void)cctx->stop_after;
}

/* Filter: reject type 0xFF, drop type 0xFE, accept everything else */
static turbo_lb_filter_result_t test_filter_cb(const char *data, size_t len,
                                                void *arg) {
    (void)arg;
    turbo_lb_filter_result_t r = { .verdict = TURBO_LB_ACCEPT };
    if (len >= 1) {
        uint8_t type = (uint8_t)data[0];
        if (type == 0xFF) {
            r.verdict = TURBO_LB_REJECT;
            /* Build a TLV error response: type=0xFF, payload="ERR" */
            static const char reject[] = "\xFF\x00\x03" "ERR";
            r.reject_data = reject;
            r.reject_len = 6;
        } else if (type == 0xFE) {
            r.verdict = TURBO_LB_DROP;
        }
    }
    return r;
}

/* Session-mode filter: reject data starting with "BLOCK:" */
static turbo_lb_filter_result_t session_filter_cb(const char *data, size_t len,
                                                   void *arg) {
    (void)arg;
    turbo_lb_filter_result_t r = { .verdict = TURBO_LB_ACCEPT };
    if (len >= 6 && memcmp(data, "BLOCK:", 6) == 0) {
        r.verdict = TURBO_LB_REJECT;
        r.reject_data = "BLOCKED";
        r.reject_len = 7;
    }
    return r;
}

spec("coro_lb") {
    describe("lifecycle") {
        it("creates and destroys LB") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = coro_LB_CONFIG_DEFAULT;

            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(lb != NULL);

            destroy_lb_and_context(lb, ctx);
        }

        it("creates with NULL config uses defaults") {
            coro_context_t *ctx = create_lb_test_context();

            coro_lb_t *lb = coro_lb_create(ctx, NULL);
            check(lb != NULL);

            destroy_lb_and_context(lb, ctx);
        }

        it("listens on frontend and backend") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = coro_LB_CONFIG_DEFAULT;
            coro_lb_t *lb = coro_lb_create(ctx, &config);

            int r1 = coro_lb_listen(lb, "127.0.0.1", 18080);
            check(r1 == 0);

            int r2 = coro_lb_accept_workers(lb, "127.0.0.1", 19090);
            check(r2 == 0);

            destroy_lb_and_context(lb, ctx);
        }
    }

    describe("L4 echo") {
        it("forwards data between client and worker") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = coro_LB_CONFIG_DEFAULT;
            coro_lb_t *lb = coro_lb_create(ctx, &config);

            check(coro_lb_listen(lb, "127.0.0.1", 18180) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19190) == 0);

            /* Spawn echo worker */
            worker_ctx_t wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19190,
                .sessions_served = 0
            };
            spawn_test_coro(ctx, echo_worker_coro, &wctx);

            /* Spawn test client */
            client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18180,
                .send_data = "hello LB",
                .success = 0
            };
            spawn_test_coro(ctx, test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            check(cctx.success == 1);
            check(strcmp(cctx.recv_buf, "hello LB") == 0);
            run_until_flag(ctx, &wctx.sessions_served);
            run_until_coro_count_at_most(ctx, 2, 3000);
            drain_after_stop(ctx);

            destroy_lb_and_context(lb, ctx);
        }
    }

    describe("L7 routing") {
        it("routes API: prefix to api worker") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = {
                .balance = TURBO_LB_ROUND_ROBIN,
                .route_cb = test_route_cb,
                .route_cb_arg = NULL,
                .peek_bytes = 4096,
            };
            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(coro_lb_listen(lb, "127.0.0.1", 18280) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19290) == 0);

            /* Spawn api worker */
            l7_worker_ctx_t api_wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19290,
                .group = "api",
                .tag = "[API]",
                .sessions_served = 0,
            };
            spawn_test_coro(ctx, l7_echo_worker_coro, &api_wctx);

            /* Spawn web worker */
            l7_worker_ctx_t web_wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19290,
                .group = "web",
                .tag = "[WEB]",
                .sessions_served = 0,
            };
            spawn_test_coro(ctx, l7_echo_worker_coro, &web_wctx);

            /* Client sends API: prefix -> routed to api worker */
            l7_client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18280,
                .send_data = "API:get_users",
                .success = 0,
                .stop_after = 1,
            };
            spawn_test_coro(ctx, l7_test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            check(cctx.success == 1);
            check(memcmp(cctx.recv_buf, "[API]", 5) == 0);
            run_until_flag(ctx, &api_wctx.sessions_served);
            coro_lb_stop(lb);
            run_until_flag(ctx, &web_wctx.sessions_served);
            destroy_stopped_lb_and_context(lb, ctx);
        }

        it("routes WEB: prefix to web worker") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = {
                .balance = TURBO_LB_ROUND_ROBIN,
                .route_cb = test_route_cb,
                .route_cb_arg = NULL,
                .peek_bytes = 4096,
            };
            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(coro_lb_listen(lb, "127.0.0.1", 18380) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19390) == 0);

            /* Spawn api worker */
            l7_worker_ctx_t api_wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19390,
                .group = "api",
                .tag = "[API]",
                .sessions_served = 0,
            };
            spawn_test_coro(ctx, l7_echo_worker_coro, &api_wctx);

            /* Spawn web worker */
            l7_worker_ctx_t web_wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19390,
                .group = "web",
                .tag = "[WEB]",
                .sessions_served = 0,
            };
            spawn_test_coro(ctx, l7_echo_worker_coro, &web_wctx);

            /* Client sends WEB: prefix -> routed to web worker */
            l7_client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18380,
                .send_data = "WEB:index.html",
                .success = 0,
                .stop_after = 1,
            };
            spawn_test_coro(ctx, l7_test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            check(cctx.success == 1);
            check(memcmp(cctx.recv_buf, "[WEB]", 5) == 0);
            run_until_flag(ctx, &web_wctx.sessions_served);
            coro_lb_stop(lb);
            run_until_flag(ctx, &api_wctx.sessions_served);
            destroy_stopped_lb_and_context(lb, ctx);
        }

        it("unknown prefix falls back to any worker") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = {
                .balance = TURBO_LB_ROUND_ROBIN,
                .route_cb = test_route_cb,
                .route_cb_arg = NULL,
                .peek_bytes = 4096,
            };
            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(coro_lb_listen(lb, "127.0.0.1", 18480) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19490) == 0);

            /* Only spawn a web worker */
            l7_worker_ctx_t web_wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19490,
                .group = "web",
                .tag = "[WEB]",
                .sessions_served = 0,
            };
            spawn_test_coro(ctx, l7_echo_worker_coro, &web_wctx);

            /* route_cb returns NULL for unknown prefix -> matches any idle worker */
            l7_client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18480,
                .send_data = "UNKNOWN:data",
                .success = 0,
                .stop_after = 1,
            };
            spawn_test_coro(ctx, l7_test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            check(cctx.success == 1);
            check(memcmp(cctx.recv_buf, "[WEB]", 5) == 0);
            run_until_flag(ctx, &web_wctx.sessions_served);
            run_until_coro_count_at_most(ctx, 2, 3000);
            drain_after_stop(ctx);

            destroy_lb_and_context(lb, ctx);
        }
    }

    describe("REQUEST mode TLV") {
        it("dispatches TLV message and gets response") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = {
                .balance = TURBO_LB_ROUND_ROBIN,
                .mode = TURBO_LB_MODE_REQUEST,
                .frame_cb = tlv_frame_cb,
            };
            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(coro_lb_listen(lb, "127.0.0.1", 18580) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19590) == 0);

            req_worker_ctx_t wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19590,
                .messages_handled = 0,
            };
            spawn_test_coro(ctx, req_echo_worker_coro, &wctx);

            tlv_client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18580,
                .send_type = 0x01,
                .send_payload = "hello",
                .success = 0,
                .stop_after = 1,
            };
            spawn_test_coro(ctx, tlv_test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            check(cctx.success == 1);
            check(cctx.recv_type == 0x81);  /* 0x01 | 0x80 */
            check(strcmp(cctx.recv_payload, "hello") == 0);
            run_until_flag(ctx, &wctx.messages_handled);
            run_until_coro_count_at_most(ctx, 3, 3000);
            drain_after_stop(ctx);

            destroy_lb_and_context(lb, ctx);
        }
    }

    describe("filter") {
        it("REQUEST mode: rejects forbidden TLV type with error response") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = {
                .balance = TURBO_LB_ROUND_ROBIN,
                .mode = TURBO_LB_MODE_REQUEST,
                .frame_cb = tlv_frame_cb,
                .filter_cb = test_filter_cb,
            };
            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(coro_lb_listen(lb, "127.0.0.1", 18680) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19690) == 0);

            req_worker_ctx_t wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19690,
                .messages_handled = 0,
            };
            spawn_test_coro(ctx, req_echo_worker_coro, &wctx);

            /* Send type=0xFF which filter rejects */
            tlv_client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18680,
                .send_type = 0xFF,
                .send_payload = "bad",
                .success = 0,
                .stop_after = 1,
            };
            spawn_test_coro(ctx, tlv_test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            /* Client receives the reject TLV, not a worker echo */
            check(cctx.success == 1);
            check(cctx.recv_type == 0xFF);
            check(strcmp(cctx.recv_payload, "ERR") == 0);
            /* Worker should NOT have handled anything */
            check(wctx.messages_handled == 0);

            destroy_lb_and_context(lb, ctx);
        }

        it("SESSION mode: rejects blocked connection") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = {
                .balance = TURBO_LB_ROUND_ROBIN,
                .mode = TURBO_LB_MODE_SESSION,
                .route_cb = test_route_cb,
                .peek_bytes = 4096,
                .filter_cb = session_filter_cb,
            };
            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(coro_lb_listen(lb, "127.0.0.1", 18780) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19790) == 0);

            l7_worker_ctx_t web_wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19790,
                .group = "web",
                .tag = "[WEB]",
                .sessions_served = 0,
            };
            spawn_test_coro(ctx, l7_echo_worker_coro, &web_wctx);

            /* Client sends "BLOCK:..." which filter rejects */
            l7_client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18780,
                .send_data = "BLOCK:malicious",
                .success = 0,
                .stop_after = 1,
            };
            spawn_test_coro(ctx, l7_test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            /* Client receives reject response */
            check(cctx.success == 1);
            check(strcmp(cctx.recv_buf, "BLOCKED") == 0);

            destroy_lb_and_context(lb, ctx);
        }

        it("SESSION mode: accepts normal connection through filter") {
            coro_context_t *ctx = create_lb_test_context();
            coro_lb_config_t config = {
                .balance = TURBO_LB_ROUND_ROBIN,
                .mode = TURBO_LB_MODE_SESSION,
                .route_cb = test_route_cb,
                .peek_bytes = 4096,
                .filter_cb = session_filter_cb,
            };
            coro_lb_t *lb = coro_lb_create(ctx, &config);
            check(coro_lb_listen(lb, "127.0.0.1", 18880) == 0);
            check(coro_lb_accept_workers(lb, "127.0.0.1", 19890) == 0);

            l7_worker_ctx_t api_wctx = {
                .ctx = ctx,
                .backend_host = "127.0.0.1",
                .backend_port = 19890,
                .group = "api",
                .tag = "[API]",
                .sessions_served = 0,
            };
            spawn_test_coro(ctx, l7_echo_worker_coro, &api_wctx);

            /* Normal data passes filter */
            l7_client_ctx_t cctx = {
                .ctx = ctx,
                .frontend_host = "127.0.0.1",
                .frontend_port = 18880,
                .send_data = "API:legit_request",
                .success = 0,
                .stop_after = 1,
            };
            spawn_test_coro(ctx, l7_test_client_coro, &cctx);

            run_until_flag(ctx, &cctx.success);

            check(cctx.success == 1);
            check(memcmp(cctx.recv_buf, "[API]", 5) == 0);
            run_until_flag(ctx, &api_wctx.sessions_served);
            run_until_coro_count_at_most(ctx, 2, 3000);
            drain_after_stop(ctx);

            destroy_lb_and_context(lb, ctx);
        }
    }
}
