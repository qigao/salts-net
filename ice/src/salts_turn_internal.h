#ifndef SALTSNET_TURN_INTERNAL_H
#define SALTSNET_TURN_INTERNAL_H
#include "ice/salts_turn.h"
/* Private checked cleanup for the enclosing ICE Owner. A nonzero result keeps
 * the client and transport alive; only success consumes the pointer. */
int turn_client_destroy_checked(salts_turn_client_t *client);
#endif
