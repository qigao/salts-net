/**
 * @file turbo_stream_epoll.c
 * @brief High-performance Linux epoll backend for turbo_stream_t.
 *
 * Implements the "one worker thread per handle" pattern with SPSC rings
 * for data and event notifications to eliminate heap churn and locks.
 * Supports scatter-gather oriented buffering.
 */

#ifdef __linux__

#include "turbo_stream_internal.h"
#include "CoroNet/turbo_coro_context.h"
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

typedef struct {
    void *owner;         /* turbo_stream_t or turbo_stream_listener_t */
    coro_context_t *ctx;
    int epoll_fd;
    int fd;
    int wake_fd;
    turbo_thread_t worker_thread;
    int connected;
    int send_pending;
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
} stream_epoll_state_t;

/* ── Helpers ─────────────────────────────────────────────── */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void post_event(stream_epoll_state_t *st, ep_op_kind_t kind, int status,
                       void *extra, const struct sockaddr *peer_addr,
                       socklen_t peer_addr_len);
static void epoll_post_wait(stream_epoll_state_t *st, coro_post_fn fn, void *arg1, void *arg2);
static void on_epoll_event_bounce(void *arg1, void *arg2);
static void handle_read_event(stream_epoll_state_t *st, stream_epoll_event_t *ev);
static void handle_write_event(stream_epoll_state_t *st, stream_epoll_event_t *ev);
static void flush_write_ring(stream_epoll_state_t *st);
static void epoll_destroy_state(stream_epoll_state_t *st);
static void epoll_shutdown_state(stream_epoll_state_t *st);
static void epoll_stream_cleanup_task(void *arg1, void *arg2);
static void epoll_listener_cleanup_task(void *arg1, void *arg2);

