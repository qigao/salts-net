#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/**
 * @file turbo_stream_epoll.c
 * @brief High-performance Linux epoll backend for turbo_stream_t.
 *
 * Implements a shared epoll reactor per process. Stream instances own their
 * buffers and state, while a bounded number of worker threads multiplex all
 * registered file descriptors.
 */

#ifdef __linux__

#include "turbo_stream_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_internal.h"
#include "turbo_buffer.h"
#include "turbo_thread.h"
#include "tlog.h"
#include "ring_buffer_spsc.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

/* ── Constants ───────────────────────────────────────────── */

#define EVENT_RING_BYTES (64 * 1024)
#define DATA_RING_SIZE   (128 * 1024)

typedef enum {
    SEP_OP_NONE,
    SEP_OP_CONNECT,
    SEP_OP_ACCEPT,
    SEP_OP_READ,
    SEP_OP_WRITE,
    SEP_OP_CLOSE,
    SEP_OP_ERROR,
    SEP_OP_TICK
} ep_op_kind_t;

typedef struct {
    int kind;
    int status;
    void *extra;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
} stream_epoll_event_t;

#include <sys/eventfd.h>

typedef struct stream_epoll_state_s {
    void *owner;         /* turbo_stream_t or turbo_stream_listener_t */
    coro_context_t *ctx;
    int fd;
    int fd_armed;
    int connected;
    int send_pending;
    int event_write_lock;
    int event_post_pending;
    int terminal_posted;
    int is_listener;
    atomic_int stopping;
    int registered;
    int cleanup_posted;
    int close_fd_on_cleanup;
    int ctx_ref_acquired;
    struct stream_epoll_state_s *reactor_free_next;
    
    /* Event Notification Ring (SPSC from worker to main) */
    ring_spsc_t event_ring;
    uint8_t *event_buf;

    /* Data Rings (SPSC) */
    ring_spsc_t read_ring;
    uint8_t *read_buf;

    ring_spsc_t write_ring;
    uint8_t *write_buf;
} stream_epoll_state_t;

typedef enum {
    REACTOR_CMD_ADD = 1,
    REACTOR_CMD_MOD,
    REACTOR_CMD_REMOVE,
    REACTOR_CMD_WAKE,
    REACTOR_CMD_CLOSE,
    REACTOR_CMD_FREE,
    REACTOR_CMD_STOP
} reactor_cmd_kind_t;

typedef struct reactor_cmd_s {
    reactor_cmd_kind_t kind;
    stream_epoll_state_t *st;
    uint32_t events;
} reactor_cmd_t;

typedef struct {
    int epoll_fd;
    int wake_fd;
    turbo_thread_t worker_thread;
    turbo_mutex_t lock;
    int initialized;
    int stopping;
    reactor_cmd_t cmds[4096];
    size_t cmd_head;
    size_t cmd_tail;
    stream_epoll_state_t *deferred_free_head;
} stream_epoll_reactor_t;

static stream_epoll_reactor_t g_reactor;
static turbo_once_t g_reactor_once = TURBO_ONCE_INIT;

/* ── Helpers ─────────────────────────────────────────────── */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int epoll_is_stopping(stream_epoll_state_t *st) {
    if (!st) {
        return 1;
    }
    return atomic_load_explicit(&st->stopping, memory_order_acquire) != 0;
}

static void epoll_request_stop(stream_epoll_state_t *st) {
    if (st) {
        atomic_store_explicit(&st->stopping, 1, memory_order_release);
    }
}

static void post_event(stream_epoll_state_t *st, ep_op_kind_t kind, int status,
                       void *extra, const struct sockaddr *peer_addr,
                       socklen_t peer_addr_len);
static void epoll_post_wait(stream_epoll_state_t *st, coro_post_fn fn, void *arg1, void *arg2);
static void on_epoll_event_bounce(void *arg1, void *arg2);
static void handle_read_event(stream_epoll_state_t *st, stream_epoll_event_t *ev);
static void handle_write_event(stream_epoll_state_t *st, stream_epoll_event_t *ev);
static void flush_write_ring(stream_epoll_state_t *st);
static void try_flush_write_ring(stream_epoll_state_t *st);
static void epoll_destroy_state(stream_epoll_state_t *st);
static void epoll_shutdown_state(stream_epoll_state_t *st);
static void epoll_release_context_ref(stream_epoll_state_t *st);
static void epoll_stream_cleanup_task(void *arg1, void *arg2);
static void epoll_listener_cleanup_task(void *arg1, void *arg2);
static int epoll_register_state(stream_epoll_state_t *st, uint32_t events);
static int epoll_reactor_wake_state(stream_epoll_state_t *st);
static int epoll_reactor_close_state(stream_epoll_state_t *st);
static void epoll_reactor_free_state(stream_epoll_state_t *st);

