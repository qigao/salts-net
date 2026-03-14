#include "websocket_crypto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ============================================================================
 * SHA-1 Implementation (for handshake validation)
 * ========================================================================= */

#define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

#define SHA1_BLK0(i) (block->l[i] = (SHA1_ROL(block->l[i], 24) & 0xFF00FF00) | \
                                     (SHA1_ROL(block->l[i], 8) & 0x00FF00FF))
#define SHA1_BLK(i) (block->l[i & 15] = SHA1_ROL(block->l[(i + 13) & 15] ^ \
                                                  block->l[(i + 8) & 15] ^ \
                                                  block->l[(i + 2) & 15] ^ \
                                                  block->l[i & 15], 1))

#define SHA1_R0(v, w, x, y, z, i) \
    z += ((w & (x ^ y)) ^ y) + SHA1_BLK0(i) + 0x5A827999 + SHA1_ROL(v, 5); \
    w = SHA1_ROL(w, 30);

#define SHA1_R1(v, w, x, y, z, i) \
    z += ((w & (x ^ y)) ^ y) + SHA1_BLK(i) + 0x5A827999 + SHA1_ROL(v, 5); \
    w = SHA1_ROL(w, 30);

#define SHA1_R2(v, w, x, y, z, i) \
    z += (w ^ x ^ y) + SHA1_BLK(i) + 0x6ED9EBA1 + SHA1_ROL(v, 5); \
    w = SHA1_ROL(w, 30);

#define SHA1_R3(v, w, x, y, z, i) \
    z += (((w | x) & y) | (w & x)) + SHA1_BLK(i) + 0x8F1BBCDC + SHA1_ROL(v, 5); \
    w = SHA1_ROL(w, 30);

#define SHA1_R4(v, w, x, y, z, i) \
    z += (w ^ x ^ y) + SHA1_BLK(i) + 0xCA62C1D6 + SHA1_ROL(v, 5); \
    w = SHA1_ROL(w, 30);

typedef union {
    uint8_t c[64];
    uint32_t l[16];
} sha1_block_t;

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e;
    sha1_block_t* block = (sha1_block_t*)buffer;

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    SHA1_R0(a, b, c, d, e, 0);  SHA1_R0(e, a, b, c, d, 1);
    SHA1_R0(d, e, a, b, c, 2);  SHA1_R0(c, d, e, a, b, 3);
    SHA1_R0(b, c, d, e, a, 4);  SHA1_R0(a, b, c, d, e, 5);
    SHA1_R0(e, a, b, c, d, 6);  SHA1_R0(d, e, a, b, c, 7);
    SHA1_R0(c, d, e, a, b, 8);  SHA1_R0(b, c, d, e, a, 9);
    SHA1_R0(a, b, c, d, e, 10); SHA1_R0(e, a, b, c, d, 11);
    SHA1_R0(d, e, a, b, c, 12); SHA1_R0(c, d, e, a, b, 13);
    SHA1_R0(b, c, d, e, a, 14); SHA1_R0(a, b, c, d, e, 15);
    SHA1_R1(e, a, b, c, d, 16); SHA1_R1(d, e, a, b, c, 17);
    SHA1_R1(c, d, e, a, b, 18); SHA1_R1(b, c, d, e, a, 19);
    SHA1_R2(a, b, c, d, e, 20); SHA1_R2(e, a, b, c, d, 21);
    SHA1_R2(d, e, a, b, c, 22); SHA1_R2(c, d, e, a, b, 23);
    SHA1_R2(b, c, d, e, a, 24); SHA1_R2(a, b, c, d, e, 25);
    SHA1_R2(e, a, b, c, d, 26); SHA1_R2(d, e, a, b, c, 27);
    SHA1_R2(c, d, e, a, b, 28); SHA1_R2(b, c, d, e, a, 29);
    SHA1_R2(a, b, c, d, e, 30); SHA1_R2(e, a, b, c, d, 31);
    SHA1_R2(d, e, a, b, c, 32); SHA1_R2(c, d, e, a, b, 33);
    SHA1_R2(b, c, d, e, a, 34); SHA1_R2(a, b, c, d, e, 35);
    SHA1_R2(e, a, b, c, d, 36); SHA1_R2(d, e, a, b, c, 37);
    SHA1_R2(c, d, e, a, b, 38); SHA1_R2(b, c, d, e, a, 39);
    SHA1_R3(a, b, c, d, e, 40); SHA1_R3(e, a, b, c, d, 41);
    SHA1_R3(d, e, a, b, c, 42); SHA1_R3(c, d, e, a, b, 43);
    SHA1_R3(b, c, d, e, a, 44); SHA1_R3(a, b, c, d, e, 45);
    SHA1_R3(e, a, b, c, d, 46); SHA1_R3(d, e, a, b, c, 47);
    SHA1_R3(c, d, e, a, b, 48); SHA1_R3(b, c, d, e, a, 49);
    SHA1_R3(a, b, c, d, e, 50); SHA1_R3(e, a, b, c, d, 51);
    SHA1_R3(d, e, a, b, c, 52); SHA1_R3(c, d, e, a, b, 53);
    SHA1_R3(b, c, d, e, a, 54); SHA1_R3(a, b, c, d, e, 55);
    SHA1_R3(e, a, b, c, d, 56); SHA1_R3(d, e, a, b, c, 57);
    SHA1_R3(c, d, e, a, b, 58); SHA1_R3(b, c, d, e, a, 59);
    SHA1_R4(a, b, c, d, e, 60); SHA1_R4(e, a, b, c, d, 61);
    SHA1_R4(d, e, a, b, c, 62); SHA1_R4(c, d, e, a, b, 63);
    SHA1_R4(b, c, d, e, a, 64); SHA1_R4(a, b, c, d, e, 65);
    SHA1_R4(e, a, b, c, d, 66); SHA1_R4(d, e, a, b, c, 67);
    SHA1_R4(c, d, e, a, b, 68); SHA1_R4(b, c, d, e, a, 69);
    SHA1_R4(a, b, c, d, e, 70); SHA1_R4(e, a, b, c, d, 71);
    SHA1_R4(d, e, a, b, c, 72); SHA1_R4(c, d, e, a, b, 73);
    SHA1_R4(b, c, d, e, a, 74); SHA1_R4(a, b, c, d, e, 75);
    SHA1_R4(e, a, b, c, d, 76); SHA1_R4(d, e, a, b, c, 77);
    SHA1_R4(c, d, e, a, b, 78); SHA1_R4(b, c, d, e, a, 79);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_init(sha1_context_t* ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = ctx->count[1] = 0;
}

