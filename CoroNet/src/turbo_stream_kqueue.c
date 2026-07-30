/**
 * @file turbo_stream_kqueue.c
 * @brief High-performance macOS/BSD kqueue backend for turbo_stream_t.
 *
 * Implements the "one worker thread per handle" pattern with SPSC rings
 * for data and event notifications to eliminate heap churn and locks.
 * Supports scatter-gather oriented buffering.
 */

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include "turbo_stream_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "turbo_buffer.h"
#include "turbo_thread.h"
#include "tlog.h"
#include "ring_buffer_spsc.h"

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

/* ── Constants ───────────────────────────────────────────── */

#define EVENT_RING_BYTES (64 * 1024)
#define DATA_RING_SIZE   (64 * 1024)

typedef enum {
    SEP_OP_NONE,
    SEP_OP_CONNECT,
    SEP_OP_ACCEPT,
    SEP_OP_READ,
    SEP_OP_WRITE,
    SEP_OP_CLOSE,
    SEP_OP_ERROR
} ep_op_kind_t;

typedef struct {
    int kind;
    int status;
    void *extra;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
} stream_kqueue_event_t;

typedef struct {
    void *owner;         /* turbo_stream_t or turbo_stream_listener_t */
    coro_context_t *ctx;
    int kq_fd;
    int fd;
    turbo_thread_t worker_thread;
    int connected;
    int is_listener;
    volatile int stopping;
    
    /* Event Notification Ring (SPSC from worker to main) */
    ring_spsc_t event_ring;
    uint8_t *event_buf;

    /* Data Rings (SPSC) */
    ring_spsc_t read_ring;
    uint8_t *read_buf;

    ring_spsc_t write_ring;
    uint8_t *write_buf;
} stream_kqueue_state_t;

/* ── Helpers ─────────────────────────────────────────────── */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void post_event(stream_kqueue_state_t *st, ep_op_kind_t kind, int status,
                       void *extra, const struct sockaddr *peer_addr,
                       socklen_t peer_addr_len);
static void on_kqueue_event_bounce(void *arg1, void *arg2);
static void handle_read_event(stream_kqueue_state_t *st, stream_kqueue_event_t *ev);
static void handle_write_event(stream_kqueue_state_t *st, stream_kqueue_event_t *ev);
static void flush_write_ring(stream_kqueue_state_t *st);
static void kqueue_cleanup_state(stream_kqueue_state_t *st);

static uint8_t *wait_ring_write(stream_kqueue_state_t *st, ring_spsc_t *ring, size_t size) {
    uint8_t *ptr;

    for (;;) {
        ptr = ring_spsc_write_acquire(ring, size);
        if (ptr) {
            return ptr;
        }

        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        turbo_thread_yield();
    }
}

static void post_event(stream_kqueue_state_t *st, ep_op_kind_t kind, int status,
                       void *extra, const struct sockaddr *peer_addr,
                       socklen_t peer_addr_len) {
    uint8_t *ptr;

    for (;;) {
        ptr = ring_spsc_write_acquire(&st->event_ring, sizeof(stream_kqueue_event_t));
        if (ptr) {
            stream_kqueue_event_t *ev = (stream_kqueue_event_t *)ptr;
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
            ring_spsc_write_release(&st->event_ring, sizeof(stream_kqueue_event_t));
            break;
        }

        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        turbo_thread_yield();
    }

    while (coro_post(st->ctx, on_kqueue_event_bounce, st, NULL) != 0) {
        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        turbo_thread_yield();
    }
}

static void handle_read_event(stream_kqueue_state_t *st, stream_kqueue_event_t *ev) {
    turbo_stream_t *s = (turbo_stream_t *)st->owner;
    size_t bytes = 0;
    uint8_t *data = ring_spsc_read_acquire(&st->read_ring, &bytes);
    int close_requested = 0;

    if (data && bytes > 0) {
        if (s->on_recv) {
            mem_slice_t slice = { .data = (char *)data, .length = bytes, .buffer = NULL };
            close_requested = s->on_recv(s, &slice, NULL);
        }
        ring_spsc_read_release(&st->read_ring, bytes);
    } else if (ev->status != 0 && s->on_recv) {
        s->on_recv(s, NULL, NULL);
    }

    if ((ev->status != 0 || close_requested) && !s->closing) {
        turbo_stream_close(s);
    }
}

