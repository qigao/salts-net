#include "password_hash.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "base64_utils.h"
#include <string.h>
#include <stdio.h>
#include <fmt.h>

#define SALT_LEN 16
#define HASH_LEN 32
#define ITERATIONS 100000

/* Format: $pbkdf2$iterations$salt_base64$hash_base64 */
int password_hash(char *out, const char *password, size_t password_len) {
    unsigned char salt[SALT_LEN];
    unsigned char hash[HASH_LEN];
    char salt_b64[64];
    char hash_b64[64];
    
    /* Generate random salt */
    if (RAND_bytes(salt, SALT_LEN) != 1)
        return -1;
    
    /* Derive key using PBKDF2 */
    if (PKCS5_PBKDF2_HMAC(password, (int)password_len, salt, SALT_LEN,
                          ITERATIONS, EVP_sha256(), HASH_LEN, hash) != 1)
        return -1;
    
    /* Encode to base64 */
    char *salt_encoded = NULL;
    char *hash_encoded = NULL;
    if (tn_base64_encode(salt, SALT_LEN, &salt_encoded) != 0)
        return -1;
    if (tn_base64_encode(hash, HASH_LEN, &hash_encoded) != 0) {
        free(salt_encoded);
        return -1;
    }
    if (strlen(salt_encoded) >= sizeof(salt_b64) || strlen(hash_encoded) >= sizeof(hash_b64)) {
        free(salt_encoded);
        free(hash_encoded);
        return -1;
    }
    strcpy(salt_b64, salt_encoded);
    strcpy(hash_b64, hash_encoded);
    free(salt_encoded);
    free(hash_encoded);
    
    /* Format: $pbkdf2$iterations$salt$hash */
    fmt(out, PASSWORD_HASH_STRBYTES, "$pbkdf2${}${}${}", ITERATIONS, salt_b64, hash_b64);
    
    return 0;
}

int password_verify(const char *hash_str, const char *password, size_t password_len) {
    char format[16];
    int iterations;
    char salt_b64[64];
    char hash_b64[64];
    unsigned char salt[SALT_LEN];
    unsigned char stored_hash[HASH_LEN];
    unsigned char computed_hash[HASH_LEN];
    
    /* Parse hash string */
    if (sscanf(hash_str, "$%15[^$]$%d$%63[^$]$%63s", 
               format, &iterations, salt_b64, hash_b64) != 4)
        return -1;
    
    if (strcmp(format, "pbkdf2") != 0)
        return -1;
    
    /* Decode base64 */
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    if (tn_base64_decode(salt_b64, &decoded, &decoded_len) != 0 || decoded_len != SALT_LEN) {
        free(decoded);
        return -1;
    }
    memcpy(salt, decoded, SALT_LEN);
    free(decoded);

    decoded = NULL;
    decoded_len = 0;
    if (tn_base64_decode(hash_b64, &decoded, &decoded_len) != 0 || decoded_len != HASH_LEN) {
        free(decoded);
        return -1;
    }
    memcpy(stored_hash, decoded, HASH_LEN);
    free(decoded);
    
    /* Compute hash with same parameters */
    if (PKCS5_PBKDF2_HMAC(password, (int)password_len, salt, SALT_LEN,
                          iterations, EVP_sha256(), HASH_LEN, computed_hash) != 1)
        return -1;
    
    /* Constant-time comparison */
    int result = 0;
    for (size_t i = 0; i < HASH_LEN; i++)
        result |= stored_hash[i] ^ computed_hash[i];
    
    return result == 0 ? 0 : -1;
}
