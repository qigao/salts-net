#ifndef __ENDIAN_H__
#define __ENDIAN_H__

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
  #include <endian.h>
#elif defined(__APPLE__)
  #include <libkern/OSByteOrder.h>
  #ifndef be16toh
    #define be16toh(x) OSSwapBigToHostInt16(x)
  #endif
  #ifndef htobe16
    #define htobe16(x) OSSwapHostToBigInt16(x)
  #endif
  #ifndef le16toh
    #define le16toh(x) OSSwapLittleToHostInt16(x)
  #endif
  #ifndef htole16
    #define htole16(x) OSSwapHostToLittleInt16(x)
  #endif
  #ifndef be32toh
    #define be32toh(x) OSSwapBigToHostInt32(x)
  #endif
  #ifndef htobe32
    #define htobe32(x) OSSwapHostToBigInt32(x)
  #endif
  #ifndef le32toh
    #define le32toh(x) OSSwapLittleToHostInt32(x)
  #endif
  #ifndef htole32
    #define htole32(x) OSSwapHostToLittleInt32(x)
  #endif
  #ifndef be64toh
    #define be64toh(x) OSSwapBigToHostInt64(x)
  #endif
  #ifndef htobe64
    #define htobe64(x) OSSwapHostToBigInt64(x)
  #endif
  #ifndef le64toh
    #define le64toh(x) OSSwapLittleToHostInt64(x)
  #endif
  #ifndef htole64
    #define htole64(x) OSSwapHostToLittleInt64(x)
  #endif
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  #include <sys/endian.h>
#elif defined(_MSC_VER)
  #ifndef be16toh
    #define be16toh(x) _byteswap_ushort(x)
  #endif
  #ifndef htobe16
    #define htobe16(x) _byteswap_ushort(x)
  #endif
  #ifndef le16toh
    #define le16toh(x) (x)
  #endif
  #ifndef htole16
    #define htole16(x) (x)
  #endif
  #ifndef be32toh
    #define be32toh(x) _byteswap_ulong(x)
  #endif
  #ifndef htobe32
    #define htobe32(x) _byteswap_ulong(x)
  #endif
  #ifndef le32toh
    #define le32toh(x) (x)
  #endif
  #ifndef htole32
    #define htole32(x) (x)
  #endif
  #ifndef be64toh
    #define be64toh(x) _byteswap_uint64(x)
  #endif
  #ifndef htobe64
    #define htobe64(x) _byteswap_uint64(x)
  #endif
  #ifndef le64toh
    #define le64toh(x) (x)
  #endif
  #ifndef htole64
    #define htole64(x) (x)
  #endif
#else
  #include <endian.h>
#endif

#endif // __ENDIAN_H__