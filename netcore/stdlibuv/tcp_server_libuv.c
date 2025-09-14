/**
 * Pure libuv Echo Server Benchmark
 * "The reference implementation - no abstractions, just raw libuv" - Linus
 * 
 * Direct libuv TCP echo server for performance baseline comparison
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <uv.h>

// Benchmark configuration
#define DEFAULT_PORT 8080
#define DEFAULT_HOST "0.0.0.0"
#define BACKLOG 128

// Client connection structure
typedef struct {
    uv_tcp_t handle;
    uv_write_t write_req;
} client_t;

// Global statistics
static struct {
    uint64_t connections_total;
    uint64_t connections_active;
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t messages_processed;
    time_t start_time;
    volatile int should_stop;
} g_stats = {0};

static uv_tcp_t g_server;
static uv_timer_t g_stats_timer;

// Forward declarations
static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
static void write_cb(uv_write_t* req, int status);
static void close_cb(uv_handle_t* handle);
static void connection_cb(uv_stream_t* server, int status);
static void print_stats(uv_timer_t* handle);
static void signal_handler(int signum);

// Buffer allocation callback
static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = buf->base ? suggested_size : 0;
}

// Write completion callback
static void write_cb(uv_write_t* req, int status)
{
    if (status == 0) {
        g_stats.messages_processed++;
    }
    
    // Free the buffer data that was allocated for this write
    if (req->data) {
        free(req->data);
    }
}

// Client read callback - echo back received data
static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    client_t* client = (client_t*)stream;
    
    if (nread > 0) {
        g_stats.bytes_received += nread;
        
        // Prepare echo buffer - make a copy since we need to free the read buffer
        char* echo_data = malloc(nread);
        if (echo_data) {
            memcpy(echo_data, buf->base, nread);
            
            uv_buf_t echo_buf = uv_buf_init(echo_data, nread);
            
            // Store echo_data in write_req.data for cleanup in write_cb
            client->write_req.data = echo_data;
            
            int err = uv_write(&client->write_req, stream, &echo_buf, 1, write_cb);
            if (err == 0) {
                g_stats.bytes_sent += nread;
            } else {
                free(echo_data);
            }
        }
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
        }
        // Close the connection
        uv_close((uv_handle_t*)stream, close_cb);
    }

    // Free read buffer
    if (buf->base) {
        free(buf->base);
    }
}

// Client connection closed
static void close_cb(uv_handle_t* handle)
{
    printf("[DEBUG] libuv client closing, active: %llu -> %llu\n", 
           (unsigned long long)g_stats.connections_active, 
           (unsigned long long)(g_stats.connections_active - 1));
    g_stats.connections_active--;
    free(handle);
}

// New connection callback
static void connection_cb(uv_stream_t* server, int status)
{
    if (status < 0) {
        fprintf(stderr, "Connection error: %s\n", uv_strerror(status));
        return;
    }

    // Allocate client structure
    client_t* client = malloc(sizeof(client_t));
    if (!client) {
        return;
    }

    // Initialize client TCP handle
    int err = uv_tcp_init(uv_default_loop(), &client->handle);
    if (err != 0) {
        free(client);
        return;
    }

    // Accept the connection
    err = uv_accept(server, (uv_stream_t*)&client->handle);
    if (err != 0) {
        uv_close((uv_handle_t*)&client->handle, close_cb);
        return;
    }

    g_stats.connections_total++;
    g_stats.connections_active++;
    
    printf("[DEBUG] libuv client accepted, active: %llu, total: %llu\n", 
           (unsigned long long)g_stats.connections_active, 
           (unsigned long long)g_stats.connections_total);

    // Start reading from client
    printf("[DEBUG] libuv starting read on client %llu\n", (unsigned long long)g_stats.connections_total);
    uv_read_start((uv_stream_t*)&client->handle, alloc_cb, read_cb);
}

// Print performance statistics
static void print_stats(uv_timer_t* handle)
{
    (void)handle;
    
    time_t now = time(NULL);
    double elapsed = difftime(now, g_stats.start_time);
    
    printf("\n=== Pure libuv Echo Server Benchmark Stats ===\n");
    printf("Runtime: %.0f seconds\n", elapsed);
    printf("Connections: %llu total, %llu active\n", 
           (unsigned long long)g_stats.connections_total,
           (unsigned long long)g_stats.connections_active);
    printf("Messages: %llu processed\n", (unsigned long long)g_stats.messages_processed);
    printf("Bandwidth: %.2f MB received, %.2f MB sent\n",
           g_stats.bytes_received / (1024.0 * 1024.0),
           g_stats.bytes_sent / (1024.0 * 1024.0));
    
    if (elapsed > 0) {
        printf("Throughput: %.2f msgs/sec, %.2f MB/sec\n",
               g_stats.messages_processed / elapsed,
               (g_stats.bytes_received + g_stats.bytes_sent) / (1024.0 * 1024.0 * elapsed));
    }
    printf("=============================================\n");
}

// Signal handler for graceful shutdown
static void signal_handler(int signum)
{
    (void)signum;
    printf("\nShutdown signal received...\n");
    g_stats.should_stop = 1;
    uv_stop(uv_default_loop());
}

int main(int argc, char* argv[])
{
    const char* host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    
    // Parse command line arguments
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
    }
    if (argc > 2) {
        host = argv[2];
    }

    printf("Pure libuv Echo Server Benchmark\n");
    printf("Listening on %s:%d\n", host, port);
    printf("Press Ctrl+C to stop and show final stats\n\n");

    // Setup signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize server TCP handle
    int err = uv_tcp_init(uv_default_loop(), &g_server);
    if (err != 0) {
        fprintf(stderr, "uv_tcp_init failed: %s\n", uv_strerror(err));
        return 1;
    }

    // Parse address
    struct sockaddr_in addr;
    err = uv_ip4_addr(host, port, &addr);
    if (err != 0) {
        fprintf(stderr, "uv_ip4_addr failed: %s\n", uv_strerror(err));
        return 1;
    }

    // Bind to address
    err = uv_tcp_bind(&g_server, (const struct sockaddr*)&addr, 0);
    if (err != 0) {
        fprintf(stderr, "uv_tcp_bind failed: %s\n", uv_strerror(err));
        return 1;
    }

    // Start listening
    err = uv_listen((uv_stream_t*)&g_server, BACKLOG, connection_cb);
    if (err != 0) {
        fprintf(stderr, "uv_listen failed: %s\n", uv_strerror(err));
        return 1;
    }

    // Setup stats timer - print stats every 10 seconds
    uv_timer_init(uv_default_loop(), &g_stats_timer);
    uv_timer_start(&g_stats_timer, print_stats, 10000, 10000);

    // Record start time
    g_stats.start_time = time(NULL);

    printf("Server started successfully, waiting for connections...\n");

    // Run event loop
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

    // Print final stats
    print_stats(NULL);

    printf("Server shutdown complete.\n");
    return 0;
}