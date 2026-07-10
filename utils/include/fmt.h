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
#include <time.h>

#define ENUM_NAME(x) #x

#ifdef __cplusplus
  #include <chrono>
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
  FMT_TYPE_STRV,
  FMT_TYPE_TIME
} fmt_type_t;

/* ============================================================================
 * Tagged Argument Structure
 * ============================================================================ */

/**
 * @brief Public value type for one formatted argument.
 *
 * This struct is intentionally not opaque: fmt() builds stack-allocated argument
 * arrays through C11 _Generic/C++ overloads, and callers may also pass explicit
 * fmt_arg_t arrays to fmt_print(). Construct values through fmt_arg_* helpers so
 * new enum values or union members can be added without changing call sites.
 *
 * Strings and tstr_v values are non-owning views. The referenced storage must
 * remain valid until fmt_print() returns.
 */
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
    turbo_timeval_t tv;
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
static inline fmt_arg_t fmt_arg_timeval(turbo_timeval_t x) { FMT_MAKE_ARG(FMT_TYPE_TIME, tv, x) }

static inline fmt_arg_t fmt_arg_time(time_t x) {
#ifdef __cplusplus
  fmt_arg_t arg;
  arg.type = FMT_TYPE_TIME;
  arg.val.tv.tv_sec = (int64_t)x;
  arg.val.tv.tv_usec = 0;
  return arg;
#else
  return (fmt_arg_t){FMT_TYPE_TIME, {.tv = {(int64_t)x, 0}}};
#endif
}

#define FMT_TIME(t) fmt_arg_time(t)

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
/* size_t overload: only enabled when size_t is a distinct type from unsigned long/unsigned long
 * long */
template <typename T = size_t>
static inline
    typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, unsigned long>::value &&
                                !std::is_same<T, unsigned long long>::value,
                            fmt_arg_t>::type
    fmt_arg_detect(T x) {
  return fmt_arg_size(x);
}
static inline fmt_arg_t fmt_arg_detect(float x) { return fmt_arg_double((double)x); }
static inline fmt_arg_t fmt_arg_detect(double x) { return fmt_arg_double(x); }
static inline fmt_arg_t fmt_arg_detect(bool x) { return fmt_arg_bool(x); }
static inline fmt_arg_t fmt_arg_detect(char *x) { return fmt_arg_str(x); }
static inline fmt_arg_t fmt_arg_detect(const char *x) { return fmt_arg_str(x); }
static inline fmt_arg_t fmt_arg_detect(void *x) { return fmt_arg_ptr(x); }
static inline fmt_arg_t fmt_arg_detect(const void *x) { return fmt_arg_ptr(x); }
static inline fmt_arg_t fmt_arg_detect(tstr_v x) { return fmt_arg_strv(x); }
static inline fmt_arg_t fmt_arg_detect(turbo_timeval_t x) { return fmt_arg_timeval(x); }
static inline fmt_arg_t fmt_arg_detect(std::chrono::system_clock::time_point tp) {
  auto dur = tp.time_since_epoch();
  auto sec = std::chrono::duration_cast<std::chrono::seconds>(dur);
  auto usec = std::chrono::duration_cast<std::chrono::microseconds>(dur - sec);
  turbo_timeval_t tv;
  tv.tv_sec = (int64_t)sec.count();
  tv.tv_usec = (int32_t)usec.count();
  return fmt_arg_timeval(tv);
}

/* Template for classes with c_str() member (e.g. std::string) */
template <typename T>
static inline auto fmt_arg_detect(const T &x) -> decltype(fmt_arg_str(x.c_str())) {
  return fmt_arg_str(x.c_str());
}

/* Template for classes with data()+size() but no c_str() (e.g. std::string_view) */
namespace __fmt_detail {
  template <typename T, typename = void> struct has_c_str : std::false_type {};
  template <typename T>
  struct has_c_str<T, decltype(void(std::declval<T>().c_str()))> : std::true_type {};
} // namespace __fmt_detail
template <typename T>
static inline auto fmt_arg_detect(const T &x) ->
    typename std::enable_if<!__fmt_detail::has_c_str<T>::value,
                            decltype(x.data(), x.size(), fmt_arg_t{})>::type {
  tstr_v sv;
  sv.data = x.data();
  sv.len = x.size();
  return fmt_arg_strv(sv);
}

/* Template catches all other pointer types */
template <typename T> static inline fmt_arg_t fmt_arg_detect(T *x) {
  return fmt_arg_ptr((const void *)x);
}

