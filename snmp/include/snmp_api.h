#ifndef SALTSNET_SNMP_API_H
#define SALTSNET_SNMP_API_H

#ifndef SALTSNET_SNMP_C_API
  #if !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
    #define SALTSNET_SNMP_C_API __attribute__((visibility("default")))
  #else
    #define SALTSNET_SNMP_C_API
  #endif
#endif

#endif /* SALTSNET_SNMP_API_H */
