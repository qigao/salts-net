#include "turbo_str_view.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define TSTR_V_COPY_TO(v, alloc_call)     \
  do {                                    \
    char *out = (char *)(alloc_call);     \
    if (!out) return NULL;                \
    if ((v).len > 0)                      \
      memcpy(out, (v).data, (v).len);     \
    out[(v).len] = '\0';                  \
    return out;                           \
  } while (0)

static const unsigned char lower_table[256] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,
    16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 91,  92,  93,  94,  95,
    96,  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
};

int tstr_v_eq(tstr_v a, tstr_v b) {
  if (a.len != b.len)
    return 0;
  if (a.len == 0)
    return 1;
  return memcmp(a.data, b.data, a.len) == 0;
}

int tstr_v_ieq(tstr_v a, tstr_v b) {
  if (a.len != b.len)
    return 0;
  for (size_t i = 0; i < a.len; i++) {
    if (lower_table[(unsigned char)a.data[i]] != lower_table[(unsigned char)b.data[i]])
      return 0;
  }
  return 1;
}

int tstr_v_starts_with(tstr_v s, tstr_v prefix) {
  if (prefix.len > s.len)
    return 0;
  if (prefix.len == 0)
    return 1;
  return memcmp(s.data, prefix.data, prefix.len) == 0;
}

int tstr_v_ends_with(tstr_v s, tstr_v suffix) {
  if (suffix.len > s.len)
    return 0;
  if (suffix.len == 0)
    return 1;
  return memcmp(s.data + (s.len - suffix.len), suffix.data, suffix.len) == 0;
}

int tstr_v_contains(tstr_v s, tstr_v needle) {
  return tstr_v_find(s, needle) != TSTR_V_NPOS;
}

size_t tstr_v_find(tstr_v s, tstr_v needle) {
  if (needle.len == 0)
    return 0;
  if (needle.len > s.len)
    return TSTR_V_NPOS;
  char first = needle.data[0];
  size_t limit = s.len - needle.len;
  for (size_t i = 0; i <= limit; i++) {
    const char *p = (const char *)memchr(s.data + i, first, limit - i + 1);
    if (!p)
      return TSTR_V_NPOS;
    i = (size_t)(p - s.data);
    if (memcmp(p, needle.data, needle.len) == 0)
      return i;
  }
  return TSTR_V_NPOS;
}

size_t tstr_v_rfind(tstr_v s, tstr_v needle) {
  if (needle.len == 0)
    return s.len;
  if (needle.len > s.len)
    return TSTR_V_NPOS;
  for (size_t i = s.len - needle.len + 1; i > 0; i--) {
    if (memcmp(s.data + i - 1, needle.data, needle.len) == 0)
      return i - 1;
  }
  return TSTR_V_NPOS;
}

size_t tstr_v_find_char(tstr_v s, char c) {
  if (s.len == 0)
    return TSTR_V_NPOS;
  const char *p = (const char *)memchr(s.data, c, s.len);
  return p ? (size_t)(p - s.data) : TSTR_V_NPOS;
}

size_t tstr_v_rfind_char(tstr_v s, char c) {
  for (size_t i = s.len; i > 0; i--) {
    if (s.data[i - 1] == c)
      return i - 1;
  }
  return TSTR_V_NPOS;
}

tstr_v tstr_v_sub(tstr_v s, size_t pos, size_t n) {
  if (pos >= s.len)
    return tstr_v_from_buf(NULL, 0);
  if (pos + n > s.len)
    n = s.len - pos;
  return tstr_v_from_buf(s.data + pos, n);
}

tstr_v tstr_v_trim(tstr_v s, const char *cset) {
  return tstr_v_trim_left(tstr_v_trim_right(s, cset), cset);
}

tstr_v tstr_v_trim_left(tstr_v s, const char *cset) {
  if (!s.data || s.len == 0 || !cset)
    return s;
  size_t start = 0;
  while (start < s.len && strchr(cset, s.data[start]) != NULL)
    start++;
  return tstr_v_from_buf(s.data + start, s.len - start);
}

tstr_v tstr_v_trim_right(tstr_v s, const char *cset) {
  if (!s.data || s.len == 0 || !cset)
    return s;
  size_t end = s.len;
  while (end > 0 && strchr(cset, s.data[end - 1]) != NULL)
    end--;
  return tstr_v_from_buf(s.data, end);
}

tstr_v tstr_v_split_next(tstr_v *rest, tstr_v delim) {
  if (!rest || rest->len == 0)
    return tstr_v_from_buf(NULL, 0);
  size_t pos = tstr_v_find(*rest, delim);
  if (pos == TSTR_V_NPOS) {
    tstr_v result = *rest;
    *rest = tstr_v_from_buf(rest->data + rest->len, 0);
    return result;
  }
  tstr_v result = tstr_v_from_buf(rest->data, pos);
  *rest = tstr_v_from_buf(rest->data + pos + delim.len, rest->len - pos - delim.len);
  return result;
}

char *tstr_v_to_cstr(tstr_v v) {
  TSTR_V_COPY_TO(v, malloc(v.len + 1));
}

char *tstr_v_to_pool(tstr_v v, MemoryPool *pool) {
  if (!pool) return NULL;
  TSTR_V_COPY_TO(v, pool_alloc(pool, v.len + 1));
}

char *tstr_v_to_arena(tstr_v v, mem_pool_t *arena) {
  if (!arena) return NULL;
  TSTR_V_COPY_TO(v, mem_alloc(arena, v.len + 1));
}
