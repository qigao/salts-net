#ifndef TURBONET_ASN1_API_H
#define TURBONET_ASN1_API_H

#ifndef TURBONET_ASN1_C_API
  #if !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
    #define TURBONET_ASN1_C_API __attribute__((visibility("default")))
  #else
    #define TURBONET_ASN1_C_API
  #endif
#endif

#endif /* TURBONET_ASN1_API_H */
