#include "mime_rfc2822.h"
#include "mime_encoded_word.h"
#include "turbo_str.h"
#include <string.h>
#include <ctype.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static void skip_whitespace(const char **ptr, const char *end) {
  while (*ptr < end && (**ptr == ' ' || **ptr == '\t' || **ptr == '\r' || **ptr == '\n')) {
    (*ptr)++;
  }
}

static void skip_comment(const char **ptr, const char *end) {
  if (*ptr >= end || **ptr != '(') return;

  (*ptr)++; // Skip '('
  int depth = 1;
  while (*ptr < end && depth > 0) {
    if (**ptr == '(') depth++;
    else if (**ptr == ')') depth--;
    else if (**ptr == '\\' && *ptr + 1 < end) (*ptr)++; // Skip escaped char
    (*ptr)++;
  }
}

/* ── Validation ────────────────────────────────────────────────────── */

int mime_is_valid_email(const char *email, size_t len) {
  if (!email || len == 0) return 0;

  // Find @
  const char *at = memchr(email, '@', len);
  if (!at || at == email || at == email + len - 1) return 0;

  // Check local part (before @)
  for (const char *p = email; p < at; p++) {
    if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != '+') {
      return 0;
    }
  }

  // Check domain part (after @)
  const char *domain = at + 1;
  size_t domain_len = len - (domain - email);

  // Must have at least one dot in domain
  if (!memchr(domain, '.', domain_len)) return 0;

  for (size_t i = 0; i < domain_len; i++) {
    if (!isalnum(domain[i]) && domain[i] != '.' && domain[i] != '-') {
      return 0;
    }
  }

  return 1;
}

/* ── Extraction ────────────────────────────────────────────────────── */

char *mime_extract_email(mem_pool_t *pool, const char *address_str, size_t len) {
  if (!pool || !address_str) return NULL;

  const char *ptr = address_str;
  const char *end = address_str + len;

  skip_whitespace(&ptr, end);

  // Look for <email>
  const char *angle_start = memchr(ptr, '<', end - ptr);
  if (angle_start) {
    const char *angle_end = memchr(angle_start + 1, '>', end - angle_start - 1);
    if (angle_end) {
      const char *email_start = angle_start + 1;
      size_t email_len = angle_end - email_start;

      // Trim whitespace
      while (email_len > 0 && (*email_start == ' ' || *email_start == '\t')) {
        email_start++;
        email_len--;
      }
      while (email_len > 0 && (email_start[email_len - 1] == ' ' ||
                                email_start[email_len - 1] == '\t')) {
        email_len--;
      }

      if (email_len > 0) {
        char *result = mem_alloc(pool, email_len + 1);
        if (!result) return NULL;
        memcpy(result, email_start, email_len);
        result[email_len] = '\0';
        return result;
      }
    }
  }

  // No angle brackets, assume entire string is email
  // Skip comments
  while (ptr < end && *ptr == '(') {
    skip_comment(&ptr, end);
    skip_whitespace(&ptr, end);
  }

  const char *email_start = ptr;
  const char *email_end = end;

  // Find end (before comment or whitespace)
  while (ptr < end) {
    if (*ptr == '(' || *ptr == ' ' || *ptr == '\t') {
      email_end = ptr;
      break;
    }
    ptr++;
  }

  size_t email_len = email_end - email_start;
  if (email_len > 0) {
    char *result = mem_alloc(pool, email_len + 1);
    if (!result) return NULL;
    memcpy(result, email_start, email_len);
    result[email_len] = '\0';
    return result;
  }

  return NULL;
}

