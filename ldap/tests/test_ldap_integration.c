/**
 * @file test_ldap_integration.c
 * @brief LDAP Integration Tests against public LDAP servers
 *
 * Public test servers:
 * 1. Forum Systems: ldap.forumsys.com:389
 * 2. Zflex: ldap.zflexsoftware.com:389
 */

#include "unity.h"
#include "ldap_client.h"
#include "ldap_parser.h"
#include "ldap_protocol.h"
#include <stdio.h>
#include <string.h>

/* Forum Systems test server */
#define FORUMSYS_URL        "ldap://ldap.forumsys.com:389"
#define FORUMSYS_BIND_DN    "cn=read-only-admin,dc=example,dc=com"
#define FORUMSYS_PASSWORD   "password"
#define FORUMSYS_BASE_DN    "dc=example,dc=com"

/* Test state */
static ldap_client_t *client = NULL;
static int entry_count = 0;

void setUp(void) {
    entry_count = 0;
}

void tearDown(void) {
    if (client) {
        ldap_client_destroy(client);
        client = NULL;
    }
}

/* ============================================================================
 * Connection Tests
 * ============================================================================ */

void test_connect_forumsys(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL_MESSAGE(client, "Failed to create LDAP client");

    /* Use bind to test connection (connect is called internally) */
    ldap_result_data_t result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &result);
    TEST_ASSERT_EQUAL_MESSAGE(0, rc, ldap_err2string(rc));

    printf("  Connected and bound to %s\n", FORUMSYS_URL);
    ldap_result_free(&result);
}

/* ============================================================================
 * Bind Tests
 * ============================================================================ */

void test_bind_simple_forumsys(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    ldap_result_data_t result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &result);

    TEST_ASSERT_EQUAL_MESSAGE(0, rc, ldap_err2string(rc));
    TEST_ASSERT_EQUAL_MESSAGE(LDAP_SUCCESS, result.result_code,
                              ldap_result_code_str(result.result_code));

    printf("  Bind successful: %s\n", FORUMSYS_BIND_DN);

    ldap_result_free(&result);
}

void test_bind_invalid_credentials(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    ldap_result_data_t result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, "wrongpassword", &result);

    /* Should fail with auth error or LDAP invalidCredentials */
    if (rc == 0) {
        TEST_ASSERT_EQUAL(LDAP_INVALID_CREDENTIALS, result.result_code);
        printf("  Got expected invalidCredentials (49)\n");
    } else {
        printf("  Got expected auth error: %s\n", ldap_err2string(rc));
    }

    ldap_result_free(&result);
}

void test_bind_anonymous(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    ldap_result_data_t result = {0};
    int rc = ldap_client_simple_bind(client, "", "", &result);

    /* Anonymous bind may or may not be allowed */
    if (rc == 0) {
        printf("  Anonymous bind result: %s (code %d)\n",
               ldap_result_code_str(result.result_code), result.result_code);
    } else {
        printf("  Anonymous bind failed: %s\n", ldap_err2string(rc));
    }

    ldap_result_free(&result);
}

/* ============================================================================
 * Search Tests
 * ============================================================================ */

static void search_callback(ldap_client_t *cli, const ldap_entry_t *entry, void *user_data) {
    (void)cli;
    (void)user_data;

    entry_count++;
    printf("  Entry %d: %s\n", entry_count, entry->dn);

    /* Print first few attributes */
    for (size_t i = 0; i < entry->attribute_count && i < 3; i++) {
        printf("    %s: ", entry->attributes[i].type);
        if (entry->attributes[i].value_count > 0 &&
            entry->attributes[i].values[0].length < 50) {
            printf("%.*s\n",
                   (int)entry->attributes[i].values[0].length,
                   entry->attributes[i].values[0].data);
        } else {
            printf("(%zu values)\n", entry->attributes[i].value_count);
        }
    }
}

void test_search_base_scope(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    /* Bind first */
    ldap_result_data_t bind_result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
    TEST_ASSERT_EQUAL(0, rc);
    ldap_result_free(&bind_result);

    /* Search base object */
    ldap_search_params_t params = {
        .base_dn = FORUMSYS_BASE_DN,
        .scope = LDAP_SCOPE_BASE,
        .filter = "(objectClass=*)",
        .attrs = NULL,
        .types_only = false,
        .size_limit = 0,
        .time_limit = 0
    };

    ldap_result_data_t search_result = {0};
    rc = ldap_client_search(client, &params, search_callback, NULL, &search_result);

    TEST_ASSERT_EQUAL_MESSAGE(0, rc, ldap_err2string(rc));
    TEST_ASSERT_EQUAL(LDAP_SUCCESS, search_result.result_code);
    TEST_ASSERT_EQUAL(1, entry_count);  /* Base scope should return 1 entry */

    printf("  Base search returned %d entry\n", entry_count);

    ldap_result_free(&search_result);
}

