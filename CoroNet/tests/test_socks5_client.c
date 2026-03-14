/**
 * @file test_socks5_client.c
 * @brief SOCKS5 client tests
 */

#include "tinytest.h"
#include "turbo_socks5.h"
#include <string.h>

/* ── Basic Configuration Tests ────────────────────────────────── */
suite("SOCKS5 ") {
  describe("SOCKS5 Configuration") {
    it("should initialize config with no auth") {
      turbo_socks5_config_t config = {0};
      strcpy(config.host, "127.0.0.1");
      config.port = 1080;
      config.auth_required = 0;

      check_str_eq(config.host, "127.0.0.1");
      check_int_eq(config.port, 1080);
      check_int_eq(config.auth_required, 0);
    }

    it("should initialize config with auth") {
      turbo_socks5_config_t config = {0};
      strcpy(config.host, "proxy.example.com");
      config.port = 1080;
      strcpy(config.username, "user");
      strcpy(config.password, "pass");
      config.auth_required = 1;

      check_str_eq(config.host, "proxy.example.com");
      check_str_eq(config.username, "user");
      check_str_eq(config.password, "pass");
      check_int_eq(config.auth_required, 1);
    }
  }

  /* ── Protocol Constants Tests ─────────────────────────────────── */

  describe("SOCKS5 Protocol Constants") {
    it("should have correct version") { check_int_eq(SOCKS5_VERSION, 0x05); }

    it("should have correct auth methods") {
      check_int_eq(SOCKS5_AUTH_NONE, 0x00);
      check_int_eq(SOCKS5_AUTH_USERPASS, 0x02);
      check_int_eq(SOCKS5_AUTH_FAILED, 0xFF);
    }

    it("should have correct address types") {
      check_int_eq(SOCKS5_ATYP_IPV4, 0x01);
      check_int_eq(SOCKS5_ATYP_DOMAIN, 0x03);
      check_int_eq(SOCKS5_ATYP_IPV6, 0x04);
    }

    it("should have correct commands") { check_int_eq(SOCKS5_CMD_CONNECT, 0x01); }

    it("should have correct reply codes") {
      check_int_eq(SOCKS5_REP_SUCCESS, 0x00);
      check_int_eq(SOCKS5_REP_FAILURE, 0x01);
      check_int_eq(SOCKS5_REP_REFUSED, 0x05);
    }
  }

  /* ── Integration Tests (require real SOCKS5 proxy) ────────────── */

  describe("SOCKS5 Integration") {
    it("should connect via proxy (manual test)") {
      /* This test requires a running SOCKS5 proxy on localhost:1080
       * To run: ssh -D 1080 user@host
       * Then uncomment the test below
       */
      

      /*
      turbo_socks5_config_t config = {0};
      strcpy(config.host, "127.0.0.1");
      config.port = 1080;
      config.auth_required = 0;

      // Connect to proxy first (not implemented here, needs socket creation)
      // int fd = socket(...);
      // connect(fd, proxy_addr, ...);
      // int result = turbo_socks5_connect(fd, &config, "example.com", 80);
      // check_int_eq(result, 0);
      */
    }
  }
}