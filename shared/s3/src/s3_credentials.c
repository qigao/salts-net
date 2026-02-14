#include "s3/s3_credentials.h"
#include "s3/s3_signer.h"
#include "s3/s3_time.h"
#include "s3_http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

void s3_credentials_clear(s3_credentials_t* creds) {
    if (!creds) return;
    tstr_free(creds->access_key);
    tstr_free(creds->secret_key);
    tstr_free(creds->session_token);
    memset(creds, 0, sizeof(s3_credentials_t));
}

void s3_credential_provider_destroy(s3_credential_provider_t* provider) {
    if (!provider) return;
    if (provider->destroy && provider->ctx) {
        provider->destroy(provider->ctx);
    }
    free(provider);
}

// Static Provider
typedef struct {
    tstr_t access_key;
    tstr_t secret_key;
    tstr_t session_token;
} static_ctx_t;

static s3_error_t static_fetch(void* ctx, s3_credentials_t* out) {
    static_ctx_t* c = (static_ctx_t*)ctx;
    out->access_key = tstr_dup(c->access_key);
    out->secret_key = tstr_dup(c->secret_key);
    out->session_token = c->session_token ? tstr_dup(c->session_token) : NULL;
    out->expiration = 0;
    return S3_OK;
}

static void static_destroy(void* ctx) {
    static_ctx_t* c = (static_ctx_t*)ctx;
    tstr_free(c->access_key);
    tstr_free(c->secret_key);
    tstr_free(c->session_token);
    free(c);
}

s3_credential_provider_t* s3_creds_static(const char* access_key, const char* secret_key, const char* session_token) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    static_ctx_t* ctx = calloc(1, sizeof(static_ctx_t));
    ctx->access_key = tstr_dup(access_key);
    ctx->secret_key = tstr_dup(secret_key);
    ctx->session_token = session_token ? tstr_dup(session_token) : NULL;
    
    p->fetch = static_fetch;
    p->ctx = ctx;
    p->destroy = static_destroy;
    return p;
}

// Env Provider (AWS)
static s3_error_t env_aws_fetch(void* ctx, s3_credentials_t* out) {
    const char* ak = getenv("AWS_ACCESS_KEY_ID");
    const char* sk = getenv("AWS_SECRET_ACCESS_KEY");
    const char* st = getenv("AWS_SESSION_TOKEN");

    if (ak && sk) {
        out->access_key = tstr_dup(ak);
        out->secret_key = tstr_dup(sk);
        out->session_token = st ? tstr_dup(st) : NULL;
        out->expiration = 0;
        return S3_OK;
    }
    return s3_error_make(-1, "AWS credentials not found in environment");
}

s3_credential_provider_t* s3_creds_env_aws(void) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    p->fetch = env_aws_fetch;
    return p;
}

// Env Provider (MinIO)
static s3_error_t env_minio_fetch(void* ctx, s3_credentials_t* out) {
    (void)ctx;
    const char* ak = getenv("MINIO_ACCESS_KEY");
    const char* sk = getenv("MINIO_SECRET_KEY");
    if (!ak) ak = getenv("MINIO_ROOT_USER");
    if (!sk) sk = getenv("MINIO_ROOT_PASSWORD");
    if (ak && sk) {
        out->access_key = tstr_dup(ak);
        out->secret_key = tstr_dup(sk);
        out->session_token = NULL;
        out->expiration = 0;
        return S3_OK;
    }
    return s3_error_make(-1, "MinIO credentials not found in environment");
}

s3_credential_provider_t* s3_creds_env_minio(void) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    p->fetch = env_minio_fetch;
    return p;
}

// AWS Config File Provider
typedef struct {
    tstr_t filename;
    tstr_t profile;
} aws_config_ctx_t;

static s3_error_t aws_config_fetch(void* ctx, s3_credentials_t* out) {
    aws_config_ctx_t* c = (aws_config_ctx_t*)ctx;
    const char* fname = c->filename;
    if (!fname || !fname[0]) {
        const char* home = getenv("HOME");
        if (!home) home = getenv("USERPROFILE");
        if (!home) return s3_error_make(-1, "Cannot determine home directory");
        tstr_t path = tstr_cat_fmt(tstr_new(), "%s/.aws/credentials", home);
        // Read the file
        FILE* fp = fopen(path, "r");
        tstr_free(path);
        if (!fp) return s3_error_make(-1, "Cannot open AWS credentials file");
        fclose(fp);
        // For simplicity, fall through to ini parsing below
        fname = path;
    }

    const char* profile = (c->profile && c->profile[0]) ? c->profile : "default";
    FILE* fp = fopen(fname, "r");
    if (!fp) return s3_error_make(-1, "Cannot open AWS credentials file");

    tstr_t ak = NULL, sk = NULL, st = NULL;
    char line[1024];
    int in_profile = 0;
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                in_profile = (strcmp(line + 1, profile) == 0);
            }
            continue;
        }
        if (!in_profile) continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        while (*key == ' ') key++;
        while (*val == ' ') val++;
        // trim trailing spaces from key
        char* ke = (char*)key + strlen(key) - 1;
        while (ke > key && *ke == ' ') *ke-- = '\0';

        if (strcmp(key, "aws_access_key_id") == 0) { tstr_free(ak); ak = tstr_dup(val); }
        else if (strcmp(key, "aws_secret_access_key") == 0) { tstr_free(sk); sk = tstr_dup(val); }
        else if (strcmp(key, "aws_session_token") == 0) { tstr_free(st); st = tstr_dup(val); }
    }
    fclose(fp);

    if (ak && sk) {
        out->access_key = ak;
        out->secret_key = sk;
        out->session_token = st;
        out->expiration = 0;
        return S3_OK;
    }
    tstr_free(ak); tstr_free(sk); tstr_free(st);
    return s3_error_make(-1, "Profile not found in AWS credentials file");
}

static void aws_config_destroy(void* ctx) {
    aws_config_ctx_t* c = (aws_config_ctx_t*)ctx;
    tstr_free(c->filename);
    tstr_free(c->profile);
    free(c);
}

s3_credential_provider_t* s3_creds_aws_config(const char* filename, const char* profile) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    aws_config_ctx_t* ctx = calloc(1, sizeof(aws_config_ctx_t));
    ctx->filename = filename ? tstr_dup(filename) : NULL;
    ctx->profile = profile ? tstr_dup(profile) : NULL;
    p->fetch = aws_config_fetch;
    p->ctx = ctx;
    p->destroy = aws_config_destroy;
    return p;
}

// Chain Provider
typedef struct {
    s3_credential_provider_t** providers;
    int count;
} chain_ctx_t;

static s3_error_t chain_fetch(void* ctx, s3_credentials_t* out) {
    chain_ctx_t* c = (chain_ctx_t*)ctx;
    for (int i = 0; i < c->count; i++) {
        s3_error_t err = c->providers[i]->fetch(c->providers[i]->ctx, out);
        if (s3_is_ok(err)) return S3_OK;
        s3_error_free(&err);
    }
    return s3_error_make(-1, "No credential provider in chain succeeded");
}

static void chain_destroy(void* ctx) {
    chain_ctx_t* c = (chain_ctx_t*)ctx;
    for (int i = 0; i < c->count; i++) {
        s3_credential_provider_destroy(c->providers[i]);
    }
    free(c->providers);
    free(c);
}

s3_credential_provider_t* s3_creds_chain(s3_credential_provider_t** providers, int count) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    chain_ctx_t* ctx = calloc(1, sizeof(chain_ctx_t));
    ctx->providers = calloc((size_t)count, sizeof(s3_credential_provider_t*));
    memcpy(ctx->providers, providers, (size_t)count * sizeof(s3_credential_provider_t*));
    ctx->count = count;
    p->fetch = chain_fetch;
    p->ctx = ctx;
    p->destroy = chain_destroy;
    return p;
}