static void handle_write_event(stream_kqueue_state_t *st, stream_kqueue_event_t *ev) {
    turbo_stream_t *s = (turbo_stream_t *)st->owner;
    if (s->on_write_complete) {
        s->on_write_complete(s, ev->status);
    }
}

static void flush_write_ring(stream_kqueue_state_t *st) {
    int wrote = 0;

    while (!st->stopping) {
        size_t avail = 0;
        uint8_t *data = ring_spsc_read_acquire(&st->write_ring, &avail);
        if (!data || avail == 0) {
            break;
        }

        ssize_t sent = send(st->fd, data, avail, 0);
        if (sent > 0) {
            ring_spsc_read_release(&st->write_ring, (size_t)sent);
            wrote = 1;
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        }

        post_event(st, SEP_OP_WRITE, (sent < 0) ? -errno : TURBO_EOF, NULL, NULL, 0);
        return;
    }

    if (wrote) {
        post_event(st, SEP_OP_WRITE, 0, NULL, NULL, 0);
    }
}

static void on_kqueue_event_bounce(void *arg1, void *arg2) {
    UNUSED(arg2);
    stream_kqueue_state_t *st = (stream_kqueue_state_t *)arg1;
    if (!st) return;

    size_t available = 0;
    while ((available = ring_spsc_read_available(&st->event_ring)) >= sizeof(stream_kqueue_event_t)) {
        size_t chunk_avail = 0;
        uint8_t *ptr = ring_spsc_read_acquire(&st->event_ring, &chunk_avail);
        if (!ptr) break;

        size_t processed = 0;
        while (processed + sizeof(stream_kqueue_event_t) <= chunk_avail) {
            stream_kqueue_event_t *ev = (stream_kqueue_event_t *)(ptr + processed);
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
                    s->connected = (ev->status == 0);
                    if (s->on_connect) s->on_connect(s, ev->status, NULL);
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
                        TLOG_ERROR("kqueue listener accept failed: {:d}", ev->status);
                        turbo_stream_listener_close(l);
                    }
                    break;
                }
                default: break;
            }
            processed += sizeof(stream_kqueue_event_t);
        }
        ring_spsc_read_release(&st->event_ring, processed);
    }
}

/* ── Worker Thread ───────────────────────────────────────── */

static void* stream_kqueue_worker(void* arg) {
    stream_kqueue_state_t *st = (stream_kqueue_state_t *)arg;
    struct kevent events[8];
    uint8_t io_buf[8192];
    
    while (!st->stopping) {
        int nfds = kevent(st->kq_fd, NULL, 0, events, 8, NULL);
        if (st->stopping) break;
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (st->is_listener) {
                if (events[i].filter == EVFILT_READ) {
                    while (1) {
                        struct sockaddr_storage addr;
                        socklen_t addr_len = sizeof(addr);
                        int client_fd = accept(st->fd, (struct sockaddr *)&addr, &addr_len);
                        if (client_fd < 0) break;
                        set_nonblocking(client_fd);
                        
                        turbo_stream_listener_t *l = (turbo_stream_listener_t *)st->owner;
                        turbo_stream_t *child = turbo_stream_create(l->ctx, l->kind);
                        if (child) {
                            extern int kqueue_init_with_socket(turbo_stream_t *s, int existing);
                            if (kqueue_init_with_socket(child, client_fd) == 0 &&
                                turbo_stream_listener_configure_child(l, child) == 0 &&
                                turbo_stream_apply_native_socket_options(child, client_fd) == 0) {
                                child->connected = 1;
                                child->listener = l;
                                l->active_connections++;
                                post_event(st, SEP_OP_ACCEPT, 0, child,
                                           (const struct sockaddr *)&addr, addr_len);
                            } else {
                                turbo_stream_destroy(child);
                                st->stopping = 1;
                                post_event(st, SEP_OP_ERROR, TURBO_ENOMEM, NULL, NULL, 0);
                                break;
                            }
                        } else {
                            close(client_fd);
                            st->stopping = 1;
                            post_event(st, SEP_OP_ERROR, TURBO_ENOMEM, NULL, NULL, 0);
                            break;
                        }
                    }
                }
            } else {
                if (events[i].filter == EVFILT_WRITE) {
                    if (!st->connected) {
                        int err = 0; socklen_t len = sizeof(err);
                        getsockopt(st->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                        st->connected = 1;
                        post_event(st, SEP_OP_CONNECT, (err == 0) ? 0 : -err, NULL, NULL, 0);
                    }
                    size_t avail = 0;
                    (void)avail;
                    flush_write_ring(st);
                }
                if (events[i].filter == EVFILT_READ) {
                    ssize_t n = recv(st->fd, io_buf, sizeof(io_buf), 0);
                    if (n > 0) {
                        uint8_t *dest = wait_ring_write(st, &st->read_ring, (size_t)n);
                        memcpy(dest, io_buf, (size_t)n);
                        ring_spsc_write_release(&st->read_ring, (size_t)n);
                        post_event(st, SEP_OP_READ, 0, NULL, NULL, 0);
                    } else if (n == 0) {
                        post_event(st, SEP_OP_READ, TURBO_EOF, NULL, NULL, 0);
                    }
                }
            }
        }
    }
    return NULL;
}

