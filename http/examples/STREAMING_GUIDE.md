# Streaming File Transfer Guide

## Overview

TurboNet HTTP client provides two modes for file transfer:

1. **Standard mode** - Loads entire file into memory
2. **Streaming mode** - Processes file in 8KB chunks (O(1) memory)

## When to Use Streaming

Use streaming when:
- File size > 10MB
- Memory is constrained
- You need progress feedback
- Handling user uploads/downloads

Use standard when:
- File size < 1MB
- Simplicity is priority
- No progress tracking needed

## API Comparison

### Standard Upload/Download

```c
// Upload entire file at once
http_response_t *resp = http_upload_file(client, url, "large_file.bin");

// Download entire file at once
http_response_t *resp = http_download_file(client, url, "output.bin");
```

**Memory usage**: O(file_size)

### Streaming Upload/Download

```c
// Progress callback
void progress_cb(size_t transferred, size_t total, void *user_data) {
    printf("Progress: %.1f%%\n", (transferred * 100.0) / total);
}

// Upload with streaming
http_response_t *resp = http_upload_file_stream(
    client, url, "large_file.bin",
    progress_cb, NULL
);

// Download with streaming
http_response_t *resp = http_download_file_stream(
    client, url, "output.bin",
    progress_cb, NULL
);
```

**Memory usage**: O(1) - fixed 8KB buffer

## Memory Efficiency Example

| File Size | Standard Mode | Streaming Mode | Savings |
|-----------|---------------|----------------|---------|
| 10 MB     | 10 MB RAM     | 8 KB RAM       | 1,280x  |
| 100 MB    | 100 MB RAM    | 8 KB RAM       | 12,800x |
| 1 GB      | 1 GB RAM      | 8 KB RAM       | 131,072x|

## Complete Example

```c
#include "http_client.h"
#include <stdio.h>

typedef struct {
    const char *filename;
    size_t last_percent;
} upload_ctx_t;

void upload_progress(size_t transferred, size_t total, void *user_data) {
    upload_ctx_t *ctx = (upload_ctx_t *)user_data;
    size_t percent = (transferred * 100) / total;

    if (percent >= ctx->last_percent + 10 || transferred == total) {
        printf("[%s] %zu/%zu bytes (%.1f%%)\n",
               ctx->filename, transferred, total, (double)percent);
        ctx->last_percent = percent;
    }
}

int main(void) {
    http_client_t *client = http_client_create("https://example.com");
    http_client_set_timeout(client, 60000);  // 60 seconds

    upload_ctx_t ctx = {
        .filename = "video.mp4",
        .last_percent = 0
    };

    // Upload large video file with progress
    http_response_t *resp = http_upload_file_stream(
        client, "/api/upload", "video.mp4",
        upload_progress, &ctx
    );

    if (resp->error_code == HTTP_ERROR_NONE) {
        printf("Upload successful! Status: %d\n", resp->status_code);
    } else {
        printf("Upload failed: %s\n", resp->error);
    }

    http_response_free(resp);
    http_client_destroy(client);
    return 0;
}
```

## Error Handling

Both streaming functions return `HTTP_ERROR_FILE_IO` for file system errors:

```c
http_response_t *resp = http_upload_file_stream(
    client, url, "missing_file.bin", NULL, NULL
);

if (resp->error_code == HTTP_ERROR_FILE_IO) {
    printf("File error: %s\n", resp->error);
}
```

## Progress Callback Details

The progress callback signature:

```c
typedef void (*http_progress_cb)(size_t downloaded, size_t total, void *user_data);
```

- `downloaded`: Bytes transferred so far
- `total`: Total file size (0 if unknown for downloads)
- `user_data`: Your custom context pointer

**Note**: For downloads, `total` may be 0 if the server doesn't send `Content-Length` header.

## Performance Tips

1. **Timeout**: Set appropriate timeout for large files
   ```c
   http_client_set_timeout(client, 300000);  // 5 minutes
   ```

2. **Progress throttling**: Don't update UI on every callback
   ```c
   // Report every 10% instead of every 8KB
   if (percent >= last_percent + 10) {
       update_ui(percent);
   }
   ```

3. **Error recovery**: Check `error_code` before accessing response body
   ```c
   if (resp->error_code != HTTP_ERROR_NONE) {
       // Handle error
   }
   ```

## Running the Example

```bash
cd build
./file_transfer_stream_example
```

The example demonstrates:
- Small file upload (10 KB)
- Large file upload (1 MB)
- File download with progress
- Error handling (missing files, invalid paths)
- Memory efficiency comparison

## See Also

- `file_transfer_example.c` - Standard mode examples
- `http_client.h` - Full API reference
- `advanced_features_example.c` - Other HTTP features
