#ifndef TURBONET_LB_API_H
#define TURBONET_LB_API_H

#ifndef TURBONET_LB_C_API
  #if !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
    #define TURBONET_LB_C_API __attribute__((visibility("default")))
  #else
    #define TURBONET_LB_C_API
  #endif
#endif

#endif /* TURBONET_LB_API_H */
