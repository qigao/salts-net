/**
 * @file fmt.h
 * @brief Type-safe formatting using C11 _Generic
 */

#ifndef FMT_H
#define FMT_H

#include "platform.h"
#include "fmt_lexer.h"
#include "turbo_str.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ENUM_NAME(x) #x

#ifdef __cplusplus
#include <type_traits>
extern "C" {
#endif

/* ============================================================================
 * Type Tags
 * ============================================================================ */

typedef enum {
  FMT_TYPE_NONE = 0,
  FMT_TYPE_CHAR,
  FMT_TYPE_INT,
  FMT_TYPE_UINT,
  FMT_TYPE_LONG,
  FMT_TYPE_ULONG,
  FMT_TYPE_LLONG,
  FMT_TYPE_ULLONG,
  FMT_TYPE_DOUBLE,
  FMT_TYPE_STR,
  FMT_TYPE_PTR,
  FMT_TYPE_SIZE,
  FMT_TYPE_BOOL,
  FMT_TYPE_STRV
} fmt_type_t;

/* ============================================================================
 * Tagged Argument Structure
 * ============================================================================ */

typedef struct {
  fmt_type_t type;
  union {
    char c;
    int i;
    unsigned int u;
    long l;
    unsigned long ul;
    long long ll;
    unsigned long long ull;
    double f;
    const char *s;
    const void *p;
    size_t sz;
    int b; /* bool stored as int */
    tstr_v sv;
  } val;
} fmt_arg_t;

/* ============================================================================
 * Type Detection Helpers
 * ============================================================================ */

#ifdef __cplusplus
  #define FMT_MAKE_ARG(t, m, v)                                                                    \
    fmt_arg_t arg;                                                                                 \
    arg.type = t;                                                                                  \
    arg.val.m = v;                                                                                 \
    return arg;
#else
  #define FMT_MAKE_ARG(t, m, v) return (fmt_arg_t){t, {.m = v}};
#endif

static inline fmt_arg_t fmt_arg_char(char x) { FMT_MAKE_ARG(FMT_TYPE_CHAR, c, x) }
static inline fmt_arg_t fmt_arg_int(int x) { FMT_MAKE_ARG(FMT_TYPE_INT, i, x) }
static inline fmt_arg_t fmt_arg_uint(unsigned int x) { FMT_MAKE_ARG(FMT_TYPE_UINT, u, x) }
static inline fmt_arg_t fmt_arg_long(long x) { FMT_MAKE_ARG(FMT_TYPE_LONG, l, x) }
static inline fmt_arg_t fmt_arg_ulong(unsigned long x) { FMT_MAKE_ARG(FMT_TYPE_ULONG, ul, x) }
static inline fmt_arg_t fmt_arg_llong(long long x) { FMT_MAKE_ARG(FMT_TYPE_LLONG, ll, x) }
static inline fmt_arg_t fmt_arg_ullong(unsigned long long x) {
  FMT_MAKE_ARG(FMT_TYPE_ULLONG, ull, x)
}
static inline fmt_arg_t fmt_arg_double(double x) { FMT_MAKE_ARG(FMT_TYPE_DOUBLE, f, x) }
static inline fmt_arg_t fmt_arg_str(const char *x) { FMT_MAKE_ARG(FMT_TYPE_STR, s, x) }
static inline fmt_arg_t fmt_arg_ptr(const void *x) { FMT_MAKE_ARG(FMT_TYPE_PTR, p, x) }
static inline fmt_arg_t fmt_arg_bool(int x) { FMT_MAKE_ARG(FMT_TYPE_BOOL, b, x) }
static inline fmt_arg_t fmt_arg_size(size_t x) { FMT_MAKE_ARG(FMT_TYPE_SIZE, sz, x) }
static inline fmt_arg_t fmt_arg_strv(tstr_v x) { FMT_MAKE_ARG(FMT_TYPE_STRV, sv, x) }

#undef FMT_MAKE_ARG

/* ============================================================================
 * Type Wrappers (C++ Overloads or C11 _Generic)
 * ============================================================================ */

#ifdef __cplusplus
} /* End extern "C" to allow C++ overloading */

/* C++ Overloads for automatic type detection */
static inline fmt_arg_t fmt_arg_detect(char x) { return fmt_arg_char(x); }
static inline fmt_arg_t fmt_arg_detect(signed char x) { return fmt_arg_char((char)x); }
static inline fmt_arg_t fmt_arg_detect(unsigned char x) { return fmt_arg_char((char)x); }
static inline fmt_arg_t fmt_arg_detect(short x) { return fmt_arg_int(x); }
static inline fmt_arg_t fmt_arg_detect(unsigned short x) { return fmt_arg_uint(x); }
static inline fmt_arg_t fmt_arg_detect(int x) { return fmt_arg_int(x); }
static inline fmt_arg_t fmt_arg_detect(unsigned int x) { return fmt_arg_uint(x); }
static inline fmt_arg_t fmt_arg_detect(long x) { return fmt_arg_long(x); }
static inline fmt_arg_t fmt_arg_detect(unsigned long x) { return fmt_arg_ulong(x); }
static inline fmt_arg_t fmt_arg_detect(long long x) { return fmt_arg_llong(x); }
static inline fmt_arg_t fmt_arg_detect(unsigned long long x) { return fmt_arg_ullong(x); }
/* size_t overload: only enabled when size_t is a distinct type from unsigned long/unsigned long long */
template <typename T = size_t>
static inline typename std::enable_if<
    !std::is_same<T, unsigned long>::value && !std::is_same<T, unsigned long long>::value,
    fmt_arg_t>::type
fmt_arg_detect(T x) { return fmt_arg_size(x); }
static inline fmt_arg_t fmt_arg_detect(float x) { return fmt_arg_double((double)x); }
static inline fmt_arg_t fmt_arg_detect(double x) { return fmt_arg_double(x); }
static inline fmt_arg_t fmt_arg_detect(bool x) { return fmt_arg_bool(x); }
static inline fmt_arg_t fmt_arg_detect(char *x) { return fmt_arg_str(x); }
static inline fmt_arg_t fmt_arg_detect(const char *x) { return fmt_arg_str(x); }
static inline fmt_arg_t fmt_arg_detect(void *x) { return fmt_arg_ptr(x); }
static inline fmt_arg_t fmt_arg_detect(const void *x) { return fmt_arg_ptr(x); }
static inline fmt_arg_t fmt_arg_detect(tstr_v x) { return fmt_arg_strv(x); }

/* Template for classes with c_str() member (e.g. std::string) */
template <typename T>
static inline auto fmt_arg_detect(const T &x) -> decltype(fmt_arg_str(x.c_str())) {
  return fmt_arg_str(x.c_str());
}

/* Template catches all other pointer types */
template <typename T> static inline fmt_arg_t fmt_arg_detect(T *x) {
  return fmt_arg_ptr((const void *)x);
}

/* Template for enum types: cast to underlying integer type */
template <typename T>
static inline typename std::enable_if<std::is_enum<T>::value, fmt_arg_t>::type
fmt_arg_detect(T x) {
  using U = typename std::underlying_type<T>::type;
  // Promote char/uchar sized enums to int so they print as numbers, not characters
  using P = typename std::conditional<(sizeof(U) == 1), int, U>::type;
  return fmt_arg_detect(static_cast<P>(x));
}

  #define FMT_ARG(x) fmt_arg_detect(x)