static void epoll_event_write_lock(stream_epoll_state_t *st) {
    while (__sync_lock_test_and_set(&st->event_write_lock, 1)) {
        turbo_thread_yield();
    }
}

static void epoll_event_write_unlock(stream_epoll_state_t *st) {
    __sync_lock_release(&st->event_write_lock);
}

static void epoll_disarm_stream_fd(stream_epoll_state_t *st) {
    if (!st || st->is_listener || st->fd < 0 || !st->fd_armed) {
        return;
    }
    if (g_reactor.epoll_fd >= 0) {
        (void)epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_DEL, st->fd, NULL);
    }
    st->fd_armed = 0;
    st->registered = 0;
}

static void post_terminal_event(stream_epoll_state_t *st, int status) {
    if (!st || st->is_listener || epoll_is_stopping(st)) {
        return;
    }
    if (!__sync_bool_compare_and_swap(&st->terminal_posted, 0, 1)) {
        return;
    }
    st->connected = 0;
    epoll_disarm_stream_fd(st);
    post_event(st, SEP_OP_READ, status, NULL, NULL, 0);
}

static uint8_t *wait_ring_write(stream_epoll_state_t *st, ring_spsc_t *ring, size_t size) {
    uint8_t *ptr;

    for (;;) {
        if (epoll_is_stopping(st)) {
            return NULL;
        }
        ptr = ring_spsc_write_acquire(ring, size);
        if (ptr) {
            return ptr;
        }

        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        if (ring == &st->write_ring) {
            (void)epoll_reactor_wake_state(st);
        }
        turbo_thread_yield();
    }
}

static void epoll_post_wait(stream_epoll_state_t *st, coro_post_fn fn, void *arg1, void *arg2) {
    if (!st || !fn) {
        return;
    }

    while (coro_post(st->ctx, fn, arg1, arg2) != 0) {
        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        turbo_thread_yield();
    }
}

static void epoll_post_event_bounce(stream_epoll_state_t *st) {
    if (!st || epoll_is_stopping(st)) {
        return;
    }
    if (!__sync_bool_compare_and_swap(&st->event_post_pending, 0, 1)) {
        return;
    }
    epoll_post_wait(st, on_epoll_event_bounce, st, NULL);
}

static void post_event(stream_epoll_state_t *st, ep_op_kind_t kind, int status,
                       void *extra, const struct sockaddr *peer_addr,
                       socklen_t peer_addr_len) {
    uint8_t *ptr;

    if (!st) {
        return;
    }
    epoll_event_write_lock(st);
    for (;;) {
        if (epoll_is_stopping(st)) {
            epoll_event_write_unlock(st);
            return;
        }
        ptr = ring_spsc_write_acquire(&st->event_ring, sizeof(stream_epoll_event_t));
        if (ptr) {
            stream_epoll_event_t *ev = (stream_epoll_event_t *)ptr;
            ev->kind = kind;
            ev->status = status;
            ev->extra = extra;
            memset(&ev->peer_addr, 0, sizeof(ev->peer_addr));
            ev->peer_addr_len = 0;
            if (peer_addr && peer_addr_len > 0) {
                if (peer_addr_len > (socklen_t)sizeof(ev->peer_addr)) {
                    peer_addr_len = (socklen_t)sizeof(ev->peer_addr);
                }
                memcpy(&ev->peer_addr, peer_addr, (size_t)peer_addr_len);
                ev->peer_addr_len = peer_addr_len;
            }
            ring_spsc_write_release(&st->event_ring, sizeof(stream_epoll_event_t));
            epoll_event_write_unlock(st);
            break;
        }

        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        turbo_thread_yield();
    }

    epoll_post_event_bounce(st);
}

static void handle_read_event(stream_epoll_state_t *st, stream_epoll_event_t *ev) {
    turbo_stream_t *s = (turbo_stream_t *)st->owner;
    size_t bytes = 0;
    uint8_t *data = ring_spsc_read_acquire(&st->read_ring, &bytes);
    int close_requested = 0;

    if (ev->status != 0) {
        TLOG_DEBUG("epoll[{:p}] read-event stream={:p} status={:d} bytes={:d}", (void *)st,
                   (void *)s, ev->status, (int)bytes);
    }

    if (!s || s->closing || s->finalized) {
        if (data && bytes > 0) {
            ring_spsc_read_release(&st->read_ring, bytes);
        }
        return;
    }
    if (!s->on_recv) {
        return;
    }

    if (data && bytes > 0) {
        mem_slice_t slice;

        slice.buffer = NULL;
        slice.data = (char *)data;
        slice.length = bytes;
        close_requested = s->on_recv(s, &slice, NULL);
        ring_spsc_read_release(&st->read_ring, bytes);
    } else if (ev->status != 0) {
        s->on_recv(s, NULL, NULL);
    }

    if ((ev->status != 0 || close_requested) && !s->closing) {
        turbo_stream_close(s);
    }
}