// S3 Client Config Provider (mc config)
typedef struct {
    tstr_t filename;
    tstr_t alias;
} mc_config_ctx_t;

static s3_error_t mc_config_fetch(void* ctx, s3_credentials_t* out) {
    mc_config_ctx_t* c = (mc_config_ctx_t*)ctx;
    const char* fname = c->filename;
    tstr_t path = NULL;
    if (!fname || !fname[0]) {
        const char* home = getenv("HOME");
        if (!home) home = getenv("USERPROFILE");
        if (!home) return s3_error_make(-1, "Cannot determine home directory");
        path = tstr_cat_fmt(tstr_new(), "%s/.mc/config.json", home);
        fname = path;
    }

    FILE* fp = fopen(fname, "r");
    if (!fp) { tstr_free(path); return s3_error_make(-1, "Cannot open mc config file"); }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); tstr_free(path); return s3_error_make(-1, "Empty mc config"); }
    char* buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);
    tstr_free(path);

    // Simple JSON key search for the alias
    const char* alias = c->alias ? c->alias : "s3";
    tstr_t ak = NULL, sk = NULL;

    // Find "alias": { ... "accessKey": "...", "secretKey": "..." }
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", alias);
    const char* pos = strstr(buf, search);
    if (pos) {
        const char* block_end = strchr(pos, '}');
        if (block_end) {
            // Extract accessKey
            const char* akp = strstr(pos, "\"accessKey\"");
            if (!akp) akp = strstr(pos, "\"access_key\"");
            if (akp && akp < block_end) {
                const char* q1 = strchr(akp + 11, '"');
                if (q1) { const char* q2 = strchr(q1 + 1, '"'); if (q2) ak = tstr_dup_len(q1 + 1, (size_t)(q2 - q1 - 1)); }
            }
            // Extract secretKey
            const char* skp = strstr(pos, "\"secretKey\"");
            if (!skp) skp = strstr(pos, "\"secret_key\"");
            if (skp && skp < block_end) {
                const char* q1 = strchr(skp + 11, '"');
                if (q1) { const char* q2 = strchr(q1 + 1, '"'); if (q2) sk = tstr_dup_len(q1 + 1, (size_t)(q2 - q1 - 1)); }
            }
        }
    }
    free(buf);

    if (ak && sk) {
        out->access_key = ak;
        out->secret_key = sk;
        out->session_token = NULL;
        out->expiration = 0;
        return S3_OK;
    }
    tstr_free(ak); tstr_free(sk);
    return s3_error_make(-1, "Alias not found in mc config");
}

static void mc_config_destroy(void* ctx) {
    mc_config_ctx_t* c = (mc_config_ctx_t*)ctx;
    tstr_free(c->filename);
    tstr_free(c->alias);
    free(c);
}

s3_credential_provider_t* s3_creds_minio_client_config(const char* filename, const char* alias) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    mc_config_ctx_t* ctx = calloc(1, sizeof(mc_config_ctx_t));
    ctx->filename = filename ? tstr_dup(filename) : NULL;
    ctx->alias = alias ? tstr_dup(alias) : NULL;
    p->fetch = mc_config_fetch;
    p->ctx = ctx;
    p->destroy = mc_config_destroy;
    return p;
}

// ── STS Assume Role Provider ──

typedef struct {
    tstr_t sts_endpoint;
    tstr_t access_key;
    tstr_t secret_key;
    tstr_t region;
    tstr_t role_arn;
    tstr_t session_name;
    int duration_secs;
    s3_credentials_t cached;
    time_t cached_expiry;
} assume_role_ctx_t;

static tstr_t extract_xml_value(const char* xml, const char* tag) {
    char open[128], close[128];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char* s = strstr(xml, open);
    if (!s) return NULL;
    s += strlen(open);
    const char* e = strstr(s, close);
    if (!e) return NULL;
    return tstr_dup_len(s, (size_t)(e - s));
}

