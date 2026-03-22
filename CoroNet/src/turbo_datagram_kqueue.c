/**
 * @file turbo_datagram_kqueue.c
 * @brief High-performance macOS/BSD kqueue backend for turbo_datagram_t.
 *
 * Uses SPSC rings for event notifications to avoid heap churn.
 */

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include "turbo_datagram_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "ring_buffer_spsc.h"

#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    mem_slice_t slice;
    struct sockaddr_storage peer;
} dg_event_t;

typedef struct {
    turbo_datagram_t *owner;
    void *ctx;
    int kq_fd;
    int fd;
    turbo_thread_t worker_thread;
    volatile int stopping;
    
    ring_spsc_t event_ring;
    uint8_t *event_buf;
} dg_kqueue_state_t;

static void on_dg_bounce(void *arg1, void *arg2);

static void* dg_kqueue_worker(void* arg) {
    dg_kqueue_state_t *st = (dg_kqueue_state_t *)arg;
    struct kevent events[1];
    uint8_t buf[65536];
    
    while (!st->stopping) {
        int r = kevent(st->kq_fd, NULL, 0, events, 1, NULL);
        if (st->stopping) break;
        if (r > 0 && (events[0].filter == EVFILT_READ)) {
            struct sockaddr_storage peer;
            socklen_t addr_len = sizeof(peer);
            ssize_t n = recvfrom(st->fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &addr_len);
            if (n > 0) {
                uint8_t *ptr = ring_spsc_write_acquire(&st->event_ring, sizeof(dg_event_t) + (size_t)n);
                if (ptr) {
                    dg_event_t *ev = (dg_event_t *)ptr;
                    memcpy(&ev->peer, &peer, sizeof(peer));
                    ev->slice.data = (char *)(ptr + sizeof(dg_event_t));
                    ev->slice.length = (size_t)n;
                    memcpy(ev->slice.data, buf, (size_t)n);
                    ring_spsc_write_release(&st->event_ring, sizeof(dg_event_t) + (size_t)n);
                    coro_post((coro_context_t *)st->ctx, on_dg_bounce, st, NULL);
                }
            }
        }
    }
    return NULL;
}

static void on_dg_bounce(void *arg1, void *arg2) {
    UNUSED(arg2);
    dg_kqueue_state_t *st = (dg_kqueue_state_t *)arg1;
    size_t avail = 0;
    while ((avail = ring_spsc_read_available(&st->event_ring)) >= sizeof(dg_event_t)) {
        size_t chunk = 0;
        uint8_t *ptr = ring_spsc_read_acquire(&st->event_ring, &chunk);
        if (!ptr) break;
        dg_event_t *ev = (dg_event_t *)ptr;
        if (st->owner->on_recv) {
            st->owner->on_recv(st->owner, &ev->slice, &ev->peer);
        }
        ring_spsc_read_release(&st->event_ring, sizeof(dg_event_t) + ev->slice.length);
    }
}

static int kqueue_dg_init(turbo_datagram_t *d) {
    dg_kqueue_state_t *st = calloc(1, sizeof(dg_kqueue_state_t));
    if (!st) return TURBO_ENOMEM;
    st->owner = d;
    st->ctx = d->ctx;
    st->kq_fd = kqueue();
    
    size_t ring_bytes = 128 * 1024;
    st->event_buf = malloc(ring_bytes);
    ring_spsc_init(&st->event_ring, st->event_buf, ring_bytes);
    
    d->backend_data = st;
    return 0;
}

static int kqueue_dg_open(turbo_datagram_t *d, int family) {
    dg_kqueue_state_t *st = (dg_kqueue_state_t *)d->backend_data;
    st->fd = socket(family, SOCK_DGRAM, 0);
    if (st->fd < 0) return -errno;
    int flags = fcntl(st->fd, F_GETFL, 0);
    fcntl(st->fd, F_SETFL, flags | O_NONBLOCK);
    struct kevent ev; EV_SET(&ev, st->fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(st->kq_fd, &ev, 1, NULL, 0, NULL);
    return 0;
}

static int kqueue_dg_recv_start(turbo_datagram_t *d, turbo_datagram_recv_cb cb) {
    dg_kqueue_state_t *st = (dg_kqueue_state_t *)d->backend_data;
    d->on_recv = cb;
    return turbo_thread_create(&st->worker_thread, (turbo_thread_cb)dg_kqueue_worker, st);
}

static void kqueue_dg_close(turbo_datagram_t *d) {
    dg_kqueue_state_t *st = (dg_kqueue_state_t *)d->backend_data;
    if (!st) return;
    st->stopping = 1;
    /* Wake up kqueue */
    struct kevent ev; EV_SET(&ev, st->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(st->kq_fd, &ev, 1, NULL, 0, NULL);
    if (st->worker_thread) turbo_thread_join(&st->worker_thread);
    if (st->kq_fd >= 0) close(st->kq_fd);
    if (st->fd >= 0) close(st->fd);
    free(st->event_buf);
    free(st);
    d->backend_data = NULL;
}

const turbo_datagram_backend_ops_t turbo_datagram_kqueue_ops = {
    .init = kqueue_dg_init, .open = kqueue_dg_open, 
    .recv_start = kqueue_dg_recv_start, .close = kqueue_dg_close
};

#endif
