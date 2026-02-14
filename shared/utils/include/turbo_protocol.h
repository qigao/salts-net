#ifndef TURBO_PROTOCOL_H
#define TURBO_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Transport/protocol identifiers shared across client APIs */
typedef enum {
  TURBO_PROTOCOL_TLS = 1,
  TURBO_PROTOCOL_TCP = 2,
  TURBO_PROTOCOL_UDP = 3,
  TURBO_PROTOCOL_KCP = 4,
  TURBO_PROTOCOL_PIPE = 5,
  TURBO_PROTOCOL_WEBSOCKET = 6
} turbo_protocol_type_t;

#ifdef __cplusplus
}
#endif

#endif /* TURBO_PROTOCOL_H */