static s3_error_t sts_parse_credentials(const char* xml, s3_credentials_t* out) {
    tstr_t ak = extract_xml_value(xml, "AccessKeyId");
    tstr_t sk = extract_xml_value(xml, "SecretAccessKey");
    tstr_t st = extract_xml_value(xml, "SessionToken");
    if (!ak || !sk) {
        tstr_free(ak); tstr_free(sk); tstr_free(st);
        return s3_error_make(-1, "Failed to parse STS credentials from response");
    }
    out->access_key = ak;
    out->secret_key = sk;
    out->session_token = st;
    tstr_t exp = extract_xml_value(xml, "Expiration");
    if (exp) {
        out->expiration = s3_time_from_iso8601(exp);
        tstr_free(exp);
    }
    return S3_OK;
}

static s3_error_t assume_role_fetch(void* ctx, s3_credentials_t* out) {
    assume_role_ctx_t* c = (assume_role_ctx_t*)ctx;

    // Return cached if still valid (with 5 min margin)
    if (c->cached.access_key && c->cached_expiry > time(NULL) + 300) {
        out->access_key = tstr_dup(c->cached.access_key);
        out->secret_key = tstr_dup(c->cached.secret_key);
        out->session_token = c->cached.session_token ? tstr_dup(c->cached.session_token) : NULL;
        out->expiration = c->cached.expiration;
        return S3_OK;
    }

    // Build STS request body
    tstr_t body = tstr_cat_fmt(tstr_new(),
        "Action=AssumeRole&Version=2011-06-15&RoleArn=%s&RoleSessionName=%s&DurationSeconds=%d",
        c->role_arn, c->session_name ? c->session_name : "s3-c-sdk",
        c->duration_secs > 0 ? c->duration_secs : 3600);

    // Execute HTTP POST to STS endpoint
    s3_http_request_t req = {0};
    req.method = "POST";
    req.url = c->sts_endpoint;
    req.body = body;
    req.body_len = tstr_len(body);
    req.headers = S3Headers_init();
    s3_headers_add(&req.headers, "Content-Type", "application/x-www-form-urlencoded");

    // Sign with STS service
    S3Headers qp = S3Headers_init();
    time_t now = time(NULL);
    tstr_t sha = s3_signer_sha256_hex(body, tstr_len(body));
    s3_error_t err = s3_signer_sign_v4_sts("POST", "/", c->region,
        &req.headers, &qp, c->access_key, c->secret_key, NULL, sha, now);
    tstr_free(sha);
    S3Headers_drop(&qp);

    if (!s3_is_ok(err)) {
        tstr_free(body);
        S3Headers_drop(&req.headers);
        return err;
    }

    s3_http_response_t hres = s3_http_execute(&req);
    tstr_free(body);
    S3Headers_drop(&req.headers);

    if (!s3_is_ok(hres.error)) {
        err = hres.error; hres.error = S3_OK;
        s3_http_response_free(&hres);
        return err;
    }
    if (hres.status_code != 200) {
        err = s3_error_make(hres.status_code, "STS AssumeRole failed");
        s3_http_response_free(&hres);
        return err;
    }

    err = sts_parse_credentials(hres.body, out);
    if (s3_is_ok(err)) {
        // Cache
        s3_credentials_clear(&c->cached);
        c->cached.access_key = tstr_dup(out->access_key);
        c->cached.secret_key = tstr_dup(out->secret_key);
        c->cached.session_token = out->session_token ? tstr_dup(out->session_token) : NULL;
        c->cached.expiration = out->expiration;
        c->cached_expiry = out->expiration;
    }
    s3_http_response_free(&hres);
    return err;
}

static void assume_role_destroy(void* ctx) {
    assume_role_ctx_t* c = (assume_role_ctx_t*)ctx;
    tstr_free(c->sts_endpoint); tstr_free(c->access_key); tstr_free(c->secret_key);
    tstr_free(c->region); tstr_free(c->role_arn); tstr_free(c->session_name);
    s3_credentials_clear(&c->cached);
    free(c);
}

