/**
 * Echo Client Benchmark Tool
 * "Stress test the echo servers - measure what matters" - Linus
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <uv.h>

// Test configuration
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_CONNECTIONS 10      // Reduced default for stability
#define DEFAULT_MESSAGES 100        // Reduced default
#define DEFAULT_CONNECTION_DELAY 10 // Milliseconds between connections
#define TEST_MESSAGE "Hello, Echo Server! This is a benchmark message with some data to transfer."

// Connection state
typedef struct {
    uv_tcp_t handle;
    uv_connect_t connect_req;
    uv_write_t write_req;
    int messages_sent;
    int messages_received;
    int target_messages;
    char* message_data;
    size_t message_size;
} client_conn_t;

// Global test state
static struct {
    int target_connections;
    int active_connections;
    int completed_connections;
    uint64_t total_messages_sent;
    uint64_t total_messages_received;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    time_t start_time;
    volatile int should_stop;
} g_test = {0};

// Forward declarations
static void connect_cb(uv_connect_t* req, int status);
static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
static void write_cb(uv_write_t* req, int status);
static void close_cb(uv_handle_t* handle);
static void send_message(client_conn_t* conn);
static void signal_handler(int signum);

// Send a message to the server
static void send_message(client_conn_t* conn)
{
    if (conn->messages_sent >= conn->target_messages) {
        // Done with this connection
        uv_close((uv_handle_t*)&conn->handle, close_cb);
        return;
    }

    uv_buf_t buf = uv_buf_init(conn->message_data, conn->message_size);
    int err = uv_write(&conn->write_req, (uv_stream_t*)&conn->handle, &buf, 1, write_cb);
    
    if (err == 0) {
        conn->messages_sent++;
        g_test.total_messages_sent++;
        g_test.total_bytes_sent += conn->message_size;
    } else {
        fprintf(stderr, "Write error: %s\n", uv_strerror(err));
        uv_close((uv_handle_t*)&conn->handle, close_cb);
    }
}

// Write completion callback
static void write_cb(uv_write_t* req, int status)
{
    if (status != 0) {
        fprintf(stderr, "Write callback error: %s\n", uv_strerror(status));
    }
    // Continue sending or wait for echo - we'll send next message when we receive echo
}

// Buffer allocation callback
static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = buf->base ? suggested_size : 0;
}

// Read callback - received echo from server
static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    client_conn_t* conn = (client_conn_t*)stream->data;
    
    if (nread > 0) {
        conn->messages_received++;
        g_test.total_messages_received++;
        g_test.total_bytes_received += nread;
        
        // Send next message after receiving echo
        send_message(conn);
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
        }
        uv_close((uv_handle_t*)stream, close_cb);
    }

    if (buf->base) {
        free(buf->base);
    }
}

// Connection closed
static void close_cb(uv_handle_t* handle)
{
    client_conn_t* conn = (client_conn_t*)handle->data;
    
    g_test.active_connections--;
    g_test.completed_connections++;
    
    if (conn->message_data) {
        free(conn->message_data);
    }
    free(conn);

    // Check if all connections are done
    if (g_test.completed_connections >= g_test.target_connections) {
        uv_stop(uv_default_loop());
    }
}

// Connection established callback
static void connect_cb(uv_connect_t* req, int status)
{
    client_conn_t* conn = (client_conn_t*)req->handle;
    
    if (status != 0) {
        fprintf(stderr, "Connection failed: %s\n", uv_strerror(status));
        uv_close((uv_handle_t*)req->handle, close_cb);
        return;
    }

    // Start reading responses
    uv_read_start((uv_stream_t*)&conn->handle, alloc_cb, read_cb);
    
    // Send first message
    send_message(conn);
}

// Signal handler
static void signal_handler(int signum)
{
    (void)signum;
    printf("\nStopping benchmark...\n");
    g_test.should_stop = 1;
    uv_stop(uv_default_loop());
}

// Create a new client connection
static int create_client_connection(const char* host, int port, int messages_per_conn)
{
    client_conn_t* conn = malloc(sizeof(client_conn_t));
    if (!conn) {
        return -1;
    }

    memset(conn, 0, sizeof(*conn));
    conn->target_messages = messages_per_conn;
    conn->message_size = strlen(TEST_MESSAGE);
    conn->message_data = malloc(conn->message_size + 1);
    
    if (!conn->message_data) {
        free(conn);
        return -1;
    }
    
    strcpy(conn->message_data, TEST_MESSAGE);

    // Initialize TCP handle
    int err = uv_tcp_init(uv_default_loop(), &conn->handle);
    if (err != 0) {
        free(conn->message_data);
        free(conn);
        return err;
    }

    // Set connection as handle data for callbacks
    conn->handle.data = conn;
    conn->connect_req.data = conn;
    conn->write_req.data = conn;

    // Parse server address
    struct sockaddr_in addr;
    err = uv_ip4_addr(host, port, &addr);
    if (err != 0) {
        uv_close((uv_handle_t*)&conn->handle, close_cb);
        return err;
    }

    // Connect to server
    err = uv_tcp_connect(&conn->connect_req, &conn->handle, (const struct sockaddr*)&addr, connect_cb);
    if (err != 0) {
        uv_close((uv_handle_t*)&conn->handle, close_cb);
        return err;
    }

    g_test.active_connections++;
    return 0;
}

int main(int argc, char* argv[])
{
    const char* host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    int connections = DEFAULT_CONNECTIONS;
    int messages = DEFAULT_MESSAGES;
    
    // Parse command line arguments
    if (argc > 1) {
        connections = atoi(argv[1]);
        if (connections <= 0) {
            fprintf(stderr, "Invalid connection count: %s\n", argv[1]);
            return 1;
        }
    }
    if (argc > 2) {
        messages = atoi(argv[2]);
        if (messages <= 0) {
            fprintf(stderr, "Invalid message count: %s\n", argv[2]);
            return 1;
        }
    }
    if (argc > 3) {
        host = argv[3];
    }
    if (argc > 4) {
        port = atoi(argv[4]);
        if (port <= 0) {
            fprintf(stderr, "Invalid port: %s\n", argv[4]);
            return 1;
        }
    }

    g_test.target_connections = connections;

    printf("Echo Client Benchmark\n");
    printf("Target: %s:%d\n", host, port);
    printf("Config: %d connections, %d messages each\n", connections, messages);
    printf("Message size: %zu bytes\n", strlen(TEST_MESSAGE));
    printf("Total messages: %d\n", connections * messages);
    printf("Press Ctrl+C to stop early\n\n");

    // Setup signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Record start time
    g_test.start_time = time(NULL);

    // Create all client connections
    for (int i = 0; i < connections; i++) {
        int err = create_client_connection(host, port, messages);
        if (err != 0) {
            fprintf(stderr, "Failed to create connection %d: %s\n", i, uv_strerror(err));
            // Continue with other connections
        }
    }

    printf("Started %d connections...\n", g_test.active_connections);

    // Run event loop until all connections complete
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

    // Print final results
    time_t end_time = time(NULL);
    double elapsed = difftime(end_time, g_test.start_time);
    
    printf("\n=== Benchmark Results ===\n");
    printf("Runtime: %.2f seconds\n", elapsed);
    printf("Connections: %d completed out of %d target\n", 
           g_test.completed_connections, g_test.target_connections);
    printf("Messages: %llu sent, %llu received\n",
           (unsigned long long)g_test.total_messages_sent,
           (unsigned long long)g_test.total_messages_received);
    printf("Bandwidth: %.2f MB sent, %.2f MB received\n",
           g_test.total_bytes_sent / (1024.0 * 1024.0),
           g_test.total_bytes_received / (1024.0 * 1024.0));
    
    if (elapsed > 0) {
        printf("Throughput: %.2f msgs/sec, %.2f MB/sec total\n",
               (g_test.total_messages_sent + g_test.total_messages_received) / elapsed,
               (g_test.total_bytes_sent + g_test.total_bytes_received) / (1024.0 * 1024.0 * elapsed));
    }
    
    double success_rate = (g_test.total_messages_received * 100.0) / g_test.total_messages_sent;
    printf("Success rate: %.1f%% (echo responses received)\n", success_rate);
    printf("========================\n");

    return 0;
}