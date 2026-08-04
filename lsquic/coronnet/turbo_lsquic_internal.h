#ifndef TURBO_LSQUIC_INTERNAL_H
#define TURBO_LSQUIC_INTERNAL_H

#include "CoroNet/turbo_lsquic.h"

int turbo_lsquic_packets_out(void *packets_out_ctx,
                             const struct lsquic_out_spec *out_spec,
                             unsigned n_packets_out);

#endif /* TURBO_LSQUIC_INTERNAL_H */