s3_credential_provider_t* s3_creds_assume_role(const char* sts_endpoint, const char* access_key,
                                                      const char* secret_key, const char* region,
                                                      const char* role_arn, const char* session_name,
                                                      int duration_secs) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    assume_role_ctx_t* ctx = calloc(1, sizeof(assume_role_ctx_t));
    ctx->sts_endpoint = tstr_dup(sts_endpoint);
    ctx->access_key = tstr_dup(access_key);
    ctx->secret_key = tstr_dup(secret_key);
    ctx->region = region ? tstr_dup(region) : tstr_dup("us-east-1");
    ctx->role_arn = tstr_dup(role_arn);
    ctx->session_name = session_name ? tstr_dup(session_name) : NULL;
    ctx->duration_secs = duration_secs;
    p->fetch = assume_role_fetch;
    p->ctx = ctx;
    p->destroy = assume_role_destroy;
    return p;
}

// ── Web Identity Provider ──

typedef struct {
    tstr_t sts_endpoint;
    tstr_t region;
    tstr_t role_arn;
    tstr_t (*token_fn)(void* ctx);
    void* token_ctx;
    s3_credentials_t cached;
    time_t cached_expiry;
} web_identity_ctx_t;

static s3_error_t web_identity_fetch(void* ctx, s3_credentials_t* out) {
    web_identity_ctx_t* c = (web_identity_ctx_t*)ctx;

    if (c->cached.access_key && c->cached_expiry > time(NULL) + 300) {
        out->access_key = tstr_dup(c->cached.access_key);
        out->secret_key = tstr_dup(c->cached.secret_key);
        out->session_token = c->cached.session_token ? tstr_dup(c->cached.session_token) : NULL;
        out->expiration = c->cached.expiration;
        return S3_OK;
    }

    tstr_t token = c->token_fn(c->token_ctx);
    if (!token) return s3_error_make(-1, "Token function returned NULL");

    tstr_t body = tstr_cat_fmt(tstr_new(),
        "Action=AssumeRoleWithWebIdentity&Version=2011-06-15&RoleArn=%s&WebIdentityToken=%s",
        c->role_arn, token);
    tstr_free(token);

    s3_http_request_t req = {0};
    req.method = "POST";
    req.url = c->sts_endpoint;
    req.body = body;
    req.body_len = tstr_len(body);
    req.headers = S3Headers_init();
    s3_headers_add(&req.headers, "Content-Type", "application/x-www-form-urlencoded");

    s3_http_response_t hres = s3_http_execute(&req);
    tstr_free(body);
    S3Headers_drop(&req.headers);

    if (!s3_is_ok(hres.error)) {
        s3_error_t e = hres.error; hres.error = S3_OK;
        s3_http_response_free(&hres);
        return e;
    }
    if (hres.status_code != 200) {
        s3_error_t e = s3_error_make(hres.status_code, "STS WebIdentity failed");
        s3_http_response_free(&hres);
        return e;
    }

    s3_error_t err = sts_parse_credentials(hres.body, out);
    if (s3_is_ok(err)) {
        s3_credentials_clear(&c->cached);
        c->cached.access_key = tstr_dup(out->access_key);
        c->cached.secret_key = tstr_dup(out->secret_key);
        c->cached.session_token = out->session_token ? tstr_dup(out->session_token) : NULL;
        c->cached.expiration = out->expiration;
        c->cached_expiry = out->expiration;
    }
    s3_http_response_free(&hres);
    return err;
}

static void web_identity_destroy(void* ctx) {
    web_identity_ctx_t* c = (web_identity_ctx_t*)ctx;
    tstr_free(c->sts_endpoint); tstr_free(c->region); tstr_free(c->role_arn);
    s3_credentials_clear(&c->cached);
    free(c);
}

