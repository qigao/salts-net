#include "base64_utils.h"

#include <libbase64.h>
#include <stdlib.h>
#include <string.h>

int tn_base64_encode(const uint8_t *data, size_t len, char **output) {
  if (!data || !output)
    return -1;

  /* Library does not add a terminator; allocate one extra byte. */
  const size_t estimated = 4 * ((len + 2) / 3);
  char *buffer = malloc(estimated + 1);
  if (!buffer)
    return -1;

  size_t written = 0;
  base64_encode((const char *)data, len, buffer, &written, 0);
  buffer[written] = '\0';

  *output = buffer;
  return 0;
}

int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len) {
  if (!input || !output || !output_len)
    return -1;

  const size_t in_len = strlen(input);
  if (in_len == 0)
    return -1;

  /* 3/4 of input size is safe upper bound for decoded bytes. */
  const size_t estimated = (in_len / 4) * 3 + 3;
  uint8_t *buffer = malloc(estimated);
  if (!buffer)
    return -1;

  size_t written = 0;
  /* base64_decode returns 1 on success, 0 on invalid input. */
  int rc = base64_decode(input, in_len, (char *)buffer, &written, 0);
  if (rc != 1) {
    free(buffer);
    return -1;
  }

  *output = buffer;
  *output_len = written;
  return 0;
}
