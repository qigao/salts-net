#ifndef __URI_H__
#define __URI_H__

#include <stdint.h>

// Host types
typedef enum { 
    HOST_UNKNOWN = 0, 
    HOST_REGNAME, 
    HOST_IPV6ADDR, 
    HOST_IPV4ADDR, 
    HOST_IPVFUTURE 
} host_type_t;

// Simplified URL structure - all stack allocated, no malloc!
typedef struct {
    char scheme[32];
    char userinfo[256];
    char host[256];
    char path[1024];
    char query[1024];
    char fragment[256];
    uint16_t port;
    uint8_t host_type;
    uint8_t valid;
} url_t;

// Simple API - re2c does the heavy lifting, no malloc/free needed
int parse_url(const char* url_string, url_t* result);

// Helper function for copying substrings safely
void copy_substring(const char* src, int start, int len, char* dest, int dest_size);

#endif   // __URI_H__