s3_credential_provider_t* s3_creds_web_identity(const char* sts_endpoint, const char* region,
                                                       const char* role_arn,
                                                       tstr_t (*token_fn)(void* ctx), void* ctx) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    web_identity_ctx_t* wctx = calloc(1, sizeof(web_identity_ctx_t));
    wctx->sts_endpoint = tstr_dup(sts_endpoint);
    wctx->region = region ? tstr_dup(region) : tstr_dup("us-east-1");
    wctx->role_arn = tstr_dup(role_arn);
    wctx->token_fn = token_fn;
    wctx->token_ctx = ctx;
    p->fetch = web_identity_fetch;
    p->ctx = wctx;
    p->destroy = web_identity_destroy;
    return p;
}

// ── IAM AWS Provider (EC2 instance metadata) ──

typedef struct {
    tstr_t endpoint;
    s3_credentials_t cached;
    time_t cached_expiry;
} iam_ctx_t;

static s3_error_t iam_fetch(void* ctx, s3_credentials_t* out) {
    iam_ctx_t* c = (iam_ctx_t*)ctx;

    if (c->cached.access_key && c->cached_expiry > time(NULL) + 300) {
        out->access_key = tstr_dup(c->cached.access_key);
        out->secret_key = tstr_dup(c->cached.secret_key);
        out->session_token = c->cached.session_token ? tstr_dup(c->cached.session_token) : NULL;
        out->expiration = c->cached.expiration;
        return S3_OK;
    }

    const char* ep = c->endpoint ? c->endpoint : "http://169.254.169.254";

    // Step 1: Get role name
    tstr_t role_url = tstr_cat_fmt(tstr_new(), "%s/latest/meta-data/iam/security-credentials/", ep);
    s3_http_request_t req = {0};
    req.method = "GET";
    req.url = role_url;
    req.headers = S3Headers_init();
    s3_http_response_t hres = s3_http_execute(&req);
    tstr_free(role_url);
    S3Headers_drop(&req.headers);

    if (hres.status_code != 200 || !hres.body || !hres.body[0]) {
        s3_http_response_free(&hres);
        return s3_error_make(-1, "Failed to get IAM role from instance metadata");
    }
    tstr_t role = tstr_dup(hres.body);
    // Trim whitespace
    size_t rlen = tstr_len(role);
    while (rlen > 0 && (role[rlen-1] == '\n' || role[rlen-1] == '\r' || role[rlen-1] == ' ')) role[--rlen] = '\0';
    s3_http_response_free(&hres);

    // Step 2: Get credentials for role
    tstr_t cred_url = tstr_cat_fmt(tstr_new(), "%s/latest/meta-data/iam/security-credentials/%s", ep, role);
    tstr_free(role);
    req.method = "GET";
    req.url = cred_url;
    req.headers = S3Headers_init();
    hres = s3_http_execute(&req);
    tstr_free(cred_url);
    S3Headers_drop(&req.headers);

    if (hres.status_code != 200 || !hres.body) {
        s3_http_response_free(&hres);
        return s3_error_make(-1, "Failed to get IAM credentials from instance metadata");
    }

    // Parse JSON response (simple extraction)
    tstr_t ak = NULL, sk = NULL, st = NULL;
    const char* body = hres.body;
    const char* p;
    if ((p = strstr(body, "\"AccessKeyId\"")) != NULL) {
        const char* q1 = strchr(p + 13, '"'); if (q1) { const char* q2 = strchr(q1+1, '"'); if (q2) ak = tstr_dup_len(q1+1, (size_t)(q2-q1-1)); }
    }
    if ((p = strstr(body, "\"SecretAccessKey\"")) != NULL) {
        const char* q1 = strchr(p + 17, '"'); if (q1) { const char* q2 = strchr(q1+1, '"'); if (q2) sk = tstr_dup_len(q1+1, (size_t)(q2-q1-1)); }
    }
    if ((p = strstr(body, "\"Token\"")) != NULL) {
        const char* q1 = strchr(p + 7, '"'); if (q1) { const char* q2 = strchr(q1+1, '"'); if (q2) st = tstr_dup_len(q1+1, (size_t)(q2-q1-1)); }
    }
    s3_http_response_free(&hres);

    if (!ak || !sk) {
        tstr_free(ak); tstr_free(sk); tstr_free(st);
        return s3_error_make(-1, "Failed to parse IAM credentials JSON");
    }

    out->access_key = ak;
    out->secret_key = sk;
    out->session_token = st;
    out->expiration = time(NULL) + 3600; // Refresh in 1 hour

    s3_credentials_clear(&c->cached);
    c->cached.access_key = tstr_dup(ak);
    c->cached.secret_key = tstr_dup(sk);
    c->cached.session_token = st ? tstr_dup(st) : NULL;
    c->cached.expiration = out->expiration;
    c->cached_expiry = out->expiration;
    return S3_OK;
}