/* ── Backend Ops ─────────────────────────────────────────── */

static int kqueue_init_state(stream_kqueue_state_t **out, void *owner, coro_context_t *ctx, int is_listener) {
    stream_kqueue_state_t *st = NULL;
    int rc = 0;

    st = (stream_kqueue_state_t *)calloc(1, sizeof(stream_kqueue_state_t));
    if (!st) return TURBO_ENOMEM;

    st->owner = owner;
    st->ctx = ctx;
    st->fd = -1;
    st->kq_fd = -1;
    st->is_listener = is_listener;
    st->kq_fd = kqueue();
    if (st->kq_fd < 0) {
        rc = -errno;
        goto fail;
    }

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
    kqueue_cleanup_state(st);
    return rc;
}

static void kqueue_cleanup_state(stream_kqueue_state_t *st) {
    if (!st) return;
    st->stopping = 1;
    /* Wake up kqueue to exit worker */
    if (st->fd >= 0) {
        struct kevent ev; EV_SET(&ev, st->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(st->kq_fd, &ev, 1, NULL, 0, NULL);
    }
    if (st->worker_thread) turbo_thread_join(&st->worker_thread);
    if (st->kq_fd >= 0) close(st->kq_fd);
    if (st->fd >= 0) close(st->fd);
    free(st->event_buf); free(st->read_buf); free(st->write_buf);
    free(st);
}

static int kqueue_init(turbo_stream_t *s) {
    return kqueue_init_state((stream_kqueue_state_t **)&s->backend_data, s, s->ctx, 0);
}

int kqueue_init_with_socket(turbo_stream_t *s, int existing) {
    int r = kqueue_init(s);
    if (r != 0) return r;
    stream_kqueue_state_t *st = (stream_kqueue_state_t *)s->backend_data;
    st->fd = existing;
    struct kevent evs[2];
    EV_SET(&evs[0], st->fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    EV_SET(&evs[1], st->fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(st->kq_fd, evs, 2, NULL, 0, NULL);
    st->connected = 1;
    r = turbo_thread_create(&st->worker_thread, (turbo_thread_cb)stream_kqueue_worker, st);
    if (r != 0) {
        s->backend_data = NULL;
        kqueue_cleanup_state(st);
        return r;
    }
    return r;
}

static int kqueue_connect(turbo_stream_t *s, const struct sockaddr *a,
                          size_t addr_len) {
    stream_kqueue_state_t *st = (stream_kqueue_state_t *)s->backend_data;
    int rc;

    st->fd = socket(a->sa_family, SOCK_STREAM, 0);
    if (st->fd < 0) return -errno;
    set_nonblocking(st->fd);
    rc = turbo_stream_apply_native_socket_options(s, st->fd);
    if (rc != 0) {
        close(st->fd);
        st->fd = -1;
        return rc;
    }
    struct kevent evs[2];
    EV_SET(&evs[0], st->fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    EV_SET(&evs[1], st->fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(st->kq_fd, evs, 2, NULL, 0, NULL);
    if (connect(st->fd, a, (socklen_t)addr_len) < 0 && errno != EINPROGRESS) {
        int err = -errno;
        close(st->fd);
        st->fd = -1;
        return err;
    }
    rc = turbo_thread_create(&st->worker_thread, (turbo_thread_cb)stream_kqueue_worker, st);
    if (rc != 0) {
        close(st->fd);
        st->fd = -1;
        return rc;
    }
    return 0;
}

static int kqueue_connect_pipe(turbo_stream_t *s, const char *n) {
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; strncpy(addr.sun_path, n, sizeof(addr.sun_path)-1);
    return kqueue_connect(s, (struct sockaddr *)&addr, sizeof(addr));
}

static int kqueue_send(turbo_stream_t *s, const char *d, size_t l) {
    stream_kqueue_state_t *st = (stream_kqueue_state_t *)s->backend_data;
    int rc;
    if (!st || !d || l == 0) return TURBO_EINVAL;
    rc = turbo_stream_send_hwm_check(s, l, ring_spsc_read_available(&st->write_ring));
    if (rc != 0) return rc;
    uint8_t *dest = wait_ring_write(st, &st->write_ring, l);
    memcpy(dest, d, l);
    ring_spsc_write_release(&st->write_ring, l);
    return 0;
}

static void kqueue_close(turbo_stream_t *s) {
    kqueue_cleanup_state((stream_kqueue_state_t *)s->backend_data);
    s->backend_data = NULL;
}

static void kqueue_listener_close(turbo_stream_listener_t *l) {
    stream_kqueue_state_t *st;

    if (!l) {
        return;
    }

    st = (stream_kqueue_state_t *)l->backend_data;
    if (!st) {
        turbo_stream_listener_notify_backend_released(l);
        return;
    }

    kqueue_cleanup_state(st);
    turbo_stream_listener_notify_backend_released(l);
}

static int kqueue_bind(turbo_stream_listener_t *l, const struct sockaddr *a,
                       size_t addr_len) {
    stream_kqueue_state_t *st;
    int r = kqueue_init_state(&st, l, l->ctx, 1);
    if (r != 0) return r;
    st->fd = socket(a->sa_family, SOCK_STREAM, 0);
    if (st->fd < 0) { kqueue_cleanup_state(st); return -errno; }
    int reuse = 1; setsockopt(st->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (l->reuse_port) {
#ifdef SO_REUSEPORT
        if (setsockopt(st->fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
            int rc = -errno;
            kqueue_cleanup_state(st);
            return rc;
        }
#else
        kqueue_cleanup_state(st);
        return TURBO_ENOTSUP;
#endif
    }
    if (bind(st->fd, a, (socklen_t)addr_len) < 0) {
        kqueue_cleanup_state(st);
        return -errno;
    }
    l->backend_data = st;
    return 0;
}

static int kqueue_bind_pipe(turbo_stream_listener_t *l, const char *n) {
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; strncpy(addr.sun_path, n, sizeof(addr.sun_path)-1);
    unlink(n); return kqueue_bind(l, (struct sockaddr *)&addr, sizeof(addr));
}

static int kqueue_listen(turbo_stream_listener_t *l, int b) {
    stream_kqueue_state_t *st = (stream_kqueue_state_t *)l->backend_data;
    int rc;
    if (listen(st->fd, b) < 0) {
        rc = -errno;
        l->backend_data = NULL;
        kqueue_cleanup_state(st);
        return rc;
    }
    struct kevent ev; EV_SET(&ev, st->fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(st->kq_fd, &ev, 1, NULL, 0, NULL) < 0) {
        rc = -errno;
        l->backend_data = NULL;
        kqueue_cleanup_state(st);
        return rc;
    }
    rc = turbo_thread_create(&st->worker_thread, (turbo_thread_cb)stream_kqueue_worker, st);
    if (rc != 0) {
        l->backend_data = NULL;
        kqueue_cleanup_state(st);
        return rc;
    }
    return 0;
}

const turbo_stream_backend_ops_t turbo_stream_kqueue_ops = {
    .init = kqueue_init, .connect = kqueue_connect, .connect_pipe = kqueue_connect_pipe,
    .send = kqueue_send, .flush = NULL, .recv_start = (int(*)(turbo_stream_t*))NULL, .recv_stop = (void(*)(turbo_stream_t*))NULL,
    .close = kqueue_close, .bind = kqueue_bind, .bind_pipe = kqueue_bind_pipe, .listen = kqueue_listen,
    .listener_close = kqueue_listener_close
};

#endif