static uint8_t *wait_ring_write(stream_epoll_state_t *st, ring_spsc_t *ring, size_t size) {
    uint8_t *ptr;

    for (;;) {
        ptr = ring_spsc_write_acquire(ring, size);
        if (ptr) {
            return ptr;
        }

        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        if (st->wake_fd >= 0) {
            uint64_t val = 1;
            write(st->wake_fd, &val, sizeof(val));
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

static void post_event(stream_epoll_state_t *st, ep_op_kind_t kind, int status,
                       void *extra, const struct sockaddr *peer_addr,
                       socklen_t peer_addr_len) {
    uint8_t *ptr;

    for (;;) {
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
            break;
        }

        turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
        turbo_thread_yield();
    }

    epoll_post_wait(st, on_epoll_event_bounce, st, NULL);
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

    if (data && bytes > 0) {
        if (s->on_recv) {
            mem_slice_t slice = { .data = (char *)data, .length = bytes };
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

static void handle_write_event(stream_epoll_state_t *st, stream_epoll_event_t *ev) {
    turbo_stream_t *s = (turbo_stream_t *)st->owner;
    if (s->on_write_complete) {
        s->on_write_complete(s, ev->status);
    }
}

static void flush_write_ring(stream_epoll_state_t *st) {
    int wrote = 0;

    while (!st->stopping) {
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

        post_event(st, SEP_OP_WRITE, (sent < 0) ? -errno : TURBO_EOF, NULL, NULL, 0);
        return;
    }

    if (wrote) {
        post_event(st, SEP_OP_WRITE, 0, NULL, NULL, 0);
    }
}

static void on_epoll_event_bounce(void *arg1, void *arg2) {
    UNUSED(arg2);
    stream_epoll_state_t *st = (stream_epoll_state_t *)arg1;
    if (!st) return;

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
                    l->active_connections++;
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
}

/* ── Worker Thread ───────────────────────────────────────── */

static void* stream_epoll_worker(void* arg) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)arg;
    struct epoll_event events[8];
    uint8_t io_buf[8192];
    
    while (!st->stopping) {
        int nfds = epoll_wait(st->epoll_fd, events, 8, 100);
        if (st->stopping) break;
        
        /* Always check for write data after waking up, even if no network event */
        int should_flush = 0;
        if (nfds > 0) {
            for (int i = 0; i < nfds; i++) {
                if (events[i].data.fd == st->wake_fd) {
                    uint64_t val;
                    read(st->wake_fd, &val, sizeof(val));
                    should_flush = 1;
                } else if (st->is_listener) {
                    if (events[i].events & EPOLLIN) {
                        while (1) {
                            struct sockaddr_storage addr;
                            socklen_t addr_len = sizeof(addr);
                            int client_fd = accept4(st->fd, (struct sockaddr *)&addr, &addr_len,
                                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
                            if (client_fd < 0) break;
                            
                            turbo_stream_listener_t *l = (turbo_stream_listener_t *)st->owner;
                            turbo_stream_t *child = turbo_stream_create(l->ctx, l->kind);
                            if (child) {
                                extern int epoll_init_with_socket(turbo_stream_t *s, int existing);
                                if (epoll_init_with_socket(child, client_fd) == 0) {
                                    child->connected = 1;
                                    child->listener = l;
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
                    uint32_t evmask = events[i].events;

                    if (events[i].events & EPOLLOUT) {
                        if (!st->connected) {
                            int err = 0; socklen_t len = sizeof(err);
                            getsockopt(st->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                            st->connected = 1;
                            post_event(st, SEP_OP_CONNECT, (err == 0) ? 0 : -err, NULL, NULL, 0);
                        }
                        should_flush = 1;
                    }
                    if (events[i].events & EPOLLIN) {
                        ssize_t n = recv(st->fd, io_buf, sizeof(io_buf), 0);
                        if (n > 0) {
                            uint8_t *dest = wait_ring_write(st, &st->read_ring, (size_t)n);
                            memcpy(dest, io_buf, (size_t)n);
                            ring_spsc_write_release(&st->read_ring, (size_t)n);
                            post_event(st, SEP_OP_READ, 0, NULL, NULL, 0);
                        } else if (n == 0) {
                            post_event(st, SEP_OP_READ, TURBO_EOF, NULL, NULL, 0);
                        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            post_event(st, SEP_OP_READ, -errno, NULL, NULL, 0);
                        }
                    }
                    if ((evmask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) &&
                        !(evmask & EPOLLIN)) {
                        TLOG_DEBUG("epoll[{:p}] hup/err event fd={:d} mask=0x{:x}", (void *)st,
                                   st->fd, (unsigned int)evmask);
                        int err = 0;
                        socklen_t len = sizeof(err);
                        if (evmask & EPOLLERR) {
                            if (getsockopt(st->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
                                err == 0) {
                                err = EIO;
                            }
                            post_event(st, SEP_OP_READ, -err, NULL, NULL, 0);
                        } else {
                            post_event(st, SEP_OP_READ, TURBO_EOF, NULL, NULL, 0);
                        }
                    }
                }
            }
        }

        if (should_flush && !st->is_listener) {
            flush_write_ring(st);
        }
    }
    TLOG_DEBUG("epoll[{:p}] worker-exit listener={:d} fd={:d} wake_fd={:d}", (void *)st,
               st->is_listener, st->fd, st->wake_fd);
    return NULL;
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
    st->epoll_fd = -1;
    st->wake_fd = -1;
    st->is_listener = is_listener;

    rc = 0;
    st->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (st->epoll_fd < 0) {
        rc = -errno;
        goto fail;
    }

    st->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (st->wake_fd >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = st->wake_fd;
        if (epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->wake_fd, &ev) < 0) {
            rc = -errno;
            goto fail;
        }
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
    epoll_destroy_state(st);
    return rc;
}

static void epoll_destroy_state(stream_epoll_state_t *st) {
    if (!st) return;
    if (st->epoll_fd >= 0) {
        close(st->epoll_fd);
        st->epoll_fd = -1;
    }
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }
    if (st->wake_fd >= 0) {
        close(st->wake_fd);
        st->wake_fd = -1;
    }
    free(st->event_buf);
    free(st->read_buf);
    free(st->write_buf);
    free(st);
}

static void epoll_shutdown_state(stream_epoll_state_t *st) {
    if (!st) return;
    TLOG_DEBUG("epoll[{:p}] shutdown-begin fd={:d} wake_fd={:d} listener={:d}",
               (void *)st, st->fd, st->wake_fd, st->is_listener);
    st->stopping = 1;
    if (st->wake_fd >= 0) { uint64_t val = 1; write(st->wake_fd, &val, sizeof(val)); }
    if (st->worker_thread) turbo_thread_join(&st->worker_thread);
    TLOG_DEBUG("epoll[{:p}] shutdown-joined fd={:d} wake_fd={:d}", (void *)st, st->fd,
               st->wake_fd);
    if (st->epoll_fd >= 0) close(st->epoll_fd);
    if (st->fd >= 0) close(st->fd);
    if (st->wake_fd >= 0) close(st->wake_fd);
    st->epoll_fd = -1;
    st->fd = -1;
    st->wake_fd = -1;
    st->worker_thread = 0;
}

static void epoll_stream_cleanup_task(void *arg1, void *arg2) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)arg1;
    turbo_stream_t *s = (turbo_stream_t *)arg2;
    if (s) s->backend_data = NULL;
    epoll_destroy_state(st);
    if (s) turbo_stream_finalize_close(s);
}

static void epoll_listener_cleanup_task(void *arg1, void *arg2) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)arg1;
    turbo_stream_listener_t *l = (turbo_stream_listener_t *)arg2;
    if (l) l->backend_data = NULL;
    epoll_destroy_state(st);
    if (l) turbo_stream_listener_finalize_close(l);
}

static int epoll_init(turbo_stream_t *s) {
    return epoll_init_state((stream_epoll_state_t **)&s->backend_data, s, s->ctx, 0);
}

int epoll_init_with_socket(turbo_stream_t *s, int existing) {
    int r = epoll_init(s);
    if (r != 0) return r;
    stream_epoll_state_t *st = (stream_epoll_state_t *)s->backend_data;
    st->fd = existing;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.fd = st->fd;
    epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->fd, &ev);
    st->connected = 1;
    r = turbo_thread_create(&st->worker_thread, (turbo_thread_cb)stream_epoll_worker, st);
    if (r != 0) {
        close(st->fd);
        st->fd = -1;
    }
    return r;
}

static int epoll_connect(turbo_stream_t *s, const struct sockaddr *a) {
    stream_epoll_state_t *st = (stream_epoll_state_t *)s->backend_data;
    int connect_rc;
    int thread_rc;

    st->fd = socket(a->sa_family, SOCK_STREAM, 0);
    if (st->fd < 0) return -errno;
    set_nonblocking(st->fd);
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.fd = st->fd;
    epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->fd, &ev);
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

    thread_rc = turbo_thread_create(&st->worker_thread, (turbo_thread_cb)stream_epoll_worker, st);
    if (thread_rc != 0) {
        close(st->fd);
        st->fd = -1;
        return thread_rc;
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
    stream_epoll_state_t *st = (stream_epoll_state_t *)s->backend_data;
    uint8_t *dest = wait_ring_write(st, &st->write_ring, l);
    memcpy(dest, d, l);
    ring_spsc_write_release(&st->write_ring, l);
    
    /* Wake worker to flush */
    if (st->wake_fd >= 0) {
        uint64_t val = 1;
        write(st->wake_fd, &val, sizeof(val));
    }
    return 0;
}

static int epoll_recv_start(turbo_stream_t *s) { (void)s; return 0; }
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
    epoll_post_wait(st, epoll_stream_cleanup_task, st, s);
}

static int epoll_bind(turbo_stream_listener_t *l, const struct sockaddr *a) {
    stream_epoll_state_t *st;
    int r = epoll_init_state(&st, l, l->ctx, 1);
    if (r != 0) return r;
    st->fd = socket(a->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (st->fd < 0) { epoll_shutdown_state(st); epoll_destroy_state(st); return -errno; }
    int reuse = 1; setsockopt(st->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    socklen_t addr_len = (a->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : 
                         (a->sa_family == AF_UNIX)  ? sizeof(struct sockaddr_un) : 
                                                      sizeof(struct sockaddr_in);
    if (bind(st->fd, a, addr_len) < 0) { epoll_shutdown_state(st); epoll_destroy_state(st); return -errno; }
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
        epoll_shutdown_state(st);
        epoll_destroy_state(st);
        return rc;
    }
    struct epoll_event ev; ev.events = EPOLLIN | EPOLLET; ev.data.fd = st->fd;
    if (epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->fd, &ev) < 0) {
        rc = -errno;
        l->backend_data = NULL;
        epoll_shutdown_state(st);
        epoll_destroy_state(st);
        return rc;
    }
    rc = turbo_thread_create(&st->worker_thread, (turbo_thread_cb)stream_epoll_worker, st);
    if (rc != 0) {
        l->backend_data = NULL;
        epoll_shutdown_state(st);
        epoll_destroy_state(st);
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
    epoll_post_wait(st, epoll_listener_cleanup_task, st, l);
}

const turbo_stream_backend_ops_t turbo_stream_epoll_ops = {
    .init = epoll_init, .connect = epoll_connect, .connect_pipe = epoll_connect_pipe,
    .send = epoll_send, .flush = NULL, .recv_start = epoll_recv_start, .recv_stop = epoll_recv_stop,
    .close = epoll_close, .bind = epoll_bind, .bind_pipe = epoll_bind_pipe, .listen = epoll_listen,
    .listener_close = epoll_listener_close
};

#endif