static void handle_write_event(stream_epoll_state_t *st, stream_epoll_event_t *ev) {
    turbo_stream_t *s = (turbo_stream_t *)st->owner;
    if (s && !s->closing && !s->finalized && s->on_write_complete) {
        s->on_write_complete(s, ev->status);
    }
}

static void flush_write_ring(stream_epoll_state_t *st) {
    int wrote = 0;

    while (!epoll_is_stopping(st)) {
        size_t avail = 0;
        uint8_t *data = ring_spsc_read_acquire(&st->write_ring, &avail);
        if (!data || avail == 0) {
            break;
        }

        ssize_t sent = send(st->fd, data, avail, MSG_NOSIGNAL);
        if (sent > 0) {
            ring_spsc_read_release(&st->write_ring, (size_t)sent);
            wrote = 1;
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        }

        {
            int status = (sent < 0) ? -errno : TURBO_EOF;
            post_event(st, SEP_OP_WRITE, status, NULL, NULL, 0);
            post_terminal_event(st, status);
        }
        return;
    }

    if (wrote) {
        post_event(st, SEP_OP_WRITE, 0, NULL, NULL, 0);
    }
}

static void try_flush_write_ring(stream_epoll_state_t *st) {
    if (!st || st->is_listener || epoll_is_stopping(st)) {
        return;
    }
    if (!__sync_bool_compare_and_swap(&st->send_pending, 0, 1)) {
        return;
    }
    flush_write_ring(st);
    __sync_lock_release(&st->send_pending);
}

static void on_epoll_event_bounce(void *arg1, void *arg2) {
    UNUSED(arg2);
    stream_epoll_state_t *st = (stream_epoll_state_t *)arg1;
    if (!st) return;

    for (;;) {
        size_t available = 0;
        while ((available = ring_spsc_read_available(&st->event_ring)) >= sizeof(stream_epoll_event_t)) {
            size_t chunk_avail = 0;
            uint8_t *ptr = ring_spsc_read_acquire(&st->event_ring, &chunk_avail);
            if (!ptr) break;

            size_t processed = 0;
            while (processed + sizeof(stream_epoll_event_t) <= chunk_avail) {
                stream_epoll_event_t *ev = (stream_epoll_event_t *)(ptr + processed);

                switch (ev->kind) {
                    case SEP_OP_ACCEPT: {
                        turbo_stream_listener_t *l = (turbo_stream_listener_t *)st->owner;
                        turbo_stream_t *child = (turbo_stream_t *)ev->extra;
                        void *peer = (ev->peer_addr_len > 0) ? (void *)&ev->peer_addr : NULL;
                        if (l->on_accept) l->on_accept(l, child, peer);
                        break;
                    }
                    case SEP_OP_CONNECT: {
                        turbo_stream_t *s = (turbo_stream_t *)st->owner;
                        if (s && !s->closing && !s->finalized) {
                            s->connected = (ev->status == 0);
                            if (s->on_connect) s->on_connect(s, ev->status, NULL);
                        }
                        break;
                    }
                    case SEP_OP_READ: {
                        handle_read_event(st, ev);
                        break;
                    }
                    case SEP_OP_WRITE: {
                        handle_write_event(st, ev);
                        break;
                    }
                    case SEP_OP_ERROR: {
                        if (st->is_listener) {
                            turbo_stream_listener_t *l = (turbo_stream_listener_t *)st->owner;
                            TLOG_ERROR("epoll listener accept failed: {:d}", ev->status);
                            turbo_stream_listener_close(l);
                        }
                        break;
                    }
                    default: break;
                }
                processed += sizeof(stream_epoll_event_t);
            }
            ring_spsc_read_release(&st->event_ring, processed);
        }

        __atomic_store_n(&st->event_post_pending, 0, __ATOMIC_RELEASE);
        if (ring_spsc_read_available(&st->event_ring) < sizeof(stream_epoll_event_t)) {
            return;
        }
        if (!__sync_bool_compare_and_swap(&st->event_post_pending, 0, 1)) {
            return;
        }
    }
}

