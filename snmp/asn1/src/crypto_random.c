/**
 * @file crypto_random.c
 * @brief Cross-platform Cryptographically Secure Random Number Generator
 */

#include "crypto_random.h"
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
/* ============================================================================
 * Windows: BCryptGenRandom
 * ============================================================================ */
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

int crypto_random_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0) return -1;

    NTSTATUS status = BCryptGenRandom(
        NULL,                    // Default algorithm provider
        buf,
        (ULONG)len,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    );

    return (status == 0) ? 0 : -1;
}

#elif defined(__APPLE__)
/* ============================================================================
 * macOS/iOS: arc4random_buf (available since macOS 10.7)
 * ============================================================================ */
#include <stdlib.h>

int crypto_random_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0) return -1;

    arc4random_buf(buf, len);
    return 0;
}

#elif defined(__linux__)
/* ============================================================================
 * Linux: getrandom() syscall (kernel 3.17+) with /dev/urandom fallback
 * ============================================================================ */
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>

#ifndef SYS_getrandom
#define SYS_getrandom 318  // x86_64
#endif

static int getrandom_syscall(void *buf, size_t buflen, unsigned int flags) {
    return (int)syscall(SYS_getrandom, buf, buflen, flags);
}

static int fallback_urandom(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, buf + total, len - total);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        total += n;
    }

    close(fd);
    return 0;
}

int crypto_random_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0) return -1;

    // Try getrandom() first
    ssize_t result = getrandom_syscall(buf, len, 0);
    if (result == (ssize_t)len) {
        return 0;
    }

    // getrandom() not available or failed, fall back to /dev/urandom
    if (result < 0 && errno == ENOSYS) {
        return fallback_urandom(buf, len);
    }

    return -1;
}

#else
/* ============================================================================
 * Other Unix: /dev/urandom
 * ============================================================================ */
#include <fcntl.h>
#include <unistd.h>

int crypto_random_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0) return -1;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, buf + total, len - total);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        total += n;
    }

    close(fd);
    return 0;
}

#endif
