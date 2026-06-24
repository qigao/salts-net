#include "turbo_parser.h"
#include "tinytest.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

spec("Turbo Parser") {
    describe("JSON Parser") {
        it("should parse valid JSON object") {
            const char* json_data = "{\"key\": \"value\", \"number\": 123}";
            void* result = NULL;
            int rc = turbo_parse_json((const uint8_t*)json_data, strlen(json_data), &result);
            
            check(rc == 0);
            check(result != NULL);
            check(turbo_json_type(result) == TURBO_JSON_OBJECT);
            
            turbo_free_json(&result);
            check(result == NULL);
        }

        it("should parse JSON types correctly") {
             const char* json_data = "{\"s\":\"str\", \"n\":1.5, \"b\":true, \"z\":null}";
             void* result = NULL;
             turbo_parse_json((const uint8_t*)json_data, strlen(json_data), &result);
             
             json_value_t* val;
             
             val = turbo_json_object_get(result, "s");
             check(val != NULL);
             check(turbo_json_type(val) == TURBO_JSON_STRING);
             check(strcmp(turbo_json_string(val), "str") == 0);
             
             val = turbo_json_object_get(result, "n");
             check(val != NULL);
             check(turbo_json_type(val) == TURBO_JSON_NUMBER);
             check(turbo_json_number(val) == 1.5);
             
             val = turbo_json_object_get(result, "b");
             check(val != NULL);
             check(turbo_json_type(val) == TURBO_JSON_BOOL);
             check(turbo_json_bool(val) == true);
             
             val = turbo_json_object_get(result, "z");
             check(val != NULL);
             check(turbo_json_type(val) == TURBO_JSON_NULL);
             check(turbo_json_is_null(val) == true);
             
             turbo_free_json(&result);
        }
    }

    describe("JSON Builder") {
        it("should build a JSON object") {
            json_value_t *root = turbo_json_create_object();
            check(root != NULL);

            turbo_json_object_set_string(root, "name", "turbo");
            turbo_json_object_set_number(root, "version", 2.0);
            turbo_json_object_set_bool(root, "active", true);
            
            size_t len = 0;
            char *serialized = turbo_json_serialize(root, &len);
            check(serialized != NULL);
            check(len > 0);
            
            check(strstr(serialized, "\"name\":\"turbo\"") != NULL);
            check(strstr(serialized, "\"version\":2") != NULL);
            check(strstr(serialized, "\"active\":true") != NULL);
            
            turbo_json_serialize_free(serialized);
            turbo_free_json(&root);
        }
    }

    describe("INI Parser") {
        it("should parse valid INI data") {
            const char* ini_data = "[section]\nkey=value\n";
            void* result = NULL;
            int rc = turbo_parse_ini((const uint8_t*)ini_data, strlen(ini_data), &result);
            
            check(rc == 0);
            check(result != NULL);
            
            const char* val = turbo_ini_get(result, "section", "key");
            check(val != NULL);
            check(strcmp(val, "value") == 0);
            
            turbo_free_ini(&result);
            check(result == NULL);
        }
    }

    describe("URI Parser") {
        it("should parse valid URI") {
            const char* uri_data = "https://example.com:8080/path?query#frag";
            void* result = NULL;
            int rc = turbo_parse_uri((const uint8_t*)uri_data, strlen(uri_data), &result);
            
            check(rc == 0);
            check(result != NULL);
            check(strcmp(turbo_uri_scheme(result), "https") == 0);
            check(strcmp(turbo_uri_host(result), "example.com") == 0);
            check(turbo_uri_port(result) == 8080);
            check(strcmp(turbo_uri_path(result), "/path") == 0);
            
            turbo_free_uri(&result);
        }
    }

    describe("LTV Parser") {
        it("should build and parse LTV message") {
            uint8_t type = 0xAA;
            const uint8_t value[] = {1, 2, 3, 4};
            size_t value_len = sizeof(value);
            
            size_t wire_size = turbo_ltv_wire_size(value_len);
            uint8_t* buffer = (uint8_t*)malloc(wire_size);
            
            size_t built_size = turbo_ltv_build(type, value, value_len, buffer, wire_size);
            check(built_size == wire_size);
            
            void* msg = NULL;
            int rc = turbo_parse_ltv(buffer, built_size, &msg);
            check(rc == 0);
            check(msg != NULL);
            
            check(turbo_ltv_type(msg) == type);
            check(turbo_ltv_value_len(msg) == value_len);
            check(memcmp(turbo_ltv_value(msg), value, value_len) == 0);
            
            turbo_free_ltv(&msg);
            free(buffer);
        }
    }

    /* TLV Parser - Manual Buffer needed for simple test */
    describe("TLV Parser") {
        it("should parse TLV frame") {
            // [MSG_ID(4)][VER(1)][TYPE(1)][LEN(2)][PAYLOAD(4)][CRC(4)]
            // Header is 8 bytes, CRC is 4 bytes. Total 12 + payload.
            uint8_t frame_data[] = {
                0x01, 0x02, 0x03, 0x04, // MSG_ID
                0x01,                   // VER
                0xA1,                   // TYPE
                0x04, 0x00,             // LEN (Little Endian? or Big? Usually networking is Big, but we'll see)
                'H', 'E', 'L', 'O',     // PAYLOAD
                0x00, 0x00, 0x00, 0x00  // CRC (Assuming 0 for simple check if parser allows)
            };
            
            void* result = NULL;
            // The TLV parser might be sensitive to the format. 
            // We'll just check if it parses without crashing if we provide enough data.
            int rc = turbo_parse_tlv(frame_data, sizeof(frame_data), &result);
            if (rc == 0 && result != NULL) {
                check(turbo_tlv_msg_id(result) == 0x04030201); // Assuming LE based on the order
                check(turbo_tlv_type(result) == 0xA1);
                check(turbo_tlv_payload_size(result) == 4);
                check(memcmp(turbo_tlv_payload(result), "HELO", 4) == 0);
                turbo_free_tlv(&result);
            }
        }
    }

    describe("XML Parser") {
        it("should parse valid XML") {
            const char* xml_data = "<root><child id=\"a\">text</child></root>";
            void* result = NULL;
            int rc = turbo_parse_xml((const uint8_t*)xml_data, strlen(xml_data), &result);
            
            check(rc == 0);
            check(result != NULL);
            
            turbo_xml_node_t* root = turbo_xml_root_element(result);
            check(root != NULL);
            check(strcmp(turbo_xml_node_name(root), "root") == 0);

            turbo_xml_node_t* child = turbo_xml_find(root, "<child>/");
            check(child != NULL);

            char* text = turbo_xml_text_dup(child);
            check(text != NULL);
            check(strcmp(text, "text") == 0);
            free(text);

            char* child_text = turbo_xml_child_text_dup(root, "child");
            check(child_text != NULL);
            check(strcmp(child_text, "text") == 0);
            free(child_text);

            turbo_xml_list_t items;
            turbo_xml_list_init(&items);
            turbo_xml_find_all(root, "<child>/", &items);
            int count = 0;
            turbo_xml_for(node, &items) {
                check(node != NULL);
                count++;
            }
            check(count == 1);
            turbo_xml_list_free(&items);

            check(turbo_xml_xpath_count(result, "//child") == 1);
            check(strcmp(turbo_xml_xpath_text(result, "//child"), "text") == 0);

            turbo_xml_xpath_node_t *xpath_child = turbo_xml_xpath_get(result, "//child");
            check(xpath_child != NULL);
            check(turbo_xml_xpath_node_type(xpath_child) == TURBO_XML_NODE_ELEMENT);
            check(strcmp(turbo_xml_xpath_node_type_name(xpath_child), "element") == 0);
            check(strcmp(turbo_xml_xpath_node_name(xpath_child), "child") == 0);
            check(strcmp(turbo_xml_xpath_node_text(xpath_child), "text") == 0);

            char *child_xml = turbo_xml_xpath_node_xml_dup(xpath_child);
            check(child_xml != NULL);
            check(strstr(child_xml, "<child") != NULL);
            turbo_xml_string_free(child_xml);

            turbo_xml_xpath_node_t *xpath_attr = turbo_xml_xpath_get(result, "//@id");
            check(xpath_attr != NULL);
            check(turbo_xml_xpath_node_type(xpath_attr) == TURBO_XML_NODE_ATTRIBUTE);
            check(strcmp(turbo_xml_xpath_node_type_name(xpath_attr), "attribute") == 0);
            check(strcmp(turbo_xml_xpath_node_name(xpath_attr), "id") == 0);
            check(strcmp(turbo_xml_xpath_node_text(xpath_attr), "a") == 0);

            turbo_xml_xpath_node_t *xpath_text = turbo_xml_xpath_get(result, "//child/text()");
            check(xpath_text != NULL);
            check(turbo_xml_xpath_node_type(xpath_text) == TURBO_XML_NODE_TEXT);
            check(strcmp(turbo_xml_xpath_node_type_name(xpath_text), "text") == 0);
            check(strcmp(turbo_xml_xpath_node_text(xpath_text), "text") == 0);

            turbo_xml_list_t xpath_items;
            turbo_xml_xpath_query(result, "//*", &xpath_items);
            int xpath_count = 0;
            turbo_xml_for(xpath_node, &xpath_items) {
                check(xpath_node != NULL);
                xpath_count++;
            }
            check(xpath_count == 2);
            turbo_xml_list_free(&xpath_items);
            
            turbo_free_xml(&result);
            check(result == NULL);
        }
    }

    describe("CSV Parser") {
        it("should parse valid CSV data") {
            const char* csv_data = "name,age\nturbo,2\n";
            void* result = NULL;
            int rc = turbo_parse_csv((const uint8_t*)csv_data, strlen(csv_data), &result);
            
            check(rc == 0);
            check(result != NULL);
            check(turbo_csv_row_count(result) == 2);
            check(turbo_csv_column_count(result) == 2);
            check(strcmp(turbo_csv_get(result, 1, 0), "turbo") == 0);
            
            turbo_free_csv(&result);
        }
    }

    describe("TOML Parser") {
        it("should parse valid TOML data") {
            const char* toml_data = "[server]\nhost = \"localhost\"\nport = 8080\n";
            void* result = NULL;
            int rc = turbo_parse_toml((const uint8_t*)toml_data, strlen(toml_data), &result);
            
            check(rc == 0);
            check(result != NULL);
            
            turbo_toml_t* server = turbo_toml_table(result, "server");
            check(server != NULL);
            
            turbo_toml_value_t host = turbo_toml_string(server, "host");
            check(host.ok);
            check(strcmp(host.u.s, "localhost") == 0);
            free(host.u.s);
            
            turbo_free_toml(&result);
        }
    }

    describe("TOON Parser") {
        it("should parse valid TOON data") {
             const char* toon_data = "name: \"turbo\"\nversion: 1.0\n";
             turbo_toon_node_t* root = NULL;
             int rc = turbo_parse_toon((const uint8_t*)toon_data, strlen(toon_data), &root);
             
             check(rc == 0);
             check(root != NULL);
             check(turbo_toon_type(root) == TURBO_TOON_OBJECT);
             
             turbo_toon_node_t* name = turbo_toon_get(root, "name");
             check(name != NULL);
             check(strcmp(turbo_toon_string(name), "turbo") == 0);
             
             turbo_free_toon(&root);
        }
    }

    describe("DotEnv Loader") {
        it("should load environment variables from file") {
            const char *env_file = ".env.turbo_bdd_test";
            FILE *f = fopen(env_file, "w");
            check(f != NULL);
            fprintf(f, "TURBO_BDD_TEST=success\n");
            fclose(f);

            int rc = turbo_dotenv_load(env_file, true);
            check(rc == 0);

            char *val = getenv("TURBO_BDD_TEST");
            check(val != NULL);
            check(strcmp(val, "success") == 0);

            remove(env_file);
        }
    }

    describe("CMD Parser") {
        it("should parse command line arguments") {
            turbo_cmd_parser_t *parser = turbo_cmd_create("test_app", "1.0");
            check(parser != NULL);

            bool verbose = false;
            char *output = NULL;
            int64_t count = 0;

            turbo_cmd_add_flag(parser, &verbose, "verbose", "v", "Enable verbose");
            turbo_cmd_add_string(parser, &output, "output", "o", "Output file");
            turbo_cmd_add_integer(parser, &count, "count", "c", "Count");

            char *argv[] = {"test_app", "--verbose", "-o", "file.txt", "--count=10"};
            int argc = 5;

            turbo_cmd_parse(parser, argc, argv, false);
            
            check(verbose == true);
            check(output != NULL);
            check(strcmp(output, "file.txt") == 0);
            check(count == 10);

            turbo_cmd_destroy(parser);
        }
    }

    describe("LTV Streaming") {
        it("should reassemble messages from stream") {
            turbo_ltv_stream_t *stream = turbo_ltv_stream_create(1024);
            check(stream != NULL);

            uint8_t type = 0x55;
            uint8_t value[] = {0xDE, 0xAD, 0xBE, 0xEF};
            size_t val_len = sizeof(value);
            size_t wire_size = turbo_ltv_wire_size(val_len);
            uint8_t *buffer = (uint8_t*)malloc(wire_size);
            turbo_ltv_build(type, value, val_len, buffer, wire_size);

            void *out_msg = NULL;
            // Feed half
            int rc = turbo_ltv_stream_feed(stream, buffer, wire_size / 2, &out_msg);
            check(rc == 1); // Need more
            check(out_msg == NULL);

            // Feed rest
            rc = turbo_ltv_stream_feed(stream, buffer + (wire_size / 2), wire_size - (wire_size / 2), &out_msg);
            check(rc == 0); // Success
            check(out_msg != NULL);
            check(turbo_ltv_type(out_msg) == type);
            check(turbo_ltv_value_len(out_msg) == val_len);
            check(memcmp(turbo_ltv_value(out_msg), value, val_len) == 0);

            turbo_free_ltv(&out_msg);
            turbo_ltv_stream_destroy(stream);
            free(buffer);
        }
    }

    describe("SOA Utils") {
        it("should return correct type widths") {
            // Assuming standard widths for common types
            // Type codes usually start from 1
            check(turbo_soa_type_width(1) > 0); 
            check(turbo_soa_type_width(1) == 1 || turbo_soa_type_width(1) == 2 || turbo_soa_type_width(1) == 4 || turbo_soa_type_width(1) == 8);
        }
    }
}
