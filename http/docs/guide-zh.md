# HTTP 客户端使用指南

实用指南，涵盖常见用途和最佳实践。

## 目录

1. [基本GET请求](#基本get请求)
2. [POST请求](#post请求)
3. [自定义头](#自定义头)
4. [认证](#认证)
5. [TLS/HTTPS](#tlshttps)
6. [异步请求](#异步请求)
7. [错误处理](#错误处理)
8. [文件操作](#文件操作)
9. [Cookie管理](#cookie管理)
10. [重试策略](#重试策略)
11. [性能优化](#性能优化)

---

## 基本GET请求

最简单的使用方式：

```c
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // 创建客户端
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    // 发送 GET 请求
    http_response_t* response = http_get(client, "https://api.github.com/users/linus");

    // 检查错误
    if (response->error) {
        fprintf(stderr, "Request failed: %s\n", response->error);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("Response:\n%s\n", response->body);
    }

    // 清理
    http_response_free(response);
    http_client_destroy(client);
    return 0;
}
```

## POST请求

### 简单 POST (文本)

```c
http_response_t* response = http_post(
    client,
    "https://httpbin.org/post",
    "This is the request body",
    24  // body length
);
```

### POST JSON

```c
#include <string.h>

const char* json_data = "{\"name\":\"Alice\",\"email\":\"alice@example.com\"}";
const char* headers[] = {
    "Content-Type: application/json",
    "Accept: application/json"
};

http_response_t* response = http_request(
    client,
    HTTP_POST,
    "https://api.example.com/users",
    headers,
    2,  // header count
    json_data,
    strlen(json_data)
);

if (!response->error && response->status_code == 201) {
    printf("User created successfully\n");
}

http_response_free(response);
```

### POST URL编码表单

```c
// 方式 1: 使用参数辅助函数
http_params_t* params = http_params_create();
http_params_add(params, "username", "john");
http_params_add(params, "password", "secret");
http_params_add(params, "remember", "on");

http_response_t* response = http_post_form(client,
                                          "https://example.com/login",
                                          params);

http_params_free(params);
http_response_free(response);

// 方式 2: 手动构建
const char* form_data = "username=john&password=secret&remember=on";
const char* headers[] = {"Content-Type: application/x-www-form-urlencoded"};

http_response_t* response = http_request(
    client, HTTP_POST, "https://example.com/login",
    headers, 1,
    form_data, strlen(form_data)
);

http_response_free(response);
```

### POST 多部分表单 (文件上传)

```c
http_multipart_form_t* form = http_multipart_form_create();

// 添加文本字段
http_multipart_form_add_field(form, "username", "john");
http_multipart_form_add_field(form, "description", "My profile");

// 添加文件
http_multipart_form_add_file_path(
    form,
    "avatar",                    // 字段名
    "/path/to/profile.jpg",      // 文件路径
    "image/jpeg"                 // Content-Type
);

// 发送
http_response_t* response = http_post_multipart(
    client,
    "https://example.com/upload",
    form
);

if (!response->error) {
    printf("File uploaded: %d\n", response->status_code);
}

http_multipart_form_destroy(form);
http_response_free(response);
```

## 自定义头

### 设置单次请求头

```c
const char* headers[] = {
    "User-Agent: MyApp/1.0",
    "Accept: application/json",
    "X-Custom-Header: custom-value"
};

http_response_t* response = http_request(
    client,
    HTTP_GET,
    "https://api.example.com/data",
    headers,
    3,  // header count
    NULL,  // no body for GET
    0
);

http_response_free(response);
```

### 设置默认头 (所有请求)

```c
// 设置默认头 - 应用于之后的所有请求
http_client_set_default_header(client, "User-Agent", "MyApp/1.0");
http_client_set_default_header(client, "Accept", "application/json");
http_client_set_default_header(client, "X-API-Version", "v2");

// 这些请求会自动包含上述头
http_get(client, "https://api.example.com/users");
http_get(client, "https://api.example.com/posts");

// 移除单个头
http_client_remove_default_header(client, "X-API-Version");

// 清除所有默认头
http_client_clear_default_headers(client);
```

### 读取响应头

```c
http_response_t* response = http_get(client, "https://example.com/data");

if (!response->error) {
    // 检查特定头是否存在
    if (http_response_has_header(response, "Content-Type")) {
        char* content_type = http_response_get_header(response, "Content-Type");
        printf("Content-Type: %s\n", content_type);
        free(content_type);  // 返回的字符串需要释放
    }

    // 检查内容类型
    if (http_response_is_json(response)) {
        printf("Response is JSON\n");
    }

    // 获取 Content-Length
    size_t length = http_response_content_length(response);
    printf("Body size: %zu bytes\n", length);
}

http_response_free(response);
```

## 认证

### 基本认证

```c
http_client_t* client = http_client_create();

// 设置用户名和密码 (自动 base64 编码)
http_client_set_basic_auth(client, "username", "password");

// 现在所有请求都会包含 Authorization: Basic ...
http_response_t* response = http_get(client, "https://api.example.com/secure");

if (!response->error) {
    printf("Authenticated request succeeded\n");
}

http_response_free(response);

// 清除认证
http_client_clear_auth(client);

http_client_destroy(client);
```

### Bearer Token (JWT/OAuth)

```c
http_client_t* client = http_client_create();

// 获取 token (假设之前的登录流程)
const char* access_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...";

// 设置 Bearer token
http_client_set_bearer_token(client, access_token);

// 所有请求会包含 Authorization: Bearer <token>
http_response_t* response = http_get(client, "https://api.example.com/profile");

if (!response->error && response->status_code == 200) {
    printf("Got user profile\n");
}

http_response_free(response);
http_client_destroy(client);
```

## TLS/HTTPS

### 基本 HTTPS

```c
// 创建客户端并使用 HTTPS
http_client_t* client = http_client_create();

// 直接使用 https:// URLs
http_response_t* response = http_get(
    client,
    "https://api.github.com/users/octocat"  // 自动 TLS
);

if (!response->error) {
    printf("HTTPS request succeeded\n");
}

http_response_free(response);
http_client_destroy(client);
```

### 处理自签名证书

客户端自动验证证书。如果需要禁用验证(仅开发用途):

```c
// 注意: 这通常在源码中配置，不在 API 中暴露
// 在生产环境中应正确验证证书
```

## 异步请求

### 基本异步 GET

```c
#include "http_client_async.h"

// 响应回调
void my_response_handler(http_async_request_t* req,
                         http_async_response_t* response,
                         void* user_data) {
    if (response->error) {
        fprintf(stderr, "Request failed: %s\n", response->error);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("Body: %s\n", response->body);
    }

    // 响应在回调后自动释放，不需要手动 free
}

int main(void) {
    http_async_client_t* client = http_async_client_create();

    // 发送异步请求 (立即返回)
    http_async_get(client,
                  "https://api.example.com/data",
                  my_response_handler,
                  NULL);  // user_data

    // 应用继续运行，不等待响应
    printf("Request sent, continuing...\n");

    // 事件循环处理响应...
    // (应用的事件循环会调用回调)

    http_async_client_destroy(client);
    return 0;
}
```

### 异步 POST

```c
void post_handler(http_async_request_t* req,
                  http_async_response_t* response,
                  void* user_data) {
    if (!response->error) {
        printf("Created: %d\n", response->status_code);
    }
}

// 发送异步 POST
const char* data = "{\"name\":\"Bob\"}";

http_async_post(client,
               "https://api.example.com/users",
               data,
               strlen(data),
               post_handler,
               NULL);
```

### 异步请求取消

```c
// 发送请求
http_async_request_t* request = http_async_get(
    client,
    "https://slow-api.example.com/data",
    response_handler,
    NULL
);

// 稍后如果需要取消
http_async_request_cancel(request);
// 回调不会被调用
```

## 错误处理

### 检查错误

```c
http_response_t* response = http_get(client, url);

// 总是先检查错误
if (response->error) {
    // 请求失败
    fprintf(stderr, "Error: %s\n", response->error);
    fprintf(stderr, "Error code: %d\n", response->error_code);

    // 根据错误码处理
    switch (response->error_code) {
        case HTTP_ERROR_INVALID_URL:
            fprintf(stderr, "Invalid URL format\n");
            break;
        case HTTP_ERROR_DNS_FAILED:
            fprintf(stderr, "DNS lookup failed\n");
            break;
        case HTTP_ERROR_CONNECTION_FAILED:
            fprintf(stderr, "Cannot connect to server\n");
            break;
        case HTTP_ERROR_TIMEOUT:
            fprintf(stderr, "Request timed out\n");
            break;
        case HTTP_ERROR_TLS_HANDSHAKE_FAILED:
            fprintf(stderr, "HTTPS handshake failed\n");
            break;
        default:
            fprintf(stderr, "Other error\n");
    }
} else {
    // 请求成功 (但可能是 404, 500 等 HTTP 错误)
    printf("Status: %d\n", response->status_code);

    if (response->status_code >= 200 && response->status_code < 300) {
        printf("Success!\n");
    } else if (response->status_code >= 400) {
        printf("HTTP error: %d\n", response->status_code);
        printf("Response: %s\n", response->body);
    }
}

http_response_free(response);
```

### 设置超时

```c
http_client_t* client = http_client_create();

// 设置总超时 (默认 30 秒)
http_client_set_timeout(client, 10000);  // 10 秒

// 或分别设置连接和读取超时
http_client_set_connect_timeout(client, 5000);   // 5 秒连接
http_client_set_read_timeout(client, 15000);     // 15 秒读取

http_response_t* response = http_get(client, "https://slow-api.example.com/data");

if (response->error_code == HTTP_ERROR_TIMEOUT) {
    fprintf(stderr, "Request timed out\n");
}

http_response_free(response);
http_client_destroy(client);
```

## 文件操作

### 下载文件

```c
http_response_t* response = http_download_file(
    client,
    "https://example.com/file.zip",
    "/local/path/file.zip"  // 保存位置
);

if (!response->error) {
    printf("File downloaded successfully\n");
} else {
    fprintf(stderr, "Download failed: %s\n", response->error);
}

http_response_free(response);
```

### 上传文件

```c
http_response_t* response = http_upload_file(
    client,
    "https://api.example.com/upload",
    "/path/to/local/file.txt"  // 要上传的文件
);

if (!response->error && response->status_code == 200) {
    printf("File uploaded successfully\n");
}

http_response_free(response);
```

### 流式下载 (带进度)

```c
// 进度回调
void progress_handler(size_t downloaded, size_t total, void* user_data) {
    if (total > 0) {
        int percent = (int)(downloaded * 100 / total);
        printf("Download progress: %d%%\n", percent);
    }
}

http_client_set_progress_callback(client, progress_handler, NULL);

// 流式下载 (边下载边保存)
int write_callback(const char* data, size_t size, void* user_data) {
    FILE* fp = (FILE*)user_data;
    fwrite(data, 1, size, fp);
    return 0;  // 返回 0 继续, 非 0 中止
}

FILE* file = fopen("downloaded.bin", "wb");
http_response_t* response = http_get_stream(
    client,
    "https://example.com/large-file.bin",
    write_callback,
    file
);
fclose(file);

http_response_free(response);
```

## Cookie管理

### 自动 Cookie 处理

```c
// 创建 cookie jar
http_cookie_jar_t* jar = http_cookie_jar_create();

// 关联到客户端 (启用自动 cookie 处理)
http_client_set_cookie_jar(client, jar);

// 现在自动处理 Set-Cookie 和 Cookie 头
http_response_t* response = http_post_form(
    client,
    "https://example.com/login",
    form_data
);

// 后续请求会自动包含 cookies
response = http_get(client, "https://example.com/dashboard");

http_response_free(response);
http_client_destroy(client);
http_cookie_jar_destroy(jar);
```

### 手动管理 Cookie

```c
http_cookie_jar_t* jar = http_cookie_jar_create();

// 手动设置 cookie
http_cookie_jar_set(jar, "session_id", "abc123xyz");
http_cookie_jar_set(jar, "user_pref", "dark_mode");

// 读取 cookie
const char* session = http_cookie_jar_get(jar, "session_id");
printf("Session: %s\n", session);

// 移除 cookie
http_cookie_jar_remove(jar, "user_pref");

// 获取 cookie 数量
int count = http_cookie_jar_count(jar);
printf("Total cookies: %d\n", count);

// 清除所有 cookie
http_cookie_jar_clear(jar);

http_cookie_jar_destroy(jar);
```

## 重试策略

### 基本重试

```c
http_client_t* client = http_client_create();

// 获取默认重试策略
http_retry_policy_t policy = http_retry_policy_default();

// 自定义参数
policy.max_retries = 3;              // 最多重试 3 次
policy.initial_delay_ms = 500;       // 起始延迟 500ms
policy.exponential_backoff = 1;      // 使用指数退避
policy.retry_on_5xx = 1;             // 在 5xx 错误时重试
policy.retry_on_timeout = 1;         // 在超时时重试

http_client_set_retry_policy(client, &policy);

// 现���失败的请求会自动重试
http_response_t* response = http_get(client, "https://unreliable-api.example.com/data");

if (!response->error) {
    printf("Got response (possibly after retries)\n");
} else {
    printf("Failed after %d retries\n", policy.max_retries);
}

http_response_free(response);
http_client_destroy(client);
```

### 禁用重试

```c
// 清除重试策略
http_client_clear_retry_policy(client);

// 或设置为 0 重试
http_retry_policy_t no_retry = {
    .max_retries = 0
};
http_client_set_retry_policy(client, &no_retry);
```

## 性能优化

### 重用客户端

```c
// 好: 创建一次，重用多次
http_client_t* client = http_client_create();

for (int i = 0; i < 100; i++) {
    http_response_t* response = http_get(client, urls[i]);
    http_response_free(response);
    // 连接被池化和重用
}

http_client_destroy(client);

// 坏: 为每个请求创建新客户端
for (int i = 0; i < 100; i++) {
    http_client_t* client = http_client_create();
    http_response_t* response = http_get(client, urls[i]);
    http_response_free(response);
    http_client_destroy(client);  // 每次都新建销毁，无连接池优势
}
```

### 基础URL (减少URL重复)

```c
http_client_t* client = http_client_create();

// 设置基础 URL
http_client_set_base_url(client, "https://api.example.com/v1");

// 现在可以使用相对路径
http_get(client, "/users");           // 实际: https://api.example.com/v1/users
http_get(client, "/posts");           // 实际: https://api.example.com/v1/posts
http_post(client, "/comments", ...);  // 实际: https://api.example.com/v1/comments

http_client_destroy(client);
```

### 流式传输大文件

```c
// 坏: 加载整个文件到内存
http_response_t* response = http_get(client, "https://example.com/movie.mp4");
// response->body 现在包含完整的电影文件

// 好: 流式下载
int write_chunk(const char* data, size_t size, void* fp) {
    fwrite(data, 1, size, (FILE*)fp);
    return 0;  // 继续
}

FILE* output = fopen("movie.mp4", "wb");
http_response_t* response = http_get_stream(
    client,
    "https://example.com/movie.mp4",
    write_chunk,
    output
);
fclose(output);
// 使用恒定的内存，不管文件多大
```

### 禁用重定向 (如不需要)

```c
http_client_set_follow_redirects(client, 0);
// 避免不必要的额外请求
```

### 启用压缩

```c
http_client_enable_compression(client, 1);
// 自动处理 gzip/deflate，减少带宽
```

### 速率限制

```c
http_rate_limit_t limit = {
    .requests_per_second = 10,
    .burst_size = 20
};

http_client_set_rate_limit(client, &limit);

// 现在客户端自动限制请求速率
// 避免被 API 限流或 DDoS 防护阻止
```

---

## 完整示例

API 客户端:

```c
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    http_client_t* client = http_client_create();
    if (!client) return 1;

    // 配置
    http_client_set_base_url(client, "https://jsonplaceholder.typicode.com");
    http_client_set_user_agent(client, "MyApp/1.0");
    http_client_set_default_header(client, "Accept", "application/json");
    http_client_set_timeout(client, 5000);

    // GET 请求
    printf("=== Fetching post ===\n");
    http_response_t* response = http_get(client, "/posts/1");

    if (!response->error) {
        printf("Status: %d\n", response->status_code);
        printf("Body: %.100s...\n", response->body);
    }

    http_response_free(response);

    // POST 请求
    printf("\n=== Creating new post ===\n");
    const char* new_post = "{\"title\":\"Test\",\"body\":\"Test post\",\"userId\":1}";
    const char* headers[] = {"Content-Type: application/json"};

    response = http_request(client, HTTP_POST, "/posts",
                           headers, 1, new_post, strlen(new_post));

    if (!response->error && response->status_code == 201) {
        printf("Post created\n");
    }

    http_response_free(response);

    // 统计
    http_client_stats_t stats;
    http_client_get_stats(client, &stats);
    printf("\n=== Statistics ===\n");
    printf("Requests: %lu\n", stats.total_requests);
    printf("Bytes sent: %lu\n", stats.bytes_sent);
    printf("Bytes received: %lu\n", stats.bytes_received);

    http_client_destroy(client);
    return 0;
}
```

编译:

```bash
gcc -I./http/include example.c -o example -lturbo_http
./example
```
