/**
 * @file test_ldap.c
 * @brief LDAP Builder/Parser Unit Tests
 */

#include "unity.h"
#include "ldap_builder.h"
#include "ldap_parser.h"
#include "ldap_protocol.h"
#include "ldap_types.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * Builder Tests
 * ============================================================================ */

void test_build_bind_request_simple(void) {
    uint8_t buf[256];
    size_t len = sizeof(buf);

    int rc = ldap_build_bind_request(1, 3, "cn=admin,dc=example,dc=com", "secret", buf, &len);

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(len < sizeof(buf));

    /* Verify SEQUENCE tag */
    TEST_ASSERT_EQUAL(0x30, buf[0]);
}

void test_build_bind_request_anonymous(void) {
    uint8_t buf[128];
    size_t len = sizeof(buf);

    int rc = ldap_build_bind_request(1, 3, "", "", buf, &len);

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL(0x30, buf[0]);
}

void test_build_unbind_request(void) {
    uint8_t buf[64];
    size_t len = sizeof(buf);

    int rc = ldap_build_unbind_request(2, buf, &len);

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL(0x30, buf[0]);
}

void test_build_search_request_basic(void) {
    uint8_t buf[512];
    size_t len = sizeof(buf);

    int rc = ldap_build_search_request(
        3,                          /* message_id */
        "dc=example,dc=com",        /* base_dn */
        LDAP_SCOPE_SUBTREE,         /* scope */
        0,                          /* deref_aliases */
        0,                          /* size_limit */
        0,                          /* time_limit */
        0,                          /* types_only */
        "(objectClass=*)",          /* filter */
        NULL,                       /* attributes (all) */
        buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL(0x30, buf[0]);
}

void test_build_search_request_with_attrs(void) {
    uint8_t buf[512];
    size_t len = sizeof(buf);

    const char *attrs[] = {"cn", "mail", "uid", NULL};

    int rc = ldap_build_search_request(
        4,
        "ou=users,dc=example,dc=com",
        LDAP_SCOPE_ONELEVEL,
        0, 100, 30, 0,
        "(uid=john)",
        attrs,
        buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
}

void test_build_search_request_complex_filter(void) {
    uint8_t buf[512];
    size_t len = sizeof(buf);

    /* Test AND filter */
    int rc = ldap_build_search_request(
        5, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
        0, 0, 0, 0,
        "(&(objectClass=person)(cn=John*))",
        NULL, buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);

    /* Test OR filter */
    len = sizeof(buf);
    rc = ldap_build_search_request(
        6, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
        0, 0, 0, 0,
        "(|(uid=admin)(uid=root))",
        NULL, buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);

    /* Test NOT filter */
    len = sizeof(buf);
    rc = ldap_build_search_request(
        7, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
        0, 0, 0, 0,
        "(!(objectClass=computer))",
        NULL, buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);

    /* Test presence filter */
    len = sizeof(buf);
    rc = ldap_build_search_request(
        8, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
        0, 0, 0, 0,
        "(mail=*)",
        NULL, buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
}

void test_build_delete_request(void) {
    uint8_t buf[256];
    size_t len = sizeof(buf);

    int rc = ldap_build_delete_request(9, "cn=test,dc=example,dc=com", buf, &len);

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL(0x30, buf[0]);
}

void test_build_add_request(void) {
    uint8_t buf[512];
    size_t len = sizeof(buf);

    ldap_value_t cn_vals[] = {{ (uint8_t *)"Test User", 9 }};
    ldap_value_t oc_vals[] = {
        { (uint8_t *)"top", 3 },
        { (uint8_t *)"person", 6 }
    };

    ldap_attribute_t attrs[] = {
        { "cn", cn_vals, 1 },
        { "objectClass", oc_vals, 2 }
    };

    int rc = ldap_build_add_request(10, "cn=Test User,dc=example,dc=com", attrs, 2, buf, &len);

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
}

void test_build_modify_request(void) {
    uint8_t buf[512];
    size_t len = sizeof(buf);

    ldap_value_t desc_vals[] = {{ (uint8_t *)"New description", 15 }};
    ldap_attribute_t desc_attr = { "description", desc_vals, 1 };

    ldap_modification_t mods[] = {
        { LDAP_MOD_REPLACE, desc_attr }
    };

    int rc = ldap_build_modify_request(11, "cn=test,dc=example,dc=com", mods, 1, buf, &len);

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
}

void test_build_modifydn_request(void) {
    uint8_t buf[256];
    size_t len = sizeof(buf);

    int rc = ldap_build_modifydn_request(
        12,
        "cn=oldname,ou=users,dc=example,dc=com",
        "cn=newname",
        1,  /* delete old RDN */
        NULL,
        buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
}

void test_build_compare_request(void) {
    uint8_t buf[256];
    size_t len = sizeof(buf);

    int rc = ldap_build_compare_request(
        13,
        "cn=admin,dc=example,dc=com",
        "userPassword",
        (const uint8_t *)"secret",
        6,
        buf, &len
    );

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
}

void test_build_abandon_request(void) {
    uint8_t buf[64];
    size_t len = sizeof(buf);

    int rc = ldap_build_abandon_request(14, 5, buf, &len);

    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);
    TEST_ASSERT_TRUE(len > 0);
}

/* ============================================================================
 * Parser Tests
 * ============================================================================ */

void test_message_complete(void) {
    /* Incomplete message */
    uint8_t incomplete[] = { 0x30, 0x10, 0x02 };
    TEST_ASSERT_EQUAL(0, ldap_message_complete(incomplete, sizeof(incomplete)));

    /* Complete short message */
    uint8_t complete[] = { 0x30, 0x03, 0x02, 0x01, 0x01 };
    TEST_ASSERT_EQUAL(5, ldap_message_complete(complete, sizeof(complete)));
}

void test_parse_bind_response_success(void) {
    /*
     * BindResponse (success):
     * SEQUENCE {
     *   messageID: 1
     *   BindResponse [APPLICATION 1] {
     *     resultCode: 0 (success)
     *     matchedDN: ""
     *     diagnosticMessage: ""
     *   }
     * }
     */
    uint8_t packet[] = {
        0x30, 0x0c,                     /* SEQUENCE, length 12 */
        0x02, 0x01, 0x01,               /* INTEGER messageID = 1 */
        0x61, 0x07,                     /* BindResponse [APPLICATION 1], length 7 */
        0x0a, 0x01, 0x00,               /* ENUMERATED resultCode = 0 */
        0x04, 0x00,                     /* OCTET STRING matchedDN = "" */
        0x04, 0x00                      /* OCTET STRING diagnosticMessage = "" */
    };

    ldap_parse_result_t result;
    int rc = ldap_parse_message(packet, sizeof(packet), &result);

    TEST_ASSERT_EQUAL(LDAP_PARSE_OK, rc);
    TEST_ASSERT_NOT_NULL(result.message);
    TEST_ASSERT_EQUAL(1, result.message->message_id);
    TEST_ASSERT_EQUAL(LDAP_RES_BIND, result.message->protocol_op);
    TEST_ASSERT_EQUAL(LDAP_SUCCESS, result.message->payload.bind_response.result_code);

    ldap_message_free(result.message);
}

void test_parse_bind_response_invalid_credentials(void) {
    /*
     * BindResponse (invalidCredentials):
     */
    uint8_t packet[] = {
        0x30, 0x0c,
        0x02, 0x01, 0x02,               /* messageID = 2 */
        0x61, 0x07,
        0x0a, 0x01, 0x31,               /* resultCode = 49 (invalidCredentials) */
        0x04, 0x00,
        0x04, 0x00
    };

    ldap_parse_result_t result;
    int rc = ldap_parse_message(packet, sizeof(packet), &result);

    TEST_ASSERT_EQUAL(LDAP_PARSE_OK, rc);
    TEST_ASSERT_EQUAL(LDAP_INVALID_CREDENTIALS, result.message->payload.bind_response.result_code);

    ldap_message_free(result.message);
}

void test_parse_search_result_done(void) {
    /*
     * SearchResultDone:
     */
    uint8_t packet[] = {
        0x30, 0x0c,
        0x02, 0x01, 0x03,               /* messageID = 3 */
        0x65, 0x07,                     /* SearchResultDone [APPLICATION 5] */
        0x0a, 0x01, 0x00,               /* resultCode = 0 */
        0x04, 0x00,
        0x04, 0x00
    };

    ldap_parse_result_t result;
    int rc = ldap_parse_message(packet, sizeof(packet), &result);

    TEST_ASSERT_EQUAL(LDAP_PARSE_OK, rc);
    TEST_ASSERT_EQUAL(LDAP_RES_SEARCH_DONE, result.message->protocol_op);
    TEST_ASSERT_EQUAL(LDAP_SUCCESS, result.message->payload.search_done.result_code);

    ldap_message_free(result.message);
}

void test_parse_search_result_entry(void) {
    /*
     * SearchResultEntry:
     * SEQUENCE {
     *   messageID: 4
     *   SearchResultEntry [APPLICATION 4] {
     *     objectName: "cn=admin,dc=example,dc=com"
     *     attributes: SEQUENCE {
     *       SEQUENCE { type: "cn", vals: SET { "admin" } }
     *     }
     *   }
     * }
     */
    uint8_t packet[] = {
        0x30, 0x3a,                     /* SEQUENCE, length 58 */
        0x02, 0x01, 0x04,               /* INTEGER messageID = 4 */
        0x64, 0x35,                     /* SearchResultEntry [APPLICATION 4], length 53 */
        /* objectName */
        0x04, 0x1a, 'c', 'n', '=', 'a', 'd', 'm', 'i', 'n', ',',
                    'd', 'c', '=', 'e', 'x', 'a', 'm', 'p', 'l', 'e', ',',
                    'd', 'c', '=', 'c', 'o', 'm',
        /* attributes SEQUENCE */
        0x30, 0x17,
        /* attribute SEQUENCE */
        0x30, 0x15,
        /* type */
        0x04, 0x02, 'c', 'n',
        /* vals SET */
        0x31, 0x0f,
        0x04, 0x0d, 'a', 'd', 'm', 'i', 'n', 'i', 's', 't', 'r', 'a', 't', 'o', 'r'
    };

    ldap_parse_result_t result;
    int rc = ldap_parse_message(packet, sizeof(packet), &result);

    TEST_ASSERT_EQUAL(LDAP_PARSE_OK, rc);
    TEST_ASSERT_EQUAL(LDAP_RES_SEARCH_ENTRY, result.message->protocol_op);
    TEST_ASSERT_EQUAL_STRING("cn=admin,dc=example,dc=com", result.message->payload.search_entry.dn);
    TEST_ASSERT_EQUAL(1, result.message->payload.search_entry.attribute_count);
    TEST_ASSERT_EQUAL_STRING("cn", result.message->payload.search_entry.attributes[0].type);
    TEST_ASSERT_EQUAL(1, result.message->payload.search_entry.attributes[0].value_count);

    ldap_message_free(result.message);
}

void test_result_code_str(void) {
    TEST_ASSERT_EQUAL_STRING("success", ldap_result_code_str(LDAP_SUCCESS));
    TEST_ASSERT_EQUAL_STRING("invalidCredentials", ldap_result_code_str(LDAP_INVALID_CREDENTIALS));
    TEST_ASSERT_EQUAL_STRING("noSuchObject", ldap_result_code_str(LDAP_NO_SUCH_OBJECT));
    TEST_ASSERT_EQUAL_STRING("busy", ldap_result_code_str(LDAP_BUSY));
    TEST_ASSERT_EQUAL_STRING("unknown", ldap_result_code_str(999));
}

/* ============================================================================
 * Round-trip Tests
 * ============================================================================ */

void test_roundtrip_bind_request(void) {
    uint8_t buf[256];
    size_t len = sizeof(buf);

    /* Build */
    int rc = ldap_build_bind_request(100, 3, "cn=test", "pass", buf, &len);
    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);

    /* Parse */
    ldap_parse_result_t result;
    rc = ldap_parse_message(buf, len, &result);

    /* Note: BindRequest is [APPLICATION 0], parser expects responses */
    /* This test verifies the message structure is valid BER */
    TEST_ASSERT_EQUAL(LDAP_PARSE_OK, rc);
    TEST_ASSERT_EQUAL(100, result.message->message_id);
    TEST_ASSERT_EQUAL(LDAP_REQ_BIND, result.message->protocol_op);

    ldap_message_free(result.message);
}

void test_roundtrip_search_request(void) {
    uint8_t buf[512];
    size_t len = sizeof(buf);

    /* Build */
    int rc = ldap_build_search_request(
        200, "dc=test", LDAP_SCOPE_BASE,
        0, 0, 0, 0, "(cn=*)", NULL,
        buf, &len
    );
    TEST_ASSERT_EQUAL(LDAP_BUILD_OK, rc);

    /* Parse */
    ldap_parse_result_t result;
    rc = ldap_parse_message(buf, len, &result);

    TEST_ASSERT_EQUAL(LDAP_PARSE_OK, rc);
    TEST_ASSERT_EQUAL(200, result.message->message_id);
    TEST_ASSERT_EQUAL(LDAP_REQ_SEARCH, result.message->protocol_op);

    ldap_message_free(result.message);
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Builder tests */
    RUN_TEST(test_build_bind_request_simple);
    RUN_TEST(test_build_bind_request_anonymous);
    RUN_TEST(test_build_unbind_request);
    RUN_TEST(test_build_search_request_basic);
    RUN_TEST(test_build_search_request_with_attrs);
    RUN_TEST(test_build_search_request_complex_filter);
    RUN_TEST(test_build_delete_request);
    RUN_TEST(test_build_add_request);
    RUN_TEST(test_build_modify_request);
    RUN_TEST(test_build_modifydn_request);
    RUN_TEST(test_build_compare_request);
    RUN_TEST(test_build_abandon_request);

    /* Parser tests */
    RUN_TEST(test_message_complete);
    RUN_TEST(test_parse_bind_response_success);
    RUN_TEST(test_parse_bind_response_invalid_credentials);
    RUN_TEST(test_parse_search_result_done);
    RUN_TEST(test_parse_search_result_entry);
    RUN_TEST(test_result_code_str);

    /* Round-trip tests */
    RUN_TEST(test_roundtrip_bind_request);
    RUN_TEST(test_roundtrip_search_request);

    return UNITY_END();
}
