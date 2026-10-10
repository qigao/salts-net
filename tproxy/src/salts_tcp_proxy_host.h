#ifndef SALTS_TCP_PROXY_HOST_INTERNAL_H
#define SALTS_TCP_PROXY_HOST_INTERNAL_H

#include "salts_tcp_proxy.h"
#include <cnet/handoff.h>
#include <cnet/sg_host.h>

/* Private protocol adapter. All calls belong to the final SG owner. */
int salts_proxy_host_config_valid(const salts_tcp_proxy_config_t *config);
int salts_proxy_host_create(const salts_tcp_proxy_config_t *config,
                            native_io_backend *backend, cnet_handoff *credits,
                            salts_tcp_proxy_t **out_proxy);
/* Consumes accepted and ticket on every attempt, including rejection. */
int salts_proxy_host_adopt(salts_tcp_proxy_t *proxy, cnet_accepted_stream *accepted,
                           cnet_handoff_ticket ticket);
int salts_proxy_host_progress(salts_tcp_proxy_t *proxy, cnet_listener *listener,
                              const native_io_sharded_completion *batch, size_t count,
                              size_t *out_work);
/* Retry after progress while EBUSY; only true terminal/recycle permits OK. */
int salts_proxy_host_stop(salts_tcp_proxy_t *proxy);

#endif
