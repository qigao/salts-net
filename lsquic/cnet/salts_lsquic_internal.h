#ifndef SALTSNET_LSQUIC_INTERNAL_H
#define SALTSNET_LSQUIC_INTERNAL_H

#include "salts_lsquic.h"

SALTSNET_LSQUIC_C_API int salts_lsquic_packets_out(void *packets_out_ctx,
                                                   const struct lsquic_out_spec *out_spec,
                                                   unsigned packet_count);

#endif
