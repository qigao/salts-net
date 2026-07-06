#include "turbo_parser.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

spec("turbo_parser") {
  describe("JSON") {
    it("should parse JSON correctly") {
      const char* json_data = "{\"key\": \"value\", \"number\": 123}";
      void* result = NULL;
      int rc = turbo_parse_json((const uint8_t*)json_data, strlen(json_data), &result);
      
      check_int_eq(rc, 0);
      check_not_null(result);
      check_int_eq(turbo_json_type(result), TURBO_JSON_OBJECT);
      
      turbo_free_json(&result);
      check_null(result);
    }

    it("should build JSON correctly") {
      json_value_t *root = turbo_json_create_object();
      check_not_null(root);

      turbo_json_object_set_string(root, "name", "turbo");
      turbo_json_object_set_number(root, "version", 2.0);
      turbo_json_object_set_bool(root, "active", true);
      
      json_value_t *arr = turbo_json_create_array();
      turbo_json_array_add(arr, turbo_json_create_number(1));
      turbo_json_array_add(arr, turbo_json_create_number(2));
      turbo_json_object_add(root, "items", arr);

      size_t len = 0;
      char *serialized = turbo_json_serialize(root, &len);
      check_not_null(serialized);
      check(len > 0);
      
      check(strstr(serialized, "\"name\":\"turbo\"") != NULL);
      check(strstr(serialized, "\"version\":2") != NULL);
      check(strstr(serialized, "\"items\":[1,2]") != NULL);

      turbo_json_serialize_free(serialized);
      turbo_free_json(&root);
      check_null(root);
    }

    it("should deep clone nested JSON values") {
      json_value_t *root = turbo_json_create_object();
      json_value_t *items = turbo_json_create_array();
      json_value_t *meta = turbo_json_create_object();
      json_value_t *clone;
      char *original_json;
      char *clone_json;

      check_not_null(root);
      check_not_null(items);
      check_not_null(meta);

      turbo_json_array_add(items, turbo_json_create_number(1));
      turbo_json_object_set_string(meta, "status", "ok");
      turbo_json_object_add(root, "items", items);
      turbo_json_object_add(root, "meta", meta);

      clone = turbo_json_clone(root);
      check_not_null(clone);

      turbo_json_array_add(items, turbo_json_create_number(2));
      turbo_json_object_set_string(meta, "owner", "root");

      original_json = turbo_json_serialize(root, NULL);
      clone_json = turbo_json_serialize(clone, NULL);
      check_not_null(original_json);
      check_not_null(clone_json);
      check(strstr(original_json, "\"items\":[1,2]") != NULL);
      check(strstr(original_json, "\"owner\":\"root\"") != NULL);
      check(strstr(clone_json, "\"items\":[1]") != NULL);
      check(strstr(clone_json, "\"owner\":\"root\"") == NULL);
      check(strstr(clone_json, "\"status\":\"ok\"") != NULL);

      turbo_json_serialize_free(clone_json);
      turbo_json_serialize_free(original_json);
      turbo_free_json(&clone);
      turbo_free_json(&root);
    }

    it("should overwrite existing object keys when setting strings") {
      json_value_t *root = turbo_json_create_object();
      char *serialized;

      check_not_null(root);

      turbo_json_object_set_string(root, "stderr", "");
      turbo_json_object_set_string(root, "stderr", "line 1\\line 2\n\"oops\"");

      check_str_eq(turbo_json_get_string(root, "stderr"), "line 1\\line 2\n\"oops\"");

      serialized = turbo_json_serialize(root, NULL);
      check_not_null(serialized);
      check(strstr(serialized, "\"stderr\":\"line 1\\\\line 2\\n\\\"oops\\\"\"") != NULL);

      turbo_json_serialize_free(serialized);
      turbo_free_json(&root);
    }

    it("should query JSON with JSONPath") {
      const char *json_data = "{\"listeners\":[{\"port\":1883,\"transport\":\"tcp\"},"
                              "{\"port\":8883,\"transport\":\"tls\"}]}";
      json_value_t *root = NULL;
      int rc = turbo_parse_json((const uint8_t *)json_data, strlen(json_data), &root);
      check_int_eq(rc, 0);
      check_not_null(root);

      json_value_t *transport = turbo_json_path_get(root, "$.listeners[-1].transport");
      check_not_null(transport);
      check_str_eq(turbo_json_string(transport), "tls");

      turbo_json_path_result_t *ports = turbo_json_path_query(root, "$.listeners[*].port");
      check_not_null(ports);
      check_size_eq(turbo_json_path_result_size(ports), 2);
      check_int_eq((int)turbo_json_number(turbo_json_path_result_get(ports, 0)), 1883);
      check_int_eq((int)turbo_json_number(turbo_json_path_result_get(ports, 1)), 8883);

      turbo_json_path_result_free(ports);
      turbo_free_json(&root);
    }

  }

  describe("INI") {
    it("should parse INI correctly") {
      const char* ini_data = "[section]\nkey=value\n";
      void* result = NULL;
      int rc = turbo_parse_ini((const uint8_t*)ini_data, strlen(ini_data), &result);
      
      check_int_eq(rc, 0);
      check_not_null(result);
      
      turbo_free_ini(&result);
      check_null(result);
    }
  }

  describe("URI") {
    it("should parse URI correctly") {
      const char* uri_data = "https://example.com:8080/path?query#frag";
      void* result = NULL;
      int rc = turbo_parse_uri((const uint8_t*)uri_data, strlen(uri_data), &result);
      
      check_int_eq(rc, 0);
      check_not_null(result);
      
      turbo_free_uri(&result);
      check_null(result);
    }
  }

  describe("CMD Parser") {
    it("should parse command line arguments correctly") {
      turbo_cmd_parser_t *parser = turbo_cmd_create("test_app", "1.0");
      check_not_null(parser);

      bool verbose = false;
      char *output = NULL;
      int64_t count = 0;

      turbo_cmd_add_flag(parser, &verbose, "verbose", "v", "Enable verbose output");
      turbo_cmd_add_string(parser, &output, "output", "o", "Output file");
      turbo_cmd_add_integer(parser, &count, "count", "c", "Count items");

      char *arg0 = strdup("test_app");
      char *arg1 = strdup("--verbose");
      char *arg2 = strdup("-o");
      char *arg3 = strdup("file.txt");
      char *arg4 = strdup("--count=10");

      char *argv[] = {arg0, arg1, arg2, arg3, arg4};
      int argc = 5;

      turbo_cmd_parse(parser, argc, argv, false);
      
      check(verbose);
      check_not_null(output);
      check_str_eq(output, "file.txt");
      check_long_eq(count, 10);

      free(arg0);
      free(arg1);
      free(arg2);
      free(arg3);
      free(arg4);

      turbo_cmd_destroy(parser);
    }
  }

  describe("TOON") {
    it("should parse TOON data correctly") {
      const char* toon_data = 
          "server:\n"
          "  host: \"localhost\"\n"
          "  port: 1883\n"
          "  enabled: true\n"
          "  timeout: 5.5\n"
          "topics: [\"a\", \"b\", \"c\"]\n";
      
      turbo_toon_node_t* root = NULL;
      int rc = turbo_parse_toon((const uint8_t*)toon_data, strlen(toon_data), &root);
      
      check_int_eq(rc, 0);
      check_not_null(root);
      check_int_eq(turbo_toon_type(root), TURBO_TOON_OBJECT);

      turbo_toon_node_t* host = turbo_toon_get(root, "server.host");
      check_not_null(host);
      check_int_eq(turbo_toon_type(host), TURBO_TOON_STRING);
      check_str_eq(turbo_toon_string(host), "localhost");

      turbo_toon_node_t* port = turbo_toon_get(root, "server.port");
      check_not_null(port);
      check_int_eq(turbo_toon_int(port), 1883);

      turbo_toon_node_t* enabled = turbo_toon_get(root, "server.enabled");
      check_not_null(enabled);
      check(turbo_toon_bool(enabled));

      turbo_toon_node_t* timeout = turbo_toon_get(root, "server.timeout");
      check_not_null(timeout);
      check_float_eq(turbo_toon_number(timeout), 5.5, 0.001);

      turbo_toon_node_t* topics = turbo_toon_get(root, "topics");
      check_not_null(topics);
      check_int_eq(turbo_toon_type(topics), TURBO_TOON_LIST);
      check_uint_eq(turbo_toon_array_size(topics), 3);

      turbo_toon_node_t* topic0 = turbo_toon_array_get(topics, 0);
      check_not_null(topic0);
      check_str_eq(turbo_toon_string(topic0), "a");

      // Test serialization
      size_t serialized_len = 0;
      char* serialized = turbo_toon_serialize(root, &serialized_len);
      check_not_null(serialized);
      check(serialized_len > 0);
      check(strstr(serialized, "host: \"localhost\"") != NULL);
      check(strstr(serialized, "port: 1883") != NULL);
      check(strstr(serialized, "topics: [\"a\", \"b\", \"c\"]") != NULL);

      turbo_toon_serialize_free(serialized);

      // Test JSON serialization
      char* json_out = turbo_toon_serialize_json(root, NULL);
      check_not_null(json_out);
      check(strstr(json_out, "\"host\": \"localhost\"") != NULL);
      turbo_toon_serialize_json_free(json_out);

      turbo_free_toon(&root);
      check_null(root);
    }

    it("should parse TOON from JSON correctly") {
      const char* json_in = "{\"name\": \"test\", \"value\": 123}";
      turbo_toon_node_t* toon = turbo_toon_from_json(json_in, strlen(json_in));
      
      check_not_null(toon);
      check_int_eq(turbo_toon_type(toon), TURBO_TOON_OBJECT);
      check_str_eq(turbo_toon_string(turbo_toon_get(toon, "name")), "test");
      check_int_eq(turbo_toon_int(turbo_toon_get(toon, "value")), 123);
      
      turbo_free_toon(&toon);
      check_null(toon);
    }
  }

  describe("DotEnv") {
    it("should load DotEnv file correctly") {
      const char *env_file = ".env.turbo_test";
      FILE *f = fopen(env_file, "w");
      if (f) {
          fprintf(f, "TURBO_PARSER_TEST=success\n");
          fclose(f);
      }

      int rc = turbo_dotenv_load(env_file, true);
      check_int_eq(rc, 0);

      char *val = getenv("TURBO_PARSER_TEST");
      check_not_null(val);
      check_str_eq(val, "success");

      remove(env_file);
    }
  }

  describe("TOML") {
    it("should parse TOML correctly") {
      const char* toml_data = 
          "[server]\n"
          "host = \"localhost\"\n"
          "port = 8080\n"
          "enabled = true\n"
          "timeout = 5.0\n"
          "started = 1979-05-27T07:32:00Z\n"
          "\n"
          "[[databases]]\n"
          "name = \"db1\"\n"
          "\n"
          "[[databases]]\n"
          "name = \"db2\"\n";
      
      turbo_toml_t* root = NULL;
      int rc = turbo_parse_toml((const uint8_t*)toml_data, strlen(toml_data), &root);
      
      check_int_eq(rc, 0);
      check_not_null(root);
      
      // Test table access
      turbo_toml_t* server = turbo_toml_table(root, "server");
      check_not_null(server);
      
      // Test string
      turbo_toml_value_t host = turbo_toml_string(server, "host");
      check(host.ok);
      check_str_eq(host.u.s, "localhost");
      free(host.u.s);
      
      // Test int
      turbo_toml_value_t port = turbo_toml_int(server, "port");
      check(port.ok);
      check_int_eq(port.u.i, 8080);
      
      // Test bool
      turbo_toml_value_t enabled = turbo_toml_bool(server, "enabled");
      check(enabled.ok);
      check(enabled.u.b);
      
      // Test double
      turbo_toml_value_t timeout = turbo_toml_double(server, "timeout");
      check(timeout.ok);
      check_float_eq(timeout.u.d, 5.0, 0.001);
      
      // Test timestamp
      turbo_toml_value_t started = turbo_toml_timestamp(server, "started");
      check(started.ok);
      check_int_eq(started.u.ts.kind, 'd');
      
      // Test array of tables
      turbo_toml_array_t* dbs = turbo_toml_array(root, "databases");
      check_not_null(dbs);
      check_int_eq(turbo_toml_array_len(dbs), 2);
      
      turbo_toml_t* db1 = turbo_toml_array_table(dbs, 0);
      check_not_null(db1);
      turbo_toml_value_t name1 = turbo_toml_string(db1, "name");
      check(name1.ok);
      check_str_eq(name1.u.s, "db1");
      free(name1.u.s);
      
      turbo_free_toml(&root);
      check_null(root);
    }
  }

  describe("Datetime") {
    it("should parse various datetime formats correctly") {
      turbo_datetime_t dt;
      const char *dt_str = "2006-03-14T13:27:54.123+03:45";
      int rc = turbo_parse_datetime(dt_str, strlen(dt_str), &dt);

      check_int_eq(rc, 0);
      check_int_eq(dt.year, 2006);
      check_int_eq(dt.month, 3);
      check_int_eq(dt.day, 14);
      check_int_eq(dt.hour, 13);
      check_int_eq(dt.minute, 27);
      check_int_eq(dt.second, 54);
      check_int_eq(dt.millisecond, 123);
      check_int_eq(dt.tz_offset, 225);
      check(dt.has_tz);

      // Test conversion to time_t
      time_t t = turbo_datetime_to_time(&dt);
      check(t != (time_t)-1);

      // Test RFC 822 formatting
      char buf[64];
      rc = turbo_datetime_format_rfc822(t, buf, sizeof(buf));
      check(rc > 0);
      check(strstr(buf, "2006") != NULL);
      check(strstr(buf, "Mar") != NULL);
    }
  }

  describe("Modbus") {
    it("should write and read a Modbus TCP struct") {
      uint8_t pdu_data[] = {0x00, 0x6B, 0x00, 0x03};
      turbo_modbus_tcp_adu_t adu = {
          .transaction_id = 0x1234,
          .protocol_id = 0,
          .unit_id = 0x11,
          .pdu = {
              .function_code = 0x03,
              .data = pdu_data,
              .data_size = sizeof(pdu_data),
          },
      };
      uint8_t buf[TURBO_MODBUS_TCP_MAX_ADU_SIZE];

      size_t written = turbo_modbus_tcp_write(&adu, buf, sizeof(buf));
      check_size_eq(written, 12);

      turbo_modbus_tcp_adu_t parsed;
      int rc = turbo_modbus_tcp_read(buf, written, &parsed);
      check_int_eq(rc, TURBO_MODBUS_PARSE_OK);
      check_int_eq(parsed.transaction_id, 0x1234);
      check_int_eq(parsed.unit_id, 0x11);
      check_int_eq(parsed.pdu.function_code, 0x03);
      check_mem_eq(parsed.pdu.data, pdu_data, sizeof(pdu_data));
    }

    it("should write and read a Modbus RTU struct") {
      uint8_t pdu_data[] = {0x00, 0x00, 0x00, 0x0A};
      turbo_modbus_rtu_adu_t adu = {
          .address = 0x01,
          .pdu = {
              .function_code = 0x03,
              .data = pdu_data,
              .data_size = sizeof(pdu_data),
          },
      };
      uint8_t buf[TURBO_MODBUS_RTU_MAX_ADU_SIZE];

      size_t written = turbo_modbus_rtu_write(&adu, buf, sizeof(buf));
      check_size_eq(written, 8);
      check_int_eq(buf[6], 0xC5);
      check_int_eq(buf[7], 0xCD);

      turbo_modbus_rtu_adu_t parsed;
      int rc = turbo_modbus_rtu_read(buf, written, &parsed);
      check_int_eq(rc, TURBO_MODBUS_PARSE_OK);
      check_int_eq(parsed.address, 0x01);
      check_int_eq(parsed.pdu.function_code, 0x03);
      check_mem_eq(parsed.pdu.data, pdu_data, sizeof(pdu_data));
    }

    it("should round trip through generic Modbus read and write") {
      uint8_t pdu_data[] = {0x00, 0x01};
      turbo_modbus_tcp_adu_t tcp = {
          .transaction_id = 9,
          .protocol_id = 0,
          .unit_id = 1,
          .pdu = {
              .function_code = 0x06,
              .data = pdu_data,
              .data_size = sizeof(pdu_data),
          },
      };
      turbo_modbus_adu_t adu = {
          .transport = TURBO_MODBUS_TRANSPORT_TCP,
          .frame = {.tcp = tcp},
      };
      uint8_t buf[TURBO_MODBUS_TCP_MAX_ADU_SIZE];
      uint8_t out[TURBO_MODBUS_TCP_MAX_ADU_SIZE];

      size_t written = turbo_modbus_write(&adu, buf, sizeof(buf));
      check(written > 0);

      turbo_modbus_adu_t parsed;
      int rc = turbo_modbus_read(TURBO_MODBUS_TRANSPORT_TCP, buf, written, &parsed);
      check_int_eq(rc, TURBO_MODBUS_PARSE_OK);

      size_t out_len = turbo_modbus_write(&parsed, out, sizeof(out));
      check_size_eq(out_len, written);
      check_mem_eq(out, buf, written);
    }
  }
}
