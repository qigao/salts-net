#ifndef SALTS_LB_HOST_INTERNAL_H
#define SALTS_LB_HOST_INTERNAL_H
#include "salts_lb.h"
#include <cnet/handoff.h>
#include <cnet/sg_host.h>

int salts_lb_host_config_valid(const salts_lb_config_t *);
int salts_lb_host_create(const salts_lb_config_t *, native_io_backend *, cnet_handoff *, salts_lb_t **);
/* Final-Owner calls only. adopt consumes the stream/ticket on every attempt. */
int salts_lb_host_adopt(salts_lb_t *, cnet_accepted_stream *, cnet_handoff_ticket, bool worker);
int salts_lb_host_progress(salts_lb_t *, cnet_listener *, const native_io_sharded_completion *, size_t, size_t *);
int salts_lb_host_stop(salts_lb_t *);
#endif