/* ── Shared Reactor Thread ───────────────────────────────── */

static void epoll_reactor_signal(void) {
    if (g_reactor.wake_fd >= 0) {
        uint64_t val = 1;
        ssize_t n;
        do {
            n = write(g_reactor.wake_fd, &val, sizeof(val));
        } while (n < 0 && errno == EINTR);
    }
}

static int epoll_reactor_push(reactor_cmd_kind_t kind, stream_epoll_state_t *st,
                              uint32_t events) {
    size_t next_head;

    turbo_mutex_lock(&g_reactor.lock);
    for (;;) {
        next_head = (g_reactor.cmd_head + 1) %
                    (sizeof(g_reactor.cmds) / sizeof(g_reactor.cmds[0]));
        if (next_head != g_reactor.cmd_tail) {
            break;
        }
        turbo_mutex_unlock(&g_reactor.lock);
        turbo_thread_yield();
        turbo_mutex_lock(&g_reactor.lock);
    }

    g_reactor.cmds[g_reactor.cmd_head].kind = kind;
    g_reactor.cmds[g_reactor.cmd_head].st = st;
    g_reactor.cmds[g_reactor.cmd_head].events = events;
    g_reactor.cmd_head = next_head;
    turbo_mutex_unlock(&g_reactor.lock);
    epoll_reactor_signal();
    return 0;
}

static int epoll_reactor_pop(reactor_cmd_t *out) {
    int has_cmd = 0;

    turbo_mutex_lock(&g_reactor.lock);
    if (g_reactor.cmd_tail != g_reactor.cmd_head) {
        *out = g_reactor.cmds[g_reactor.cmd_tail];
        g_reactor.cmd_tail = (g_reactor.cmd_tail + 1) %
                             (sizeof(g_reactor.cmds) / sizeof(g_reactor.cmds[0]));
        has_cmd = 1;
    }
    turbo_mutex_unlock(&g_reactor.lock);
    return has_cmd;
}

static void epoll_reactor_close_fd(stream_epoll_state_t *st) {
    if (!st) {
        return;
    }
    if (st->fd_armed && g_reactor.epoll_fd >= 0 && st->fd >= 0) {
        (void)epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_DEL, st->fd, NULL);
    }
    st->fd_armed = 0;
    st->registered = 0;
    if (st->fd >= 0) {
        if (st->close_fd_on_cleanup) {
            close(st->fd);
        }
        st->fd = -1;
    }
}

static void epoll_reactor_defer_free(stream_epoll_state_t *st) {
    if (!st) {
        return;
    }
    st->reactor_free_next = g_reactor.deferred_free_head;
    g_reactor.deferred_free_head = st;
}

static void epoll_reactor_drain_deferred_frees(void) {
    stream_epoll_state_t *st = g_reactor.deferred_free_head;

    g_reactor.deferred_free_head = NULL;
    while (st) {
        stream_epoll_state_t *next = st->reactor_free_next;
        st->reactor_free_next = NULL;
        epoll_destroy_state(st);
        st = next;
    }
}

static void epoll_reactor_process_commands(void) {
    reactor_cmd_t cmd;

    while (epoll_reactor_pop(&cmd)) {
        stream_epoll_state_t *st = cmd.st;
        struct epoll_event ev;

        if (cmd.kind == REACTOR_CMD_STOP) {
            g_reactor.stopping = 1;
            continue;
        }
        if (!st) {
            continue;
        }

        switch (cmd.kind) {
            case REACTOR_CMD_ADD:
            case REACTOR_CMD_MOD:
                if (epoll_is_stopping(st) || st->fd < 0) {
                    break;
                }
                memset(&ev, 0, sizeof(ev));
                ev.events = cmd.events;
                ev.data.ptr = st;
                if (!st->fd_armed) {
                    if (epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_ADD, st->fd, &ev) == 0 ||
                        errno == EEXIST) {
                        st->fd_armed = 1;
                        st->registered = 1;
                    } else {
                        st->registered = 0;
                        post_event(st, SEP_OP_ERROR, -errno, NULL, NULL, 0);
                    }
                } else if (epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_MOD, st->fd, &ev) != 0 &&
                           errno != ENOENT) {
                    post_event(st, SEP_OP_ERROR, -errno, NULL, NULL, 0);
                }
                break;

            case REACTOR_CMD_REMOVE:
                epoll_reactor_close_fd(st);
                break;

            case REACTOR_CMD_WAKE:
                if (!epoll_is_stopping(st) && !st->is_listener) {
                    try_flush_write_ring(st);
                }
                break;

            case REACTOR_CMD_CLOSE:
                epoll_reactor_close_fd(st);
                if (st->is_listener) {
                    epoll_post_wait(st, epoll_listener_cleanup_task, st, st->owner);
                } else {
                    epoll_post_wait(st, epoll_stream_cleanup_task, st, st->owner);
                }
                break;

            case REACTOR_CMD_FREE:
                epoll_reactor_defer_free(st);
                break;

            default:
                break;
        }
    }
}

