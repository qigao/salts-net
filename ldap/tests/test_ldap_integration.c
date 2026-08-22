/**
 * @file test_ldap_integration.c
 * @brief LDAP Integration Tests against public LDAP servers
 *
 * Public test servers:
 * 1. Forum Systems: ldap.forumsys.com:389
 * 2. Zflex: ldap.zflexsoftware.com:389
 */

#include "ldap_client.h"
#include "ldap_parser.h"
#include "ldap_protocol.h"
#include "tinytest.h"
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

spec("ldap_integration") {
  before_each() {
    entry_count = 0;
  }

  after_each() {
    if (client) {
        ldap_client_destroy(client);
        client = NULL;
    }
  }

  describe("LDAP Connection") {
    it("should successfully connect and bind to ForumSys") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        /* Use bind to test connection (connect is called internally) */
        ldap_result_data_t result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &result);
        check_equal(rc, 0);

        printf("  Connected and bound to %s\n", FORUMSYS_URL);
        ldap_result_free(&result);
    }
  }

  describe("LDAP Bind Operations") {
    it("should successfully perform simple bind to ForumSys") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        ldap_result_data_t result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &result);

        check_equal(rc, 0);
        check_equal(result.result_code, LDAP_SUCCESS);

        printf("  Bind successful: %s\n", FORUMSYS_BIND_DN);

        ldap_result_free(&result);
    }

    it("should return expected failure for invalid credentials") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        ldap_result_data_t result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, "wrongpassword", &result);

        /* Should fail with auth error or LDAP invalidCredentials */
        if (rc == 0) {
            check_equal(result.result_code, LDAP_INVALID_CREDENTIALS);
            printf("  Got expected invalidCredentials (49)\n");
        } else {
            printf("  Got expected auth error: %s\n", ldap_err2string(rc));
        }

        ldap_result_free(&result);
    }

    it("should handle anonymous bind attempts") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

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
  }

  describe("LDAP Search Operations") {
    it("should successfully search with base scope") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        /* Bind first */
        ldap_result_data_t bind_result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
        check_equal(rc, 0);
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

        check_equal(rc, 0);
        check_equal(search_result.result_code, LDAP_SUCCESS);
        check_equal(entry_count, 1);  /* Base scope should return 1 entry */

        printf("  Base search returned %d entry\n", entry_count);

        ldap_result_free(&search_result);
    }

    it("should successfully search with subtree scope") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        /* Bind first */
        ldap_result_data_t bind_result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
        check_equal(rc, 0);
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

        check_equal(rc, 0);
        /* Accept success or sizeLimitExceeded (server may limit results) */
        check(search_result.result_code == LDAP_SUCCESS ||
              search_result.result_code == LDAP_SIZELIMIT_EXCEEDED);
        check(entry_count > 0);

        printf("  Subtree search returned %d entries (result: %s)\n",
               entry_count, ldap_result_code_str(search_result.result_code));

        ldap_result_free(&search_result);
    }

    it("should successfully search with a filter") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        /* Bind first */
        ldap_result_data_t bind_result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
        check_equal(rc, 0);
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

        check_equal(rc, 0);
        check_equal(search_result.result_code, LDAP_SUCCESS);

        printf("  Filter search (uid=einstein) returned %d entries\n", entry_count);

        ldap_result_free(&search_result);
    }

    it("should search and retrieve specific attributes") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        /* Bind first */
        ldap_result_data_t bind_result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
        check_equal(rc, 0);
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

        check_equal(rc, 0);
        /* Accept success or sizeLimitExceeded (server may limit results) */
        check(search_result.result_code == LDAP_SUCCESS ||
              search_result.result_code == LDAP_SIZELIMIT_EXCEEDED);

        printf("  Attribute search returned %d entries (result: %s)\n",
               entry_count, ldap_result_code_str(search_result.result_code));

        ldap_result_free(&search_result);
    }
  }

  describe("LDAP Unbind Operation") {
    it("should successfully unbind and close the connection") {
        ldap_client_config_t config = {
            .url = FORUMSYS_URL,
            .timeout_ms = 10000
        };

        client = ldap_client_create(&config);
        check_not_null(client);

        /* Bind */
        ldap_result_data_t bind_result = {0};
        int rc = ldap_client_simple_bind(client, FORUMSYS_BIND_DN, FORUMSYS_PASSWORD, &bind_result);
        check_equal(rc, 0);
        ldap_result_free(&bind_result);

        /* Unbind */
        rc = ldap_client_unbind(client);
        check_equal(rc, 0);

        printf("  Unbind successful\n");

        /* Client is now disconnected, destroy will handle cleanup */
        ldap_client_destroy(client);
        client = NULL;
    }
  }
}
