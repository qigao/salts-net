#include "s3/s3_client.h"
#include "s3/s3_response.h"
#include "tinytest.h"
#include <http_client.h>
#include <CoroNet/turbo_coro_context.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_coro.h>
#include <turbo_str.h>


// Generate a ListAllMyBucketsResult XML with N buckets
static tstr_t generate_list_buckets_xml(int bucket_count) {
  tstr_t xml = tstr_new();
  xml = tstr_cat(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                      "<Owner><ID>test-owner-id</ID><DisplayName>testuser</DisplayName></Owner>"
                      "<Buckets>");

  for (int i = 0; i < bucket_count; i++) {
    xml = tstr_cat_fmt(xml,
                       "<Bucket>"
                       "<Name>test-bucket-%d</Name>"
                       "<CreationDate>2024-01-01T00:00:00.000Z</CreationDate>"
                       "</Bucket>",
                       i);
  }

  xml = tstr_cat(xml, "</Buckets></ListAllMyBucketsResult>");
  return xml;
}

// Helper: dump response body to file for inspection
static void dump_to_file(const char *filename, const char *data, size_t len) {
  FILE *fp = fopen(filename, "wb");
  if (fp) {
    fwrite(data, 1, len, fp);
    fclose(fp);
  }
}

// Test that fetches real XML from play.min.io and parses it
static void test_real_list_buckets(void *arg) {
  __bdd_config_type__ *__bdd_config__ = ((__bdd_config_type__ **)arg)[0];
  coro_context_t *ctx = ((coro_context_t **)arg)[1];

  // Step 1: Raw HTTP GET to fetch and dump the XML body
  http_client_t *hc = http_client_create(NULL);  http_client_set_timeout(hc, 30000);
  http_response_t *raw = http_get(hc, "https://play.min.io/");
  if (raw && raw->status_code == 200 && raw->body && raw->body_len > 0) {
    printf("  Raw response: status=%d body_len=%zu\n", raw->status_code, raw->body_len);
    dump_to_file("s3_list_buckets_raw.xml", raw->body, raw->body_len);
    printf("  Dumped raw body to s3_list_buckets_raw.xml\n");

    // Verify null termination
    printf("  body[body_len] = 0x%02x\n", (unsigned char)raw->body[raw->body_len]);

    // Try parsing the raw body directly
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(raw->body);
    if (s3_is_ok(res.error)) {
      printf("  Parsed %d buckets from raw body\n", (int)S3BucketVec_size(&res.buckets));
      check((int)S3BucketVec_size(&res.buckets) > 0);
    } else {
      printf("  PARSE ERROR on raw body: %s\n", res.error.message);

      // Dump first 500 chars around first <Bucket>
      const char *fb = strstr(raw->body, "<Bucket>");
      if (fb) {
        size_t off = (size_t)(fb - raw->body);
        printf("  First <Bucket> at offset %zu:\n  %.500s\n", off, fb);
      }
    }
    check(s3_is_ok(res.error));
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
  } else {
    printf("  Raw HTTP failed: status=%d error=%s\n", raw ? raw->status_code : -1,
           (raw && raw->error) ? raw->error : "(null)");
  }
  if (raw)
    http_response_free(raw);
  http_client_destroy(hc);

  // Step 2: Also test via the public S3 API
  s3_base_url_t url = {0};
  url.host = tstr_dup("play.min.io");
  url.port = 443;
  url.is_https = 1;
  url.region = tstr_dup("us-east-1");
  s3_credential_provider_t *prov =
      s3_creds_static("Q3AM3UQ867SPQQA43P2F", "zuf+tfteSlswRu7BJ86wekitnifILbZam1KYY3TG", NULL);
  s3_client_t *client = s3_client_create(ctx, &url, prov);

  s3_list_buckets_response_t resp = s3_list_buckets(client);
  if (s3_is_ok(resp.error)) {
    printf("  s3_list_buckets: %d buckets\n", (int)S3BucketVec_size(&resp.buckets));
    check((int)S3BucketVec_size(&resp.buckets) > 0);
  } else {
    printf("  s3_list_buckets error: %s\n", resp.error.message);
  }
  check(s3_is_ok(resp.error));
  s3_list_buckets_free(&resp);

  s3_client_destroy(client);
  s3_credential_provider_destroy(prov);
  s3_base_url_free(&url);
}