/* Template for enum types: cast to underlying integer type */
template <typename T>
static inline typename std::enable_if<std::is_enum<T>::value, fmt_arg_t>::type fmt_arg_detect(T x) {
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
           turbo_timeval_t: fmt_arg_timeval,                                                       \
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
          turbo_timeval_t: fmt_arg_timeval,                                                        \
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
#define FMT_NARGS(...) FMT_EXPAND(FMT_NARGS_IMPL(0, ##__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0))

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
 * @return Number of characters written, or 0 for invalid input or backend failure.
 *
 * Error conditions: NULL buf, zero size, NULL fmt, or args == NULL with
 * arg_count > 0. On valid buf/size, failures leave buf as an empty string.
 */
CXX_C_API int fmt_print(char *buf, size_t size, const char *fmt, const fmt_arg_t *args,
                        size_t arg_count);

/**
 * @brief Append formatted content directly to tstr_t.
 *
 * This is the dynamic-string backend for fmt formatting. It avoids fixed-size
 * temporary buffers by growing the render buffer until fmt_print() can produce
 * the full output, then appends that output to s.
 *
 * @param s         Existing tstr_t, or NULL to create one.
 * @param fmt       Format string with {} placeholders.
 * @param args      Array of typed arguments.
 * @param arg_count Number of arguments.
 * @return Updated tstr_t. Callers must assign the return value.
 */
CXX_C_API tstr_t fmt_print_tstr(tstr_t s, const char *fmt, const fmt_arg_t *args, size_t arg_count);

/* ============================================================================
 * tstr_t Integration
 * ============================================================================ */

static inline tstr_t tstr_cat_typed_impl(tstr_t s, const char *format, const fmt_arg_t *args,
                                         size_t count) {
  return fmt_print_tstr(s, format, args, count);
}

#define tstr_cat_typed(s, format, ...)                                                             \
  tstr_cat_typed_impl((s), (format), FMT_ARGS(__VA_ARGS__), FMT_NARGS(__VA_ARGS__))

static inline tstr_t tstr_format_typed_impl(const char *format, const fmt_arg_t *args,
                                            size_t count) {
  return fmt_print_tstr(tstr_new(), format, args, count);
}

#define tstr_format(format, ...)                                                                   \
  tstr_format_typed_impl((format), FMT_ARGS(__VA_ARGS__), FMT_NARGS(__VA_ARGS__))

#define tstr_append_format(s, format, ...)                                                         \
  tstr_cat_typed_impl((s), (format), FMT_ARGS(__VA_ARGS__), FMT_NARGS(__VA_ARGS__))

#ifdef __cplusplus
} /* End extern "C" */

/* ============================================================================
 * Custom Formatter Support (ADL-based)
 * ============================================================================ */

/**
 * @brief Buffer for custom formatters - wraps char* with position tracking
 */
struct fmt_buffer_t {
  char *data;
  size_t size;
  size_t pos;

  fmt_buffer_t(char *d, size_t s) : data(d), size(s), pos(0) {}

  void write(const char *str, size_t len) {
    if (pos + len < size) {
      memcpy(data + pos, str, len);
      pos += len;
      data[pos] = '\0';
    }
  }

  void write(const char *str) { write(str, strlen(str)); }

  template <typename... Args> void print(const char *format, const Args &...args) {
    if (pos < size) {
      int n = fmt_cpp_wrapper(data + pos, size - pos, format, args...);
      if (n > 0) pos += (size_t)n;
    }
  }
};

/**
 * @brief SFINAE helper to detect ADL fmt_format(fmt_buffer_t&, const T&)
 */
namespace __fmt_detail {
  template <typename T, typename = void> struct has_adl_format : std::false_type {};

  template <typename T>
  struct has_adl_format<T, decltype(void(fmt_format(std::declval<fmt_buffer_t &>(),
                                                    std::declval<const T &>())))> : std::true_type {
  };
} // namespace __fmt_detail

/**
 * @brief fmt_arg_detect for types with ADL fmt_format()
 *
 * Usage: Define in same namespace as your type:
 *   void fmt_format(fmt_buffer_t& buf, const YourType& val) {
 *       buf.print("{}:{}", val.field1, val.field2);
 *   }
 */
template <typename T>
static inline auto fmt_arg_detect(const T &x) -> typename std::enable_if<
    __fmt_detail::has_adl_format<T>::value && !__fmt_detail::has_c_str<T>::value, fmt_arg_t>::type {
  thread_local char buf[512];
  fmt_buffer_t fb(buf, sizeof(buf));
  fmt_format(fb, x);
  tstr_v sv;
  sv.data = buf;
  sv.len = fb.pos;
  return fmt_arg_strv(sv);
}

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
  const fmt_arg_t arg_array[] = {FMT_ARG(args)..., {FMT_TYPE_NONE}};
  return fmt_print_tstr(s, format, arg_array, sizeof...(Args));
}

  #undef tstr_cat_typed
  #define tstr_cat_typed(s, format, ...) tstr_cat_typed_cpp((s), (format), ##__VA_ARGS__)

template <typename... Args>
inline tstr_t tstr_format_typed_cpp(const char *format, const Args &...args) {
  const fmt_arg_t arg_array[] = {FMT_ARG(args)..., {FMT_TYPE_NONE}};
  return fmt_print_tstr(tstr_new(), format, arg_array, sizeof...(Args));
}

  #undef tstr_format
  #define tstr_format(format, ...) tstr_format_typed_cpp((format), ##__VA_ARGS__)

  #undef tstr_append_format
  #define tstr_append_format(s, format, ...) tstr_cat_typed_cpp((s), (format), ##__VA_ARGS__)
#endif

#endif /* FMT_H */
