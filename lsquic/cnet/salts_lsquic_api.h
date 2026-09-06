#ifndef SALTSNET_LSQUIC_API_H
#define SALTSNET_LSQUIC_API_H

#ifndef SALTSNET_LSQUIC_C_API
  #if defined(__GNUC__) && !defined(_WIN32)
    #define SALTSNET_LSQUIC_C_API __attribute__((visibility("default")))
  #else
    #define SALTSNET_LSQUIC_C_API
  #endif
#endif

#endif