extern "C" { /* Re-open extern "C" */

#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define FMT_HAS_GENERIC 1

  #ifdef _MSC_VER
    /* MSVC-specific cascading _Generic */
    #define FMT_ARG(x)                                                                             \
      (_Generic((x),                                                                               \
           char *: fmt_arg_str,                                                                    \
           const char *: fmt_arg_str,                                                              \
           double: fmt_arg_double,                                                                 \
           float: fmt_arg_double,                                                                  \
           void *: fmt_arg_ptr,                                                                    \
           const void *: fmt_arg_ptr,                                                              \
           tstr_v: fmt_arg_strv,                                                                   \
           char: fmt_arg_char,                                                                     \
           int: fmt_arg_int,                                                                       \
           unsigned int: fmt_arg_uint,                                                             \
           long long: fmt_arg_llong,                                                               \
           unsigned long long: fmt_arg_ullong,                                                     \
           default: _Generic((x),                                                                  \
               signed char: fmt_arg_char,                                                          \
               unsigned char: fmt_arg_char,                                                        \
               short: fmt_arg_int,                                                                 \
               unsigned short: fmt_arg_uint,                                                       \
               long: fmt_arg_long,                                                                 \
               unsigned long: fmt_arg_ulong,                                                       \
               default: fmt_arg_ptr))(x))
  #else
    /* Standard C11 _Generic */
    #define FMT_ARG(x)                                                                             \
      _Generic((x),                                                                                \
          char: fmt_arg_char,                                                                      \
          signed char: fmt_arg_char,                                                               \
          unsigned char: fmt_arg_char,                                                             \
          short: fmt_arg_int,                                                                      \
          unsigned short: fmt_arg_uint,                                                            \
          int: fmt_arg_int,                                                                        \
          unsigned int: fmt_arg_uint,                                                              \
          long: fmt_arg_long,                                                                      \
          unsigned long: fmt_arg_ulong,                                                            \
          long long: fmt_arg_llong,                                                                \
          unsigned long long: fmt_arg_ullong,                                                      \
          float: fmt_arg_double,                                                                   \
          double: fmt_arg_double,                                                                  \
          char *: fmt_arg_str,                                                                     \
          const char *: fmt_arg_str,                                                               \
          void *: fmt_arg_ptr,                                                                     \
          const void *: fmt_arg_ptr,                                                               \
          tstr_v: fmt_arg_strv,                                                                    \
          _Bool: fmt_arg_bool,                                                                     \
          default: fmt_arg_ptr)(x)
  #endif
#else
  #define FMT_HAS_GENERIC 0
  /* Fallback: use uintptr_t cast for absolute safety on pointer/long conversion */
  #define FMT_ARG(x) fmt_arg_llong((long long)(uintptr_t)(x))
#endif

/* ============================================================================
 * Argument Count Macros (for variadic)
 * ============================================================================ */

/* Use a helper to force macro expansion on MSVC */
#define FMT_EXPAND(x) x

/* Count arguments (up to 8). MSVC compatible 0-arg detection. */
#define FMT_NARGS_IMPL(_0, _1, _2, _3, _4, _5, _6, _7, _8, N, ...) N
#define FMT_NARGS(...)                                                                             \
  FMT_EXPAND(FMT_NARGS_IMPL(0, ##__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0))

/* Expand each argument with FMT_ARG */
#define FMT_WRAP_0() {FMT_TYPE_NONE}
#define FMT_WRAP_1(a) FMT_ARG(a)
#define FMT_WRAP_2(a, b) FMT_ARG(a), FMT_ARG(b)
#define FMT_WRAP_3(a, b, c) FMT_ARG(a), FMT_ARG(b), FMT_ARG(c)
#define FMT_WRAP_4(a, b, c, d) FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d)
#define FMT_WRAP_5(a, b, c, d, e) FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d), FMT_ARG(e)
#define FMT_WRAP_6(a, b, c, d, e, f)                                                               \
  FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d), FMT_ARG(e), FMT_ARG(f)
#define FMT_WRAP_7(a, b, c, d, e, f, g)                                                            \
  FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d), FMT_ARG(e), FMT_ARG(f), FMT_ARG(g)
#define FMT_WRAP_8(a, b, c, d, e, f, g, h)                                                         \
  FMT_ARG(a), FMT_ARG(b), FMT_ARG(c), FMT_ARG(d), FMT_ARG(e), FMT_ARG(f), FMT_ARG(g), FMT_ARG(h)

/* Dispatch to correct wrapper based on count */
#define FMT_WRAP_N_INNER(N, ...) FMT_WRAP_##N(__VA_ARGS__)
#define FMT_WRAP_N(N, ...) FMT_WRAP_N_INNER(N, __VA_ARGS__)

/* Main wrapper: creates array of typed args. */
#define FMT_ARGS(...)                                                                              \
  (fmt_arg_t[]) { FMT_WRAP_N(FMT_NARGS(__VA_ARGS__), __VA_ARGS__) }

/* ============================================================================
 * Type-Safe Formatting Function
 * ============================================================================ */

/**
 * @brief Simple macro for buffer formatting using type-safe logic
 */
#define fmt(buf, size, fmt, ...)                                                                   \
  fmt_print((buf), (size), (fmt), FMT_ARGS(__VA_ARGS__), FMT_NARGS(__VA_ARGS__))

/**
 * @brief Format a string using typed arguments
 *
 * @param buf      Output buffer
 * @param size     Buffer size
 * @param fmt      Format string with {} placeholders
 * @param args     Array of typed arguments
 * @param arg_count Number of arguments
 * @return Number of characters written
 */
CXX_C_API int fmt_print(char *buf, size_t size, const char *fmt, const fmt_arg_t *args,
                        size_t arg_count);

/* ============================================================================
 * tstr_t Integration
 * ============================================================================ */

static inline tstr_t tstr_cat_typed_impl(tstr_t s, const char *format, const fmt_arg_t *args,
                                          size_t count) {
  char tmp[1024];
  int n = fmt_print(tmp, sizeof(tmp), format, args, count);
  if (n > 0)
    return tstr_cat_len(s, tmp, (size_t)n);
  return s;
}

#define tstr_cat_typed(s, format, ...)                                                             \
  tstr_cat_typed_impl((s), (format), FMT_ARGS(__VA_ARGS__), FMT_NARGS(__VA_ARGS__))

#ifdef __cplusplus
} /* End extern "C" */

/**
 * @brief C++ Helper for type-safe formatting
 */
template <typename... Args>
inline int fmt_cpp_wrapper(char *buf, size_t size, const char *fmt, const Args &...args) {
  const fmt_arg_t arg_array[] = {FMT_ARG(args)..., {FMT_TYPE_NONE}};
  return fmt_print(buf, size, fmt, arg_array, sizeof...(Args));
}

/* Override macro for C++ */
#undef fmt
#define fmt(buf, size, fmt, ...) fmt_cpp_wrapper((buf), (size), (fmt), ##__VA_ARGS__)

template <typename... Args>
inline tstr_t tstr_cat_typed_cpp(tstr_t s, const char *format, const Args &...args) {
  char tmp[1024];
  int n = fmt_cpp_wrapper(tmp, sizeof(tmp), format, args...);
  if (n > 0)
    return tstr_cat_len(s, tmp, (size_t)n);
  return s;
}

#undef tstr_cat_typed
#define tstr_cat_typed(s, format, ...) tstr_cat_typed_cpp((s), (format), ##__VA_ARGS__)
#endif

#endif /* FMT_H */