char *mime_extract_display_name(mem_pool_t *pool, const char *address_str, size_t len) {
  if (!pool || !address_str) return NULL;

  const char *ptr = address_str;
  const char *end = address_str + len;

  skip_whitespace(&ptr, end);

  // Look for <email> - display name is before it
  const char *angle_start = memchr(ptr, '<', end - ptr);
  if (!angle_start) return NULL; // No display name

  const char *name_start = ptr;
  const char *name_end = angle_start;

  // Trim trailing whitespace
  while (name_end > name_start && (name_end[-1] == ' ' || name_end[-1] == '\t')) {
    name_end--;
  }

  if (name_end <= name_start) return NULL;

  // Check if quoted
  if (*name_start == '"') {
    name_start++;
    if (name_end > name_start && name_end[-1] == '"') {
      name_end--;
    }
  }

  size_t name_len = name_end - name_start;
  if (name_len == 0) return NULL;

  // Check if RFC 2047 encoded
  if (mime_is_encoded_word(name_start, name_len)) {
    return mime_decode_header(pool, name_start, name_len);
  }

  // Plain text name
  char *result = mem_alloc(pool, name_len + 1);
  if (!result) return NULL;
  memcpy(result, name_start, name_len);
  result[name_len] = '\0';
  return result;
}

/* ── Parsing ───────────────────────────────────────────────────────── */

mime_address_t *mime_parse_address(mem_pool_t *pool, const char *address_str, size_t len) {
  if (!pool || !address_str) return NULL;

  mime_address_t *addr = mem_alloc(pool, sizeof(mime_address_t));
  if (!addr) return NULL;
  memset(addr, 0, sizeof(*addr));

  // Extract email
  addr->email = mime_extract_email(pool, address_str, len);
  if (!addr->email) return NULL;

  // Extract display name
  addr->display_name = mime_extract_display_name(pool, address_str, len);

  // Split email into local@domain
  const char *at = strchr(addr->email, '@');
  if (at) {
    size_t local_len = at - addr->email;
    addr->local_part = mem_alloc(pool, local_len + 1);
    if (addr->local_part) {
      memcpy(addr->local_part, addr->email, local_len);
      addr->local_part[local_len] = '\0';
    }

    size_t domain_len = strlen(at + 1);
    addr->domain = mem_alloc(pool, domain_len + 1);
    if (addr->domain) {
      memcpy(addr->domain, at + 1, domain_len);
      addr->domain[domain_len] = '\0';
    }
  }

  return addr;
}

mime_address_t *mime_parse_address_list(mem_pool_t *pool, const char *header_value, size_t len) {
  if (!pool || !header_value) return NULL;

  mime_address_t *head = NULL;
  mime_address_t *tail = NULL;

  const char *ptr = header_value;
  const char *end = header_value + len;

  while (ptr < end) {
    skip_whitespace(&ptr, end);
    if (ptr >= end) break;

    // Find next comma or end
    const char *addr_start = ptr;
    const char *addr_end = ptr;
    int in_quotes = 0;
    int in_angle = 0;

    while (ptr < end) {
      if (*ptr == '"') in_quotes = !in_quotes;
      else if (*ptr == '<' && !in_quotes) in_angle = 1;
      else if (*ptr == '>' && !in_quotes) in_angle = 0;
      else if (*ptr == ',' && !in_quotes && !in_angle) {
        addr_end = ptr;
        ptr++; // Skip comma
        break;
      }
      ptr++;
    }

    if (addr_end == addr_start) {
      addr_end = ptr;
    }

    size_t addr_len = addr_end - addr_start;
    if (addr_len > 0) {
      mime_address_t *addr = mime_parse_address(pool, addr_start, addr_len);
      if (addr) {
        if (!head) {
          head = addr;
          tail = addr;
        } else {
          tail->next = addr;
          tail = addr;
        }
      }
    }
  }

  return head;
}

/* ── List operations ───────────────────────────────────────────────── */

void mime_address_free(mime_address_t *addr) {
  // Note: In pool-based allocation, we don't actually free
  // This is a no-op for compatibility
  (void)addr;
}

void mime_address_list_free(mime_address_t *list) {
  // Note: In pool-based allocation, we don't actually free
  // This is a no-op for compatibility
  (void)list;
}

int mime_address_list_count(const mime_address_t *list) {
  int count = 0;
  while (list) {
    count++;
    list = list->next;
  }
  return count;
}

mime_address_t *mime_address_list_get(mime_address_t *list, int index) {
  int i = 0;
  while (list && i < index) {
    list = list->next;
    i++;
  }
  return list;
}