void sha1_update(sha1_context_t* ctx, const uint8_t* data, size_t len) {
    size_t i, j;

    j = (ctx->count[0] >> 3) & 63;
    uint32_t len_bits = (uint32_t)(len << 3); // Convert to uint32_t, assume len fits
    uint32_t carry = (ctx->count[0] + len_bits < ctx->count[0]) ? 1 : 0; // Check carry
    ctx->count[0] += len_bits;
    ctx->count[1] += carry;
    ctx->count[1] += (uint32_t)(len >> 29); // Higher bits

    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        sha1_transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

void sha1_final(sha1_context_t* ctx, uint8_t digest[20]) {
    static const uint8_t SHA1_PAD_80[8] = {0x80, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t SHA1_PAD_00[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t i;
    uint8_t finalcount[8];

    for (i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }

    sha1_update(ctx, SHA1_PAD_80, 1);
    while ((ctx->count[0] & 504) != 448) {
        sha1_update(ctx, SHA1_PAD_00, 1);
    }
    sha1_update(ctx, finalcount, 8);

    for (i = 0; i < 20; i++) {
        digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

/* ============================================================================
 * Cryptographically Secure Random
 * ========================================================================= */

int secure_random(uint8_t* buffer, size_t length) {
#ifdef _WIN32
    HCRYPTPROV hProvider = 0;
    if (!CryptAcquireContext(&hProvider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return -1;
    }
    if (!CryptGenRandom(hProvider, (DWORD)length, buffer)) {
        CryptReleaseContext(hProvider, 0);
        return -1;
    }
    CryptReleaseContext(hProvider, 0);
    return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t result = read(fd, buffer, length);
    close(fd);
    return (result == (ssize_t)length) ? 0 : -1;
#endif
}

/* ============================================================================
 * UTF-8 Validation
 * ========================================================================= */

int validate_utf8(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (data[i] <= 0x7F) {
            // 1-byte sequence (ASCII)
            i++;
        } else if ((data[i] & 0xE0) == 0xC0) {
            // 2-byte sequence
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80) return -1;
            // Check for overlong encoding
            if ((data[i] & 0xFE) == 0xC0) return -1; // Overlong 2-byte
            i += 2;
        } else if ((data[i] & 0xF0) == 0xE0) {
            // 3-byte sequence
            if (i + 2 >= len ||
                (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80) return -1;
            // Check for overlong encoding or surrogate halves
            if ((data[i] & 0xF0) == 0xE0 && (data[i] & 0x0F) <= 0x0) {
                if ((data[i] & 0x0F) == 0x0 && (data[i + 1] & 0x20) == 0) return -1;
            }
            // Surrogate halves (U+D800 to U+DFFF)
            uint32_t codepoint = ((data[i] & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6) | (data[i + 2] & 0x3F);
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return -1;
            i += 3;
        } else if ((data[i] & 0xF8) == 0xF0) {
            // 4-byte sequence
            if (i + 3 >= len ||
                (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80 ||
                (data[i + 3] & 0xC0) != 0x80) return -1;
            // Check for overlong encoding or invalid codepoints
            uint32_t codepoint = ((data[i] & 0x07) << 18) | ((data[i + 1] & 0x3F) << 12) | ((data[i + 2] & 0x3F) << 6) | (data[i + 3] & 0x3F);
            if (codepoint < 0x10000 || codepoint > 0x10FFFF) return -1;
            i += 4;
        } else {
            // Invalid byte
            return -1;
        }
    }
    return 0;
}
