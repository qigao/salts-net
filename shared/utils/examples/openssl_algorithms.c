/* ============================================================================
 * OPENSSL ALGORITHM DISCOVERY EXAMPLE
 * "直接查询OpenSSL支持的算法"
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* OpenSSL Headers */
#include <openssl/ssl.h>
#include <openssl/opensslv.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

void print_openssl_info() {
    printf("[+] OpenSSL Version: %s\n", OpenSSL_version(OPENSSL_VERSION));
    printf("[+] Build Info: %s\n", OpenSSL_version(OPENSSL_BUILT_ON));
    printf("[+] Platform: %s\n", OpenSSL_version(OPENSSL_PLATFORM));
}

void list_ssl_ciphers() {
    printf("\n========================================\n");
    printf("SSL/TLS CIPHER SUITES\n");
    printf("========================================\n");
    
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx) {
        printf("[-] Failed to create SSL context\n");
        return;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        printf("[-] Failed to create SSL object\n");
        SSL_CTX_free(ctx);
        return;
    }
    
    STACK_OF(SSL_CIPHER) *ciphers = SSL_get_ciphers(ssl);
    int cipher_count = sk_SSL_CIPHER_num(ciphers);
    
    printf("Total available ciphers: %d\n\n", cipher_count);
    
    for (int i = 0; i < cipher_count && i < 20; i++) {  /* Show first 20 */
        const SSL_CIPHER *cipher = sk_SSL_CIPHER_value(ciphers, i);
        const char *name = SSL_CIPHER_get_name(cipher);
        const char *version = SSL_CIPHER_get_version(cipher);
        int bits = SSL_CIPHER_get_bits(cipher, NULL);
        
        printf("  [%2d] %-35s %s (%d bits)\n", i+1, name, version, bits);
    }
    
    if (cipher_count > 20) {
        printf("  ... and %d more ciphers\n", cipher_count - 20);
    }
    
    SSL_free(ssl);
    SSL_CTX_free(ctx);
}

void list_elliptic_curves() {
    printf("\n========================================\n");
    printf("ELLIPTIC CURVES\n");
    printf("========================================\n");
    
    size_t curve_count = EC_get_builtin_curves(NULL, 0);
    if (curve_count == 0) {
        printf("No curves available\n");
        return;
    }
    
    EC_builtin_curve *curves = malloc(sizeof(EC_builtin_curve) * curve_count);
    if (!curves) {
        printf("[-] Memory allocation failed\n");
        return;
    }
    
    EC_get_builtin_curves(curves, curve_count);
    printf("Total available curves: %zu\n\n", curve_count);
    
    for (size_t i = 0; i < curve_count && i < 15; i++) {  /* Show first 15 */
        const char *short_name = OBJ_nid2sn(curves[i].nid);
        const char *long_name = OBJ_nid2ln(curves[i].nid);
        
        printf("  [%2zu] %-20s - %s\n", i+1, 
               short_name ? short_name : "unknown",
               long_name ? long_name : "no description");
    }
    
    if (curve_count > 15) {
        printf("  ... and %zu more curves\n", curve_count - 15);
    }
    
    free(curves);
}

void list_message_digests() {
    printf("\n========================================\n");
    printf("MESSAGE DIGEST ALGORITHMS\n");
    printf("========================================\n");
    
    /* Common message digests to check */
    const char* digest_names[] = {
        "md5", "sha1", "sha224", "sha256", "sha384", "sha512",
        "sha3-224", "sha3-256", "sha3-384", "sha3-512",
        "blake2b512", "blake2s256", "sm3", NULL
    };
    
    printf("Checking common digest algorithms:\n\n");
    
    for (int i = 0; digest_names[i]; i++) {
        const EVP_MD *md = EVP_get_digestbyname(digest_names[i]);
        if (md) {
            int digest_size = EVP_MD_size(md);
            printf("  [+] %-15s - %d bytes output\n", digest_names[i], digest_size);
        } else {
            printf("  [-] %-15s - not available\n", digest_names[i]);
        }
    }
}

void check_tls_versions() {
    printf("\n========================================\n");
    printf("TLS PROTOCOL VERSIONS\n");
    printf("========================================\n");
    
    /* Check TLS version support */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    struct {
        const char *name;
        const SSL_METHOD *(*method)(void);
    } tls_versions[] = {
        {"TLS 1.0", TLSv1_method},
        {"TLS 1.1", TLSv1_1_method}, 
        {"TLS 1.2", TLSv1_2_method},
        {"TLS 1.3", TLS_method},  /* TLS 1.3 uses generic method */
        {NULL, NULL}
    };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    
    for (int i = 0; tls_versions[i].name; i++) {
        SSL_CTX *ctx = SSL_CTX_new(tls_versions[i].method());
        if (ctx) {
            printf("  [+] %s - supported\n", tls_versions[i].name);
            SSL_CTX_free(ctx);
        } else {
            printf("  [-] %s - not supported\n", tls_versions[i].name);
        }
    }
    
    /* Check specific TLS 1.3 support */
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (ctx) {
        if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1) {
            printf("  [+] TLS 1.3 - explicitly supported\n");
        } else {
            printf("  [!] TLS 1.3 - may not be supported\n");
        }
        SSL_CTX_free(ctx);
    }
}

void show_recommendations() {
    printf("\n========================================\n");
    printf("SECURITY RECOMMENDATIONS\n");
    printf("========================================\n");
    
    printf("\n[SHIELD] HIGH SECURITY:\n");
    printf("  TLS: 1.3 only\n");
    printf("  Ciphers: TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256\n");
    printf("  Curves: X25519, secp384r1\n");
    printf("  Signatures: ECDSA+SHA384, Ed25519\n");

    printf("\n[BOLT] HIGH PERFORMANCE:\n");
    printf("  TLS: 1.3 preferred, 1.2 fallback\n");
    printf("  Ciphers: TLS_CHACHA20_POLY1305_SHA256, TLS_AES_128_GCM_SHA256\n");
    printf("  Curves: X25519, secp256r1\n");
    printf("  Signatures: ECDSA+SHA256, Ed25519\n");

    printf("\n[PUBLIC] COMPATIBILITY:\n");
    printf("  TLS: 1.2 and 1.3\n");
    printf("  Ciphers: ECDHE-RSA-AES256-GCM-SHA384, ECDHE-RSA-AES128-GCM-SHA256\n");
    printf("  Curves: secp256r1, secp384r1, secp521r1\n");
    printf("  Signatures: RSA-PSS+SHA256, ECDSA+SHA256\n");
}

int main() {
    printf("[SEARCH] OpenSSL Algorithm Discovery Tool\n");
    printf("=====================================\n");
    
    /* Initialize OpenSSL */
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
#else
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif
    
    /* Display OpenSSL information */
    print_openssl_info();
    
    /* List supported algorithms */
    list_ssl_ciphers();
    list_elliptic_curves(); 
    list_message_digests();
    check_tls_versions();
    
    /* Show recommendations */
    show_recommendations();
    
    printf("\n[+] OpenSSL algorithm discovery completed!\n");
    printf("[INFO] Use this information to configure optimal TLS/QUIC settings.\n");
    
    /* Cleanup OpenSSL */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_cleanup();
#endif
    
    return 0;
}