static void iam_destroy(void* ctx) {
    iam_ctx_t* c = (iam_ctx_t*)ctx;
    tstr_free(c->endpoint);
    s3_credentials_clear(&c->cached);
    free(c);
}

s3_credential_provider_t* s3_creds_iam_aws(const char* custom_endpoint) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    iam_ctx_t* ctx = calloc(1, sizeof(iam_ctx_t));
    ctx->endpoint = custom_endpoint ? tstr_dup(custom_endpoint) : NULL;
    p->fetch = iam_fetch;
    p->ctx = ctx;
    p->destroy = iam_destroy;
    return p;
}

// ── LDAP Identity Provider ──

typedef struct {
    tstr_t sts_endpoint;
    tstr_t ldap_username;
    tstr_t ldap_password;
    s3_credentials_t cached;
    time_t cached_expiry;
} ldap_ctx_t;

static s3_error_t ldap_fetch(void* ctx, s3_credentials_t* out) {
    ldap_ctx_t* c = (ldap_ctx_t*)ctx;

    if (c->cached.access_key && c->cached_expiry > time(NULL) + 300) {
        out->access_key = tstr_dup(c->cached.access_key);
        out->secret_key = tstr_dup(c->cached.secret_key);
        out->session_token = c->cached.session_token ? tstr_dup(c->cached.session_token) : NULL;
        out->expiration = c->cached.expiration;
        return S3_OK;
    }

    tstr_t body = tstr_cat_fmt(tstr_new(),
        "Action=AssumeRoleWithLDAPIdentity&Version=2011-06-15&LDAPUsername=%s&LDAPPassword=%s",
        c->ldap_username, c->ldap_password);

    s3_http_request_t req = {0};
    req.method = "POST";
    req.url = c->sts_endpoint;
    req.body = body;
    req.body_len = tstr_len(body);
    req.headers = S3Headers_init();
    s3_headers_add(&req.headers, "Content-Type", "application/x-www-form-urlencoded");

    s3_http_response_t hres = s3_http_execute(&req);
    tstr_free(body);
    S3Headers_drop(&req.headers);

    if (!s3_is_ok(hres.error)) {
        s3_error_t e = hres.error; hres.error = S3_OK;
        s3_http_response_free(&hres);
        return e;
    }
    if (hres.status_code != 200) {
        s3_error_t e = s3_error_make(hres.status_code, "STS LDAP identity failed");
        s3_http_response_free(&hres);
        return e;
    }

    s3_error_t err = sts_parse_credentials(hres.body, out);
    if (s3_is_ok(err)) {
        s3_credentials_clear(&c->cached);
        c->cached.access_key = tstr_dup(out->access_key);
        c->cached.secret_key = tstr_dup(out->secret_key);
        c->cached.session_token = out->session_token ? tstr_dup(out->session_token) : NULL;
        c->cached.expiration = out->expiration;
        c->cached_expiry = out->expiration;
    }
    s3_http_response_free(&hres);
    return err;
}

static void ldap_destroy(void* ctx) {
    ldap_ctx_t* c = (ldap_ctx_t*)ctx;
    tstr_free(c->sts_endpoint); tstr_free(c->ldap_username); tstr_free(c->ldap_password);
    s3_credentials_clear(&c->cached);
    free(c);
}

