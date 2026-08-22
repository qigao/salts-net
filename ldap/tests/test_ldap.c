/**
 * @file test_ldap.c
 * @brief LDAP Builder/Parser Unit Tests
 */

#include "ldap_builder.h"
#include "ldap_parser.h"
#include "ldap_protocol.h"
#include "ldap_types.h"
#include "tinytest.h"
#include <string.h>
#include <stdio.h>

spec("ldap") {
  describe("LDAP Builder") {
    it("should build simple bind request correctly") {
        uint8_t buf[256];
        size_t len = sizeof(buf);

        int rc = ldap_build_bind_request(1, 3, "cn=admin,dc=example,dc=com", "secret", buf, &len);

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
        check(len < sizeof(buf));

        /* Verify SEQUENCE tag */
        check_equal(buf[0], 0x30);
    }

    it("should build anonymous bind request correctly") {
        uint8_t buf[128];
        size_t len = sizeof(buf);

        int rc = ldap_build_bind_request(1, 3, "", "", buf, &len);

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
        check_equal(buf[0], 0x30);
    }

    it("should build unbind request correctly") {
        uint8_t buf[64];
        size_t len = sizeof(buf);

        int rc = ldap_build_unbind_request(2, buf, &len);

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
        check_equal(buf[0], 0x30);
    }

    it("should build basic search request correctly") {
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

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
        check_equal(buf[0], 0x30);
    }

    it("should build search request with specified attributes") {
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

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
    }

    it("should build search requests with complex filters correctly") {
        uint8_t buf[512];
        size_t len = sizeof(buf);

        /* Test AND filter */
        int rc = ldap_build_search_request(
            5, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
            0, 0, 0, 0,
            "(&(objectClass=person)(cn=John*))",
            NULL, buf, &len
        );
        check_equal(rc, LDAP_BUILD_OK);

        /* Test OR filter */
        len = sizeof(buf);
        rc = ldap_build_search_request(
            6, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
            0, 0, 0, 0,
            "(|(uid=admin)(uid=root))",
            NULL, buf, &len
        );
        check_equal(rc, LDAP_BUILD_OK);

        /* Test NOT filter */
        len = sizeof(buf);
        rc = ldap_build_search_request(
            7, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
            0, 0, 0, 0,
            "(!(objectClass=computer))",
            NULL, buf, &len
        );
        check_equal(rc, LDAP_BUILD_OK);

        /* Test presence filter */
        len = sizeof(buf);
        rc = ldap_build_search_request(
            8, "dc=example,dc=com", LDAP_SCOPE_SUBTREE,
            0, 0, 0, 0,
            "(mail=*)",
            NULL, buf, &len
        );
        check_equal(rc, LDAP_BUILD_OK);
    }

    it("should build delete request correctly") {
        uint8_t buf[256];
        size_t len = sizeof(buf);

        int rc = ldap_build_delete_request(9, "cn=test,dc=example,dc=com", buf, &len);

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
        check_equal(buf[0], 0x30);
    }

    it("should build add request correctly") {
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

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
    }

    it("should build modify request correctly") {
        uint8_t buf[512];
        size_t len = sizeof(buf);

        ldap_value_t desc_vals[] = {{ (uint8_t *)"New description", 15 }};
        ldap_attribute_t desc_attr = { "description", desc_vals, 1 };

        ldap_modification_t mods[] = {
            { LDAP_MOD_REPLACE, desc_attr }
        };

        int rc = ldap_build_modify_request(11, "cn=test,dc=example,dc=com", mods, 1, buf, &len);

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
    }

    it("should build modifyDN request correctly") {
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

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
    }

    it("should build compare request correctly") {
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

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
    }

    it("should build abandon request correctly") {
        uint8_t buf[64];
        size_t len = sizeof(buf);

        int rc = ldap_build_abandon_request(14, 5, buf, &len);

        check_equal(rc, LDAP_BUILD_OK);
        check(len > 0);
    }
  }

  describe("LDAP Parser") {
    it("should identify complete messages correctly") {
        /* Incomplete message */
        uint8_t incomplete[] = { 0x30, 0x10, 0x02 };
        check_equal(ldap_message_complete(incomplete, sizeof(incomplete)), 0);

        /* Complete short message */
        uint8_t complete[] = { 0x30, 0x03, 0x02, 0x01, 0x01 };
        check_equal(ldap_message_complete(complete, sizeof(complete)), 5);
    }

    it("should parse successful bind response correctly") {
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

        check_equal(rc, LDAP_PARSE_OK);
        check_not_null(result.message);
        check_equal(result.message->message_id, 1);
        check_equal(result.message->protocol_op, LDAP_RES_BIND);
        check_equal(result.message->payload.bind_response.result_code, LDAP_SUCCESS);

        ldap_message_free(result.message);
    }

    it("should parse invalidCredentials bind response correctly") {
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

        check_equal(rc, LDAP_PARSE_OK);
        check_equal(result.message->payload.bind_response.result_code, LDAP_INVALID_CREDENTIALS);

        ldap_message_free(result.message);
    }

    it("should parse SearchResultDone correctly") {
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

        check_equal(rc, LDAP_PARSE_OK);
        check_equal(result.message->protocol_op, LDAP_RES_SEARCH_DONE);
        check_equal(result.message->payload.search_done.result_code, LDAP_SUCCESS);

        ldap_message_free(result.message);
    }

    it("should parse SearchResultEntry correctly") {
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

        check_equal(rc, LDAP_PARSE_OK);
        check_equal(result.message->protocol_op, LDAP_RES_SEARCH_ENTRY);
        check_equal(result.message->payload.search_entry.dn, "cn=admin,dc=example,dc=com");
        check_equal(result.message->payload.search_entry.attribute_count, 1);
        check_equal(result.message->payload.search_entry.attributes[0].type, "cn");
        check_equal(result.message->payload.search_entry.attributes[0].value_count, 1);

        ldap_message_free(result.message);
    }

    it("should provide correct string labels for result codes") {
        check_equal(ldap_result_code_str(LDAP_SUCCESS), "success");
        check_equal(ldap_result_code_str(LDAP_INVALID_CREDENTIALS), "invalidCredentials");
        check_equal(ldap_result_code_str(LDAP_NO_SUCH_OBJECT), "noSuchObject");
        check_equal(ldap_result_code_str(LDAP_BUSY), "busy");
        check_equal(ldap_result_code_str(999), "unknown");
    }
  }

  describe("Round-trip Logic") {
    it("should round-trip a bind request structure through BER") {
        uint8_t buf[256];
        size_t len = sizeof(buf);

        /* Build */
        int rc = ldap_build_bind_request(100, 3, "cn=test", "pass", buf, &len);
        check_equal(rc, LDAP_BUILD_OK);

        /* Parse */
        ldap_parse_result_t result;
        rc = ldap_parse_message(buf, len, &result);

        check_equal(rc, LDAP_PARSE_OK);
        check_equal(result.message->message_id, 100);
        check_equal(result.message->protocol_op, LDAP_REQ_BIND);

        ldap_message_free(result.message);
    }

    it("should round-trip a search request structure through BER") {
        uint8_t buf[512];
        size_t len = sizeof(buf);

        /* Build */
        int rc = ldap_build_search_request(
            200, "dc=test", LDAP_SCOPE_BASE,
            0, 0, 0, 0, "(cn=*)", NULL,
            buf, &len
        );
        check_equal(rc, LDAP_BUILD_OK);

        /* Parse */
        ldap_parse_result_t result;
        rc = ldap_parse_message(buf, len, &result);

        check_equal(rc, LDAP_PARSE_OK);
        check_equal(result.message->message_id, 200);
        check_equal(result.message->protocol_op, LDAP_REQ_SEARCH);

        ldap_message_free(result.message);
    }
  }
}
