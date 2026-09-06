#ifndef SALTSNET_TCP_PROXY_API_H
#define SALTSNET_TCP_PROXY_API_H

#ifndef SALTSNET_TCP_PROXY_C_API
  #if defined(__GNUC__) && !defined(_WIN32)
    #define SALTSNET_TCP_PROXY_C_API __attribute__((visibility("default")))
  #else
    #define SALTSNET_TCP_PROXY_C_API
  #endif
#endif

#endif