static void stream_epoll_handle_listener_event(stream_epoll_state_t *st) {
    if (!st || epoll_is_stopping(st)) {
        return;
    }

    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int client_fd = accept4(st->fd, (struct sockaddr *)&addr, &addr_len,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            post_event(st, SEP_OP_ERROR, -errno, NULL, NULL, 0);
            return;
        }

        turbo_stream_listener_t *l = (turbo_stream_listener_t *)st->owner;
        turbo_stream_t *child = turbo_stream_create(l->ctx, l->kind);
        if (child) {
            extern int epoll_init_with_socket(turbo_stream_t *s, int existing);
            if (epoll_init_with_socket(child, client_fd) == 0 &&
                turbo_stream_listener_configure_child(l, child) == 0 &&
                turbo_stream_apply_native_socket_options(child, client_fd) == 0) {
                child->connected = 1;
                child->listener = l;
                l->active_connections++;
                post_event(st, SEP_OP_ACCEPT, 0, child,
                           (const struct sockaddr *)&addr, addr_len);
                continue;
            }
            turbo_stream_destroy(child);
        } else {
            close(client_fd);
        }

        post_event(st, SEP_OP_ERROR, TURBO_ENOMEM, NULL, NULL, 0);
        return;
    }
}

static void stream_epoll_handle_stream_event(stream_epoll_state_t *st, uint32_t evmask) {
    uint8_t io_buf[8192];
    int read_terminal_reported = 0;

    if (!st || epoll_is_stopping(st)) {
        return;
    }

    if (evmask & EPOLLOUT) {
        if (!st->connected) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(st->fd, SOL_SOCKET, SO_ERROR, &err, &len);
            st->connected = 1;
            post_event(st, SEP_OP_CONNECT, (err == 0) ? 0 : -err, NULL, NULL, 0);
        }
        try_flush_write_ring(st);
    }

    if (evmask & EPOLLIN) {
        for (;;) {
            ssize_t n = recv(st->fd, io_buf, sizeof(io_buf), 0);
            if (n > 0) {
                uint8_t *dest = ring_spsc_write_acquire(&st->read_ring, (size_t)n);
                if (dest == NULL) {
                    post_event(st, SEP_OP_READ, 0, NULL, NULL, 0);
                    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
                    break;
                }
                memcpy(dest, io_buf, (size_t)n);
                ring_spsc_write_release(&st->read_ring, (size_t)n);
                post_event(st, SEP_OP_READ, 0, NULL, NULL, 0);
                continue;
            }
            if (n == 0) {
                post_terminal_event(st, TURBO_EOF);
                read_terminal_reported = 1;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            post_terminal_event(st, -errno);
            read_terminal_reported = 1;
            break;
        }
    }

    if ((evmask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) && !read_terminal_reported) {
        if (evmask & EPOLLERR) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(st->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err == 0) {
                err = EIO;
            }
            post_terminal_event(st, -err);
        } else {
            post_terminal_event(st, TURBO_EOF);
        }
    }
}

static void stream_epoll_reactor_worker(void *arg) {
    (void)arg;

    while (!g_reactor.stopping) {
        struct epoll_event events[64];
        int nfds = epoll_wait(g_reactor.epoll_fd, events, 64, -1);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            TLOG_ERROR("epoll reactor wait failed err={:d}", errno);
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == &g_reactor) {
                uint64_t val;
                while (read(g_reactor.wake_fd, &val, sizeof(val)) > 0) {
                }
                epoll_reactor_process_commands();
                continue;
            }

            stream_epoll_state_t *st = (stream_epoll_state_t *)events[i].data.ptr;
            if (!st || epoll_is_stopping(st)) {
                continue;
            }
            if (st->is_listener) {
                stream_epoll_handle_listener_event(st);
            } else {
                stream_epoll_handle_stream_event(st, events[i].events);
            }
        }
        /* epoll may return the same state more than once in one event batch. */
        epoll_reactor_drain_deferred_frees();
    }
    epoll_reactor_drain_deferred_frees();
}

