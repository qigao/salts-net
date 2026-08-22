#ifndef TURBONET_TPROXY_API_H
#define TURBONET_TPROXY_API_H

#ifndef TURBONET_TPROXY_C_API
  #if !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
    #define TURBONET_TPROXY_C_API __attribute__((visibility("default")))
  #else
    #define TURBONET_TPROXY_C_API
  #endif
#endif

#endif /* TURBONET_TPROXY_API_H */
