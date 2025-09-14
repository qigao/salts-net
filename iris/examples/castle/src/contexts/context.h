#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdbool.h>
#if defined(_POSIX_C_SOURCE) || defined(__linux__) || defined(__APPLE__)
  #define USE_STRTOK_R
#elif defined(_MSC_VER)
  #define strtok_r strtok_s
#endif
 
typedef struct
{
  char* id;  // It should be char* because libpq waits for string
  char* name;
  char* username;
  bool is_admin;
  bool is_author;
  char* user_slug;
} auth_context_t;

#endif