static void epoll_reactor_init_once(void) {
    struct epoll_event ev;

    memset(&g_reactor, 0, sizeof(g_reactor));
    g_reactor.epoll_fd = -1;
    g_reactor.wake_fd = -1;
    turbo_mutex_init(&g_reactor.lock);

    g_reactor.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_reactor.epoll_fd < 0) {
        return;
    }

    g_reactor.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_reactor.wake_fd < 0) {
        close(g_reactor.epoll_fd);
        g_reactor.epoll_fd = -1;
        return;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_reactor;
    if (epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_ADD, g_reactor.wake_fd, &ev) < 0) {
        close(g_reactor.wake_fd);
        close(g_reactor.epoll_fd);
        g_reactor.wake_fd = -1;
        g_reactor.epoll_fd = -1;
        return;
    }

    if (turbo_thread_create(&g_reactor.worker_thread, stream_epoll_reactor_worker, NULL) != 0) {
        close(g_reactor.wake_fd);
        close(g_reactor.epoll_fd);
        g_reactor.wake_fd = -1;
        g_reactor.epoll_fd = -1;
        return;
    }

    g_reactor.initialized = 1;
}

static int epoll_reactor_ensure(void) {
    turbo_once(&g_reactor_once, epoll_reactor_init_once);
    return g_reactor.initialized ? 0 : TURBO_ENOMEM;
}

static int epoll_register_state(stream_epoll_state_t *st, uint32_t events) {
    int rc;

    if (!st || st->fd < 0) {
        return TURBO_EINVAL;
    }
    if (st->registered) {
        return 0;
    }
    rc = epoll_reactor_ensure();
    if (rc != 0) {
        return rc;
    }
    st->registered = 1;
    return epoll_reactor_push(st->fd_armed ? REACTOR_CMD_MOD : REACTOR_CMD_ADD, st, events);
}

static int epoll_reactor_wake_state(stream_epoll_state_t *st) {
    if (!st || epoll_is_stopping(st)) {
        return TURBO_ECANCELED;
    }
    if (epoll_reactor_ensure() != 0) {
        return TURBO_ENOMEM;
    }
    return epoll_reactor_push(REACTOR_CMD_WAKE, st, 0);
}

static int epoll_reactor_close_state(stream_epoll_state_t *st) {
    if (!st || !__sync_bool_compare_and_swap(&st->cleanup_posted, 0, 1)) {
        return 0;
    }
    if (epoll_reactor_ensure() != 0) {
        return TURBO_ENOMEM;
    }
    return epoll_reactor_push(REACTOR_CMD_CLOSE, st, 0);
}

static void epoll_reactor_free_state(stream_epoll_state_t *st) {
    if (!st) {
        return;
    }
    if (!g_reactor.initialized) {
        epoll_destroy_state(st);
        return;
    }
    (void)epoll_reactor_push(REACTOR_CMD_FREE, st, 0);
}

/* ── Backend Ops ─────────────────────────────────────────── */

static int epoll_init_state(stream_epoll_state_t **out, void *owner, coro_context_t *ctx, int is_listener) {
    stream_epoll_state_t *st;
    int rc;

    st = (stream_epoll_state_t *)calloc(1, sizeof(stream_epoll_state_t));
    if (!st) return TURBO_ENOMEM;

    st->owner = owner;
    st->ctx = ctx;
    st->fd = -1;
    st->is_listener = is_listener;
    st->close_fd_on_cleanup = 1;
    coro_context_acquire_external(st->ctx);
    st->ctx_ref_acquired = 1;

    st->event_buf = (uint8_t *)malloc(EVENT_RING_BYTES);
    if (!st->event_buf) {
        rc = TURBO_ENOMEM;
        goto fail;
    }
    if (!ring_spsc_init(&st->event_ring, st->event_buf, EVENT_RING_BYTES)) {
        rc = TURBO_EINVAL;
        goto fail;
    }

    if (!is_listener) {
        st->read_buf = (uint8_t *)malloc(DATA_RING_SIZE);
        if (!st->read_buf) {
            rc = TURBO_ENOMEM;
            goto fail;
        }
        if (!ring_spsc_init(&st->read_ring, st->read_buf, DATA_RING_SIZE)) {
            rc = TURBO_EINVAL;
            goto fail;
        }

        st->write_buf = (uint8_t *)malloc(DATA_RING_SIZE);
        if (!st->write_buf) {
            rc = TURBO_ENOMEM;
            goto fail;
        }
        if (!ring_spsc_init(&st->write_ring, st->write_buf, DATA_RING_SIZE)) {
            rc = TURBO_EINVAL;
            goto fail;
        }
    }

    *out = st;
    return 0;

fail:
    epoll_destroy_state(st);
    return rc;
}

