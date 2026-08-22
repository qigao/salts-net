#ifndef CORONET_API_H
#define CORONET_API_H

#ifndef CORONET_C_API
  #if !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
    #define CORONET_C_API __attribute__((visibility("default")))
  #else
    #define CORONET_C_API
  #endif
#endif

#endif /* CORONET_API_H */
