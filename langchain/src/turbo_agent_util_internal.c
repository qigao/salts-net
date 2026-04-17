#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

CXX_C_API char *turbo_agent_util_strdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }

  len = strlen(src) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, src, len);
  return copy;
}

CXX_C_API void turbo_agent_util_free_user_data(void *user_data) { free(user_data); }

CXX_C_API int turbo_agent_util_append_bytes(char **buffer, size_t *length, const char *data,
                                            size_t data_len) {
  char *next;

  if (!buffer || !length || (!data && data_len > 0)) {
    return -1;
  }

  next = (char *)realloc(*buffer, *length + data_len + 1);
  if (!next) {
    return -1;
  }

  if (data_len > 0) {
    memcpy(next + *length, data, data_len);
  }
  *length += data_len;
  next[*length] = '\0';
  *buffer = next;
  return 0;
}

CXX_C_API int turbo_agent_util_append_text(char **buffer, size_t *length, const char *text) {
  if (!buffer || !length || !text) {
    return -1;
  }

  return turbo_agent_util_append_bytes(buffer, length, text, strlen(text));
}