static void epoll_destroy_state(stream_epoll_state_t *st) {
    if (!st) return;
    if (st->fd >= 0) {
        if (st->close_fd_on_cleanup) {
            close(st->fd);
        }
        st->fd = -1;
    }
    free(st->event_buf);
    free(st->read_buf);
    free(st->write_buf);
    epoll_release_context_ref(st);
    free(st);
}

static void epoll_release_context_ref(stream_epoll_state_t *st) {
    if (!st) {
        return;
    }
    if (st->ctx_ref_acquired) {
        coro_context_release_external(st->ctx);
        st->ctx_ref_acquired = 0;
    }
}

static void epoll_shutdown_state(stream_epoll_state_t *st) {
    if (!st) return;
    TLOG_DEBUG("epoll[{:p}] shutdown-begin fd={:d} listener={:d}",
               (void *)st, st->fd, st->is_listener);
    epoll_request_stop(st);
    if (st->fd >= 0) {
        (void)shutdown(st->fd, SHUT_RDWR);
    }
    if (epoll_reactor_close_state(st) != 0) {
        if (st->is_listener) {
            epoll_listener_cleanup_task(st, st->owner);
        } else {
            epoll_stream_cleanup_task(st, st->owner);
        }
    }
}

static void epoll_stream_cleanup_task(void *arg1, void *arg2) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)arg1;
    turbo_stream_t *s = (turbo_stream_t *)arg2;
    if (s) s->backend_data = NULL;
    if (s) turbo_stream_finalize_close(s);
    epoll_release_context_ref(st);
    epoll_reactor_free_state(st);
}

static void epoll_listener_cleanup_task(void *arg1, void *arg2) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)arg1;
    turbo_stream_listener_t *l = (turbo_stream_listener_t *)arg2;
    if (l) turbo_stream_listener_notify_backend_released(l);
    epoll_release_context_ref(st);
    epoll_reactor_free_state(st);
}

static int epoll_init(turbo_stream_t *s) {
    return epoll_init_state((stream_epoll_state_t **)&s->backend_data, s, s->ctx, 0);
}

int epoll_init_with_socket(turbo_stream_t *s, int existing) {
    stream_epoll_state_t *st;
    int r;

    if (!s) {
        return TURBO_EINVAL;
    }
    if (!s->backend_data) {
        r = epoll_init(s);
        if (r != 0) return r;
    }
    st = (stream_epoll_state_t *)s->backend_data;
    if (st->fd >= 0 && st->fd != existing) {
        close(st->fd);
    }
    st->fd = existing;
    st->connected = 1;
    return 0;
}

static int epoll_connect(turbo_stream_t *s, const struct sockaddr *a) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)s->backend_data;
    int connect_rc;
    int rc;

    rc = epoll_reactor_ensure();
    if (rc != 0) {
        return rc;
    }
    st->fd = socket(a->sa_family, SOCK_STREAM, 0);
    if (st->fd < 0) return -errno;
    set_nonblocking(st->fd);
    rc = turbo_stream_apply_native_socket_options(s, st->fd);
    if (rc != 0) {
        close(st->fd);
        st->fd = -1;
        return rc;
    }
    socklen_t addr_len = (a->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : 
                         (a->sa_family == AF_UNIX)  ? sizeof(struct sockaddr_un) : 
                                                      sizeof(struct sockaddr_in);
    connect_rc = connect(st->fd, a, addr_len);
    if (connect_rc < 0 && errno != EINPROGRESS) {
        int err = -errno;
        close(st->fd);
        st->fd = -1;
        return err;
    }

    rc = epoll_register_state(st, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
    if (rc != 0) {
        close(st->fd);
        st->fd = -1;
        return rc;
    }

    if (connect_rc == 0) {
        st->connected = 1;
        TLOG_DEBUG("epoll[{:p}] connect-immediate fd={:d}", (void *)st, st->fd);
        post_event(st, SEP_OP_CONNECT, 0, NULL, NULL, 0);
    }

    return 0;
}

static int epoll_connect_pipe(turbo_stream_t *s, const char *n) {
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; strncpy(addr.sun_path, n, sizeof(addr.sun_path)-1);
    return epoll_connect(s, (struct sockaddr *)&addr);
}

static int epoll_send(turbo_stream_t *s, const char *d, size_t l) {
    stream_epoll_state_t *st;
    int rc;
    size_t offset;

    if (!s || !s->backend_data || !d || l == 0) {
        return TURBO_EINVAL;
    }
    st = (stream_epoll_state_t *)s->backend_data;
    rc = turbo_stream_send_hwm_check(s, l, ring_spsc_read_available(&st->write_ring));
    if (rc != 0) {
        return rc;
    }
    rc = epoll_register_state(st, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
    if (rc != 0) {
        return rc;
    }

    offset = 0;
    while (offset < l) {
        size_t chunk = l - offset;
        uint8_t *dest;

        if (chunk > 16 * 1024) {
            chunk = 16 * 1024;
        }

        dest = wait_ring_write(st, &st->write_ring, chunk);
        if (dest == NULL) {
            return TURBO_ECANCELED;
        }
        memcpy(dest, d + offset, chunk);
        ring_spsc_write_release(&st->write_ring, chunk);
        offset += chunk;
        (void)epoll_reactor_wake_state(st);
    }

    return 0;
}

static int epoll_recv_start(turbo_stream_t *s) {
    stream_epoll_state_t *st = s ? (stream_epoll_state_t *)s->backend_data : NULL;
    int rc;

    if (!st) {
        return TURBO_EINVAL;
    }
    if (!st->is_listener) {
        rc = epoll_register_state(st, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
        if (rc != 0) {
            return rc;
        }
    }
    if (!st->is_listener && ring_spsc_read_available(&st->read_ring) > 0) {
        post_event(st, SEP_OP_READ, 0, NULL, NULL, 0);
    }
    return 0;
}
static void epoll_recv_stop(turbo_stream_t *s) { (void)s; }

static void epoll_close(turbo_stream_t *s) {
    stream_epoll_state_t *st = s ? (stream_epoll_state_t *)s->backend_data : NULL;
    if (!s) return;
    if (!st) {
        turbo_stream_finalize_close(s);
        return;
    }

    TLOG_DEBUG("epoll[{:p}] stream-close stream={:p} fd={:d}", (void *)st, (void *)s, st->fd);

    epoll_shutdown_state(st);
}

static int epoll_bind(turbo_stream_listener_t *l, const struct sockaddr *a) {
    stream_epoll_state_t *st;
    int r = epoll_init_state(&st, l, l->ctx, 1);
    if (r != 0) return r;
    r = epoll_reactor_ensure();
    if (r != 0) { epoll_destroy_state(st); return r; }
    st->fd = socket(a->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (st->fd < 0) { epoll_destroy_state(st); return -errno; }
    int reuse = 1; setsockopt(st->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (l->reuse_port) {
#ifdef SO_REUSEPORT
        if (setsockopt(st->fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
            int rc = -errno;
            epoll_destroy_state(st);
            return rc;
        }
#else
        epoll_destroy_state(st);
        return TURBO_ENOTSUP;
#endif
    }
    socklen_t addr_len = (a->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : 
                         (a->sa_family == AF_UNIX)  ? sizeof(struct sockaddr_un) : 
                                                      sizeof(struct sockaddr_in);
    if (bind(st->fd, a, addr_len) < 0) { int rc = -errno; epoll_destroy_state(st); return rc; }
    l->backend_data = st;
    return 0;
}

static int epoll_bind_pipe(turbo_stream_listener_t *l, const char *n) {
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; strncpy(addr.sun_path, n, sizeof(addr.sun_path)-1);
    unlink(n); return epoll_bind(l, (struct sockaddr *)&addr);
}

static int epoll_listen(turbo_stream_listener_t *l, int b) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)l->backend_data;
    int rc;
    if (listen(st->fd, b) < 0) {
        rc = -errno;
        l->backend_data = NULL;
        epoll_destroy_state(st);
        return rc;
    }
    rc = epoll_register_state(st, EPOLLIN);
    if (rc != 0) {
        l->backend_data = NULL;
        epoll_shutdown_state(st);
        return rc;
    }
    return 0;
}

static void epoll_listener_close(turbo_stream_listener_t *l) {
    stream_epoll_state_t *st = l ? (stream_epoll_state_t *)l->backend_data : NULL;
    if (!l) return;
    if (!st) {
        turbo_stream_listener_finalize_close(l);
        return;
    }

    TLOG_DEBUG("epoll[{:p}] listener-close listener={:p} fd={:d}", (void *)st, (void *)l,
               st->fd);

    epoll_shutdown_state(st);
}

const turbo_stream_backend_ops_t turbo_stream_epoll_ops = {
    .init = epoll_init, .connect = epoll_connect, .connect_pipe = epoll_connect_pipe,
    .send = epoll_send, .flush = NULL, .recv_start = epoll_recv_start, .recv_stop = epoll_recv_stop,
    .close = epoll_close, .bind = epoll_bind, .bind_pipe = epoll_bind_pipe, .listen = epoll_listen,
    .listener_close = epoll_listener_close
};

#endif