typedef void (*test_fn_t2)(void *arg);
typedef struct {
  test_fn_t2 fn;
  void *arg;
  coro_context_t *ctx;
} coro_wrap2_t;
static void coro_entry2(coro_t *co, void *a) {
  (void)co;
  coro_wrap2_t *w = (coro_wrap2_t *)a;
  w->fn(w->arg);
  coro_context_stop(w->ctx);
}
static void run_in_coro2(coro_context_t *ctx, test_fn_t2 fn, void *arg) {
  coro_wrap2_t w = {.fn = fn, .arg = arg, .ctx = ctx};
  coro_t *co = coro_create(coro_entry2, &w, NULL);
  coro_resume(co);
  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  coro_destroy(co);
}

spec("cxml S3 XML Parser Tests") {

  it("should parse 1 bucket") {
    tstr_t xml = generate_list_buckets_xml(1);
    printf("  xml len = %zu\n", tstr_len(xml));
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(xml);
    check(s3_is_ok(res.error));
    check_int_eq((int)S3BucketVec_size(&res.buckets), 1);
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
    tstr_free(xml);
  }

  it("should parse 10 buckets") {
    tstr_t xml = generate_list_buckets_xml(10);
    printf("  xml len = %zu\n", tstr_len(xml));
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(xml);
    check(s3_is_ok(res.error));
    if (s3_is_ok(res.error)) {
      check_int_eq((int)S3BucketVec_size(&res.buckets), 10);
    } else {
      printf("  PARSE ERROR: %s\n", res.error.message);
    }
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
    tstr_free(xml);
  }

  it("should parse 100 buckets") {
    tstr_t xml = generate_list_buckets_xml(100);
    printf("  xml len = %zu\n", tstr_len(xml));
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(xml);
    check(s3_is_ok(res.error));
    if (s3_is_ok(res.error)) {
      check_int_eq((int)S3BucketVec_size(&res.buckets), 100);
    } else {
      printf("  PARSE ERROR: %s\n", res.error.message);
    }
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
    tstr_free(xml);
  }

  it("should parse 500 buckets") {
    tstr_t xml = generate_list_buckets_xml(500);
    printf("  xml len = %zu\n", tstr_len(xml));
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(xml);
    check(s3_is_ok(res.error));
    if (s3_is_ok(res.error)) {
      check_int_eq((int)S3BucketVec_size(&res.buckets), 500);
    } else {
      printf("  PARSE ERROR: %s\n", res.error.message);
    }
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
    tstr_free(xml);
  }

  it("should parse 1000 buckets (~130KB)") {
    tstr_t xml = generate_list_buckets_xml(1000);
    printf("  xml len = %zu\n", tstr_len(xml));
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(xml);
    check(s3_is_ok(res.error));
    if (s3_is_ok(res.error)) {
      check_int_eq((int)S3BucketVec_size(&res.buckets), 1000);
    } else {
      printf("  PARSE ERROR: %s\n", res.error.message);
    }
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
    tstr_free(xml);
  }

  it("should parse 2000 buckets (~260KB)") {
    tstr_t xml = generate_list_buckets_xml(2000);
    printf("  xml len = %zu\n", tstr_len(xml));
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(xml);
    check(s3_is_ok(res.error));
    if (s3_is_ok(res.error)) {
      check_int_eq((int)S3BucketVec_size(&res.buckets), 2000);
    } else {
      printf("  PARSE ERROR: %s\n", res.error.message);
    }
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
    tstr_free(xml);
  }

  it("should parse minimal Bucket element correctly") {
    const char *xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                      "<Owner><ID>id</ID><DisplayName>dn</DisplayName></Owner>"
                      "<Buckets>"
                      "<Bucket><Name>my-bucket</Name><CreationDate>2024-01-01T00:00:00.000Z</"
                      "CreationDate></Bucket>"
                      "</Buckets>"
                      "</ListAllMyBucketsResult>";
    s3_list_buckets_parser_res_t res = s3_parse_list_buckets_xml(xml);
    check(s3_is_ok(res.error));
    if (s3_is_ok(res.error)) {
      check_int_eq((int)S3BucketVec_size(&res.buckets), 1);
      check_str_eq(S3BucketVec_at(&res.buckets, 0)->name, "my-bucket");
    } else {
      printf("  PARSE ERROR: %s\n", res.error.message);
    }
    S3BucketVec_drop(&res.buckets);
    s3_error_free(&res.error);
  }

  it("should parse real play.min.io ListBuckets response") {
    coro_context_t *ctx = coro_context_create(NULL);
    void *args[2] = {__bdd_config__, ctx};
    run_in_coro2(ctx, test_real_list_buckets, args);
    coro_context_destroy(ctx);
  }
}
