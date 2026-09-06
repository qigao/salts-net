#ifndef SALTSNET_ASN1_API_H
#define SALTSNET_ASN1_API_H

#ifndef SALTSNET_ASN1_C_API
  #if !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
    #define SALTSNET_ASN1_C_API __attribute__((visibility("default")))
  #else
    #define SALTSNET_ASN1_C_API
  #endif
#endif

#endif /* SALTSNET_ASN1_API_H */
