/**
 * @file turbo_coro.h
 * @brief Compatibility include for the TurboNet coroutine primitive.
 */

#ifndef TURBO_CORONET_CORO_COMPAT_H
#define TURBO_CORONET_CORO_COMPAT_H

#if defined(__has_include)
#  if __has_include("../turbo_coro.h")
#    include "../turbo_coro.h"
#  else
#    include "../../../utils/include/turbo_coro.h"
#  endif
#else
#  include "../../../utils/include/turbo_coro.h"
#endif

#endif /* TURBO_CORONET_CORO_COMPAT_H */