s3_credential_provider_t* s3_creds_ldap_identity(const char* sts_endpoint,
                                                        const char* ldap_username, const char* ldap_password) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    ldap_ctx_t* ctx = calloc(1, sizeof(ldap_ctx_t));
    ctx->sts_endpoint = tstr_dup(sts_endpoint);
    ctx->ldap_username = tstr_dup(ldap_username);
    ctx->ldap_password = tstr_dup(ldap_password);
    p->fetch = ldap_fetch;
    p->ctx = ctx;
    p->destroy = ldap_destroy;
    return p;
}

// ── Certificate Identity Provider ──

typedef struct {
    tstr_t sts_endpoint;
    tstr_t cert_file;
    tstr_t key_file;
    s3_credentials_t cached;
    time_t cached_expiry;
} cert_ctx_t;

static s3_error_t cert_fetch(void* ctx, s3_credentials_t* out) {
    cert_ctx_t* c = (cert_ctx_t*)ctx;

    if (c->cached.access_key && c->cached_expiry > time(NULL) + 300) {
        out->access_key = tstr_dup(c->cached.access_key);
        out->secret_key = tstr_dup(c->cached.secret_key);
        out->session_token = c->cached.session_token ? tstr_dup(c->cached.session_token) : NULL;
        out->expiration = c->cached.expiration;
        return S3_OK;
    }

    // POST to STS with client certificate (mTLS)
    // The HTTP client needs to be configured with client cert.
    // For now, we send a simple POST and rely on the HTTP layer handling mTLS.
    const char* body_str = "Action=AssumeRoleWithCertificate&Version=2011-06-15";

    s3_http_request_t req = {0};
    req.method = "POST";
    req.url = c->sts_endpoint;
    req.body = body_str;
    req.body_len = strlen(body_str);
    req.headers = S3Headers_init();
    s3_headers_add(&req.headers, "Content-Type", "application/x-www-form-urlencoded");

    s3_http_response_t hres = s3_http_execute(&req);
    S3Headers_drop(&req.headers);

    if (!s3_is_ok(hres.error)) {
        s3_error_t e = hres.error; hres.error = S3_OK;
        s3_http_response_free(&hres);
        return e;
    }
    if (hres.status_code != 200) {
        s3_error_t e = s3_error_make(hres.status_code, "STS CertIdentity failed");
        s3_http_response_free(&hres);
        return e;
    }

    s3_error_t err = sts_parse_credentials(hres.body, out);
    if (s3_is_ok(err)) {
        s3_credentials_clear(&c->cached);
        c->cached.access_key = tstr_dup(out->access_key);
        c->cached.secret_key = tstr_dup(out->secret_key);
        c->cached.session_token = out->session_token ? tstr_dup(out->session_token) : NULL;
        c->cached.expiration = out->expiration;
        c->cached_expiry = out->expiration;
    }
    s3_http_response_free(&hres);
    return err;
}

static void cert_destroy(void* ctx) {
    cert_ctx_t* c = (cert_ctx_t*)ctx;
    tstr_free(c->sts_endpoint); tstr_free(c->cert_file); tstr_free(c->key_file);
    s3_credentials_clear(&c->cached);
    free(c);
}

s3_credential_provider_t* s3_creds_cert_identity(const char* sts_endpoint,
                                                        const char* cert_file, const char* key_file) {
    s3_credential_provider_t* p = calloc(1, sizeof(s3_credential_provider_t));
    cert_ctx_t* ctx = calloc(1, sizeof(cert_ctx_t));
    ctx->sts_endpoint = tstr_dup(sts_endpoint);
    ctx->cert_file = tstr_dup(cert_file);
    ctx->key_file = tstr_dup(key_file);
    p->fetch = cert_fetch;
    p->ctx = ctx;
    p->destroy = cert_destroy;
    return p;
}
