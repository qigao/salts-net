#ifndef TURBO_ENDIAN_H
#define TURBO_ENDIAN_H

#include <stddef.h>
#include <stdint.h>

/* 
 * Detect byte order using compiler-defined macros (GCC/Clang)
 */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
  #define TURBO_IS_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#elif defined(_WIN32) || defined(_MSC_VER)
  #define TURBO_IS_LITTLE_ENDIAN 1
#elif defined(__LITTLE_ENDIAN__)
  #define TURBO_IS_LITTLE_ENDIAN 1
#elif defined(__BIG_ENDIAN__)
  #define TURBO_IS_LITTLE_ENDIAN 0
#else
  /* Default to little-endian as it's most common today */
  #define TURBO_IS_LITTLE_ENDIAN 1
#endif

/* 
 * Byte swap intrinsics
 */
#if defined(_MSC_VER) && !defined(__clang__)
  #include <intrin.h>
  #define TURBO_BSWAP16(x) _byteswap_ushort(x)
  #define TURBO_BSWAP32(x) _byteswap_ulong(x)
  #define TURBO_BSWAP64(x) _byteswap_uint64(x)
#else
  #define TURBO_BSWAP16(x) __builtin_bswap16(x)
  #define TURBO_BSWAP32(x) __builtin_bswap32(x)
  #define TURBO_BSWAP64(x) __builtin_bswap64(x)
#endif

/* 
 * Conversion functions - defined as macros to ensure expansion and avoid linking issues
 */

/* Little Endian <-> Host */
#if TURBO_IS_LITTLE_ENDIAN
  #define turbo_le16toh(x) (uint16_t)(x)
  #define turbo_le32toh(x) (uint32_t)(x)
  #define turbo_le64toh(x) (uint64_t)(x)
  #define turbo_htole16(x) (uint16_t)(x)
  #define turbo_htole32(x) (uint32_t)(x)
  #define turbo_htole64(x) (uint64_t)(x)
#else
  #define turbo_le16toh(x) TURBO_BSWAP16((uint16_t)(x))
  #define turbo_le32toh(x) TURBO_BSWAP32((uint32_t)(x))
  #define turbo_le64toh(x) TURBO_BSWAP64((uint64_t)(x))
  #define turbo_htole16(x) TURBO_BSWAP16((uint16_t)(x))
  #define turbo_htole32(x) TURBO_BSWAP32((uint32_t)(x))
  #define turbo_htole64(x) TURBO_BSWAP64((uint64_t)(x))
#endif

/* Big Endian <-> Host */
#if TURBO_IS_LITTLE_ENDIAN
  #define turbo_be16toh(x) TURBO_BSWAP16((uint16_t)(x))
  #define turbo_be32toh(x) TURBO_BSWAP32((uint32_t)(x))
  #define turbo_be64toh(x) TURBO_BSWAP64((uint64_t)(x))
  #define turbo_htobe16(x) TURBO_BSWAP16((uint16_t)(x))
  #define turbo_htobe32(x) TURBO_BSWAP32((uint32_t)(x))
  #define turbo_htobe64(x) TURBO_BSWAP64((uint64_t)(x))
#else
  #define turbo_be16toh(x) (uint16_t)(x)
  #define turbo_be32toh(x) (uint32_t)(x)
  #define turbo_be64toh(x) (uint64_t)(x)
  #define turbo_htobe16(x) (uint16_t)(x)
  #define turbo_htobe32(x) (uint32_t)(x)
  #define turbo_htobe64(x) (uint64_t)(x)
#endif

/* Compatibility mapping to standard names (le32toh, htole32, etc.) */

#ifndef le16toh
#define le16toh(x) turbo_le16toh(x)
#endif
#ifndef le32toh
#define le32toh(x) turbo_le32toh(x)
#endif
#ifndef le64toh
#define le64toh(x) turbo_le64toh(x)
#endif

#ifndef htole16
#define htole16(x) turbo_htole16(x)
#endif
#ifndef htole32
#define htole32(x) turbo_htole32(x)
#endif
#ifndef htole64
#define htole64(x) turbo_htole64(x)
#endif

#ifndef be16toh
#define be16toh(x) turbo_be16toh(x)
#endif
#ifndef be32toh
#define be32toh(x) turbo_be32toh(x)
#endif
#ifndef be64toh
#define be64toh(x) turbo_be64toh(x)
#endif

#ifndef htobe16
#define htobe16(x) turbo_htobe16(x)
#endif
#ifndef htobe32
#define htobe32(x) turbo_htobe32(x)
#endif
#ifndef htobe64
#define htobe64(x) turbo_htobe64(x)
#endif

#endif /* TURBO_ENDIAN_H */