void test_search_subtree_scope(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    /* Bind first */
    ldap_result_data_t bind_result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
    TEST_ASSERT_EQUAL(0, rc);
    ldap_result_free(&bind_result);

    /* Search subtree for persons */
    ldap_search_params_t params = {
        .base_dn = FORUMSYS_BASE_DN,
        .scope = LDAP_SCOPE_SUBTREE,
        .filter = "(objectClass=person)",
        .attrs = NULL,
        .types_only = false,
        .size_limit = 10,
        .time_limit = 30
    };

    ldap_result_data_t search_result = {0};
    rc = ldap_client_search(client, &params, search_callback, NULL, &search_result);

    TEST_ASSERT_EQUAL_MESSAGE(0, rc, ldap_err2string(rc));
    /* Accept success or sizeLimitExceeded (server may limit results) */
    TEST_ASSERT_TRUE(search_result.result_code == LDAP_SUCCESS ||
                     search_result.result_code == LDAP_SIZELIMIT_EXCEEDED);
    TEST_ASSERT_TRUE(entry_count > 0);

    printf("  Subtree search returned %d entries (result: %s)\n",
           entry_count, ldap_result_code_str(search_result.result_code));

    ldap_result_free(&search_result);
}

void test_search_with_filter(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    /* Bind first */
    ldap_result_data_t bind_result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
    TEST_ASSERT_EQUAL(0, rc);
    ldap_result_free(&bind_result);

    /* Search for specific user */
    ldap_search_params_t params = {
        .base_dn = FORUMSYS_BASE_DN,
        .scope = LDAP_SCOPE_SUBTREE,
        .filter = "(uid=einstein)",
        .attrs = NULL,
        .types_only = false,
        .size_limit = 0,
        .time_limit = 0
    };

    ldap_result_data_t search_result = {0};
    rc = ldap_client_search(client, &params, search_callback, NULL, &search_result);

    TEST_ASSERT_EQUAL_MESSAGE(0, rc, ldap_err2string(rc));
    TEST_ASSERT_EQUAL(LDAP_SUCCESS, search_result.result_code);

    printf("  Filter search (uid=einstein) returned %d entries\n", entry_count);

    ldap_result_free(&search_result);
}

void test_search_with_attributes(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    /* Bind first */
    ldap_result_data_t bind_result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
    TEST_ASSERT_EQUAL(0, rc);
    ldap_result_free(&bind_result);

    /* Search with specific attributes */
    const char *attrs[] = {"cn", "mail", "uid", NULL};

    ldap_search_params_t params = {
        .base_dn = FORUMSYS_BASE_DN,
        .scope = LDAP_SCOPE_SUBTREE,
        .filter = "(objectClass=person)",
        .attrs = attrs,
        .types_only = false,
        .size_limit = 5,
        .time_limit = 0
    };

    ldap_result_data_t search_result = {0};
    rc = ldap_client_search(client, &params, search_callback, NULL, &search_result);

    TEST_ASSERT_EQUAL_MESSAGE(0, rc, ldap_err2string(rc));
    /* Accept success or sizeLimitExceeded (server may limit results) */
    TEST_ASSERT_TRUE(search_result.result_code == LDAP_SUCCESS ||
                     search_result.result_code == LDAP_SIZELIMIT_EXCEEDED);

    printf("  Attribute search returned %d entries (result: %s)\n",
           entry_count, ldap_result_code_str(search_result.result_code));

    ldap_result_free(&search_result);
}

/* ============================================================================
 * Unbind Test
 * ============================================================================ */

void test_unbind(void) {
    ldap_client_config_t config = {
        .url = FORUMSYS_URL,
        .timeout_ms = 10000
    };

    client = ldap_client_create(&config);
    TEST_ASSERT_NOT_NULL(client);

    /* Bind */
    ldap_result_data_t bind_result = {0};
    int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
    TEST_ASSERT_EQUAL(0, rc);
    ldap_result_free(&bind_result);

    /* Unbind */
    rc = ldap_client_unbind(client);
    TEST_ASSERT_EQUAL(0, rc);

    printf("  Unbind successful\n");

    /* Client is now disconnected, destroy will handle cleanup */
    ldap_client_destroy(client);
    client = NULL;
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

int main(void) {
    printf("\n=== LDAP Integration Tests ===\n");
    printf("Target: %s\n\n", FORUMSYS_URL);

    UNITY_BEGIN();

    /* Connection */
    RUN_TEST(test_connect_forumsys);

    /* Bind */
    RUN_TEST(test_bind_simple_forumsys);
    RUN_TEST(test_bind_invalid_credentials);
    RUN_TEST(test_bind_anonymous);

    /* Search */
    RUN_TEST(test_search_base_scope);
    RUN_TEST(test_search_subtree_scope);
    RUN_TEST(test_search_with_filter);
    RUN_TEST(test_search_with_attributes);

    /* Unbind */
    RUN_TEST(test_unbind);

    return UNITY_END();
}
