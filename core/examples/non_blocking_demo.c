/**
 * Non-Blocking Event Loop Demo
 * "Real apps need to do work between I/O operations" - Practical integration
 * 
 * This demonstrates the user scenario: init -> connect -> do work -> send -> do work -> send -> stop
 * Perfect for games, GUIs, or any app with custom main loops.
 */
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "log.h"
#include "turbonet.h"

// Platform-specific sleep function
#ifdef TURBO_WIN32
#include <windows.h>
#define usleep(x) Sleep((x)/1000)
#else
#include <unistd.h>
#endif

// Application state for the demo
typedef struct {
    turbo_handle_t client;
    turbo_req_t write_req;
    bool connected;
    bool app_running;
    int work_counter;
    int messages_sent;
    int max_messages;
    char work_buffer[256];
    time_t last_work_time;
    time_t last_send_time;
} app_state_t;

static app_state_t g_app = {0};

// Forward declarations
static void simulate_application_work(void);
static void send_periodic_data(void);
static bool should_send_data(void);
static void cleanup_and_exit(void);

// Callback functions
static void client_alloc_cb(turbo_handle_t* handle, size_t size, turbo_buf_t* buf)
{
    buf->base = malloc(size);
    buf->len = buf->base ? size : 0;
}

static void client_close_cb(turbo_handle_t* handle)
{
    printf("🔴 [DISCONNECTED] Connection closed gracefully\n");
    printf("📊 [FINAL STATS] Read: %llu bytes, Written: %llu bytes\n",
           (unsigned long long)handle->bytes_read,
           (unsigned long long)handle->bytes_written);
    
    g_app.connected = false;
    g_app.app_running = false;
}

static void client_read_cb(turbo_handle_t* handle, ssize_t nread, const turbo_buf_t* buf)
{
    if (nread > 0) {
        printf("📨 [RECEIVED] %zd bytes from server: \"%.*s\"\n", 
               nread, (int)nread, buf->base);
        printf("📊 [STATS] Total received: %llu bytes\n", 
               (unsigned long long)handle->bytes_read);
    } else if (nread < 0) {
        printf("❌ [READ ERROR] Connection error: %d (%s)\n", 
               (int)nread, turbo_strerror((int)nread));
        turbo_close(handle, client_close_cb);
    }
    // nread == 0 means EOF (connection closed by peer)
    else {
        printf("🔴 [EOF] Server closed connection\n");
        turbo_close(handle, client_close_cb);
    }
    
    if (buf->base) {
        free(buf->base);
    }
}

static void client_write_cb(turbo_req_t* req, int status)
{
    if (status == 0) {
        printf("✅ [SENT] Message sent successfully!\n");
        printf("📊 [STATS] Total sent: %llu bytes\n", 
               (unsigned long long)req->handle->bytes_written);
        
        g_app.messages_sent++;
        
        // Check if we've sent enough messages
        if (g_app.messages_sent >= g_app.max_messages) {
            printf("🎯 [COMPLETE] Sent %d messages, closing connection...\n", 
                   g_app.messages_sent);
            turbo_close(req->handle, client_close_cb);
        }
    } else {
        printf("❌ [SEND ERROR] Failed to send message: %d (%s)\n", 
               status, turbo_strerror(status));
        turbo_close(req->handle, client_close_cb);
    }
}

static void client_connect_cb(turbo_handle_t* handle, int status)
{
    if (status != 0) {
        printf("❌ [CONNECTION FAILED] Error: %d (%s)\n", 
               status, turbo_strerror(status));
        g_app.app_running = false;
        return;
    }
    
    printf("🟢 [CONNECTED] Successfully connected via %s to %s:%d\n",
           turbo_transport_name(handle->transport),
           handle->remote_ip,
           handle->remote_port);
    
    g_app.connected = true;
    
    // Start reading for server responses
    turbo_read_start(handle, client_alloc_cb, client_read_cb);
    
    // Initialize timing
    time(&g_app.last_work_time);
    time(&g_app.last_send_time);
}

// Simulate application work (game logic, UI updates, etc.)
static void simulate_application_work(void)
{
    time_t now;
    time(&now);
    
    // Do work every second
    if (now - g_app.last_work_time >= 1) {
        g_app.work_counter++;
        
        // Simulate different types of work
        switch (g_app.work_counter % 4) {
            case 0:
                snprintf(g_app.work_buffer, sizeof(g_app.work_buffer), 
                        "Processing game logic (frame %d)", g_app.work_counter);
                break;
            case 1:
                snprintf(g_app.work_buffer, sizeof(g_app.work_buffer), 
                        "Updating UI elements (tick %d)", g_app.work_counter);
                break;
            case 2:
                snprintf(g_app.work_buffer, sizeof(g_app.work_buffer), 
                        "Rendering graphics (render %d)", g_app.work_counter);
                break;
            case 3:
                snprintf(g_app.work_buffer, sizeof(g_app.work_buffer), 
                        "Processing user input (input %d)", g_app.work_counter);
                break;
        }
        
        printf("⚙️  [WORK] %s\n", g_app.work_buffer);
        g_app.last_work_time = now;
    }
}

static bool should_send_data(void)
{
    time_t now;
    time(&now);
    
    // Send data every 3 seconds, and only if connected
    return g_app.connected && (now - g_app.last_send_time >= 3);
}

static void send_periodic_data(void)
{
    if (!should_send_data()) {
        return;
    }
    
    // Create message with current work state
    char message[512];
    snprintf(message, sizeof(message), 
            "Message %d: %s [timestamp: %ld]", 
            g_app.messages_sent + 1, 
            g_app.work_buffer, 
            time(NULL));
    
    turbo_buf_t buf = turbo_buf_init(message, strlen(message));
    
    printf("📤 [SENDING] \"%s\" (%zu bytes)\n", message, strlen(message));
    
    int err = turbo_write(&g_app.write_req, &g_app.client, &buf, 1, client_write_cb);
    if (err != 0) {
        printf("❌ [SEND ERROR] turbo_write failed: %d (%s)\n", 
               err, turbo_strerror(err));
        g_app.app_running = false;
    } else {
        time(&g_app.last_send_time);
    }
}

static void cleanup_and_exit(void)
{
    printf("🧹 [CLEANUP] Shutting down application...\n");
    
    if (g_app.connected) {
        turbo_close(&g_app.client, client_close_cb);
        
        // Process any remaining events
        printf("⏳ [CLEANUP] Processing final events...\n");
        for (int i = 0; i < 10 && g_app.connected; i++) {
            turbo_run_once();
            usleep(10000); // 10ms
        }
    }
    
    turbo_global_cleanup();
    printf("✅ [EXIT] Application shutdown complete\n");
}

// Main application demonstration
static void demo_non_blocking_client(const char* url, int max_messages)
{
    printf("\n🚀 === Non-Blocking Event Loop Demo ===\n");
    printf("📋 Target: %s\n", url);
    printf("📊 Max messages: %d\n", max_messages);
    printf("⚡ Pattern: init -> connect -> work -> send -> work -> send -> stop\n\n");
    
    // Initialize application state
    g_app.connected = false;
    g_app.app_running = true;
    g_app.work_counter = 0;
    g_app.messages_sent = 0;
    g_app.max_messages = max_messages;
    memset(g_app.work_buffer, 0, sizeof(g_app.work_buffer));
    
    // 1. INIT: Initialize TurboNet global state and handle
    printf("🔧 [INIT] Initializing TurboNet...\n");
    turbo_global_init(&g_app.client);
    
    // 2. CONNECT: Start connection (non-blocking)
    printf("🔗 [CONNECT] Connecting to %s...\n", url);
    int err = turbo_connect_url(&g_app.client, url, client_connect_cb);
    if (err != 0) {
        printf("❌ [INIT ERROR] turbo_connect_url failed: %d (%s)\n", 
               err, turbo_strerror(err));
        cleanup_and_exit();
        return;
    }
    
    printf("⏳ [STARTED] Connection initiated, entering main loop...\n\n");
    
    // 3. MAIN LOOP: The core pattern - work and I/O interleaved
    int loop_count = 0;
    while (g_app.app_running) {
        loop_count++;
        
        // Process network events (non-blocking!)
        turbo_run_once();
        
        // Do application work (simulated)
        simulate_application_work();
        
        // Send data when appropriate
        send_periodic_data();
        
        // Simulate frame rate control (60 FPS for games)
        usleep(16667); // ~16.67ms = 60 FPS
        
        // Debug output every 60 iterations (1 second at 60 FPS)
        if (loop_count % 60 == 0) {
            printf("🔄 [LOOP] Iteration %d, Connected: %s, Messages sent: %d\n", 
                   loop_count, g_app.connected ? "Yes" : "No", g_app.messages_sent);
        }
        
        // Safety exit after reasonable time
        if (loop_count > 3600) { // 1 minute at 60 FPS
            printf("⏰ [TIMEOUT] Demo time limit reached, exiting...\n");
            break;
        }
    }
    
    // 4. STOP: Clean shutdown
    cleanup_and_exit();
    
    printf("\n📈 [SUMMARY] Demo Statistics:\n");
    printf("   🔄 Loop iterations: %d\n", loop_count);
    printf("   ⚙️  Work cycles: %d\n", g_app.work_counter);
    printf("   📤 Messages sent: %d\n", g_app.messages_sent);
    printf("   🎯 Target messages: %d\n", g_app.max_messages);
    printf("   ✅ Success: %s\n", (g_app.messages_sent >= g_app.max_messages) ? "Yes" : "Partial");
}

// Alternative demo showing different integration patterns
static void demo_integration_patterns(void)
{
    printf("\n🎯 === Integration Pattern Examples ===\n");
    printf("Showing how to integrate TurboNet with different app types:\n\n");
    
    printf("🎮 [GAME PATTERN]\n");
    printf("while (game_running) {\n");
    printf("    turbo_run_once();        // Process network\n");
    printf("    process_input();         // Handle user input\n");
    printf("    update_game_logic();     // Game physics/logic\n");
    printf("    render_frame();          // Draw to screen\n");
    printf("    if (need_network_send) {\n");
    printf("        turbo_write(...);    // Send game data\n");
    printf("    }\n");
    printf("    limit_fps(60);           // Frame rate control\n");
    printf("}\n\n");
    
    printf("🖥️  [GUI APPLICATION PATTERN]\n");
    printf("while (app_running) {\n");
    printf("    turbo_run_once();        // Process network\n");
    printf("    process_gui_events();    // Handle UI events\n");
    printf("    update_widgets();        // Update UI state\n");
    printf("    if (user_clicked_send) {\n");
    printf("        turbo_write(...);    // Send user data\n");
    printf("    }\n");
    printf("    thread_yield();          // Cooperative scheduling\n");
    printf("}\n\n");
    
    printf("🖥️  [SERVER PATTERN]\n");
    printf("while (server_running) {\n");
    printf("    turbo_run_once();        // Process connections\n");
    printf("    process_business_logic(); // Handle requests\n");
    printf("    update_metrics();        // Update statistics\n");
    printf("    cleanup_expired_sessions(); // Maintenance\n");
    printf("    usleep(1000);            // 1ms pause\n");
    printf("}\n\n");
    
    printf("🔄 [COMPARISON: Blocking vs Non-Blocking]\n");
    printf("❌ BLOCKING (Old way):\n");
    printf("   turbo_run(); // Blocks forever - can't do other work!\n\n");
    printf("✅ NON-BLOCKING (New way):\n");
    printf("   while (running) {\n");
    printf("       turbo_run_once(); // Returns immediately\n");
    printf("       do_my_work();     // Your code runs here\n");
    printf("   }\n\n");
}

int main(int argc, char* argv[])
{
    // Set reasonable log level
    log_set_level(LOG_INFO);
    
    printf("🚀 TurboNet Non-Blocking Event Loop Demo\n");
    printf("==========================================\n");
    printf("\"Real applications need to work while networking\" - Practical Design\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <mode> [url] [messages]\n");
        printf("Modes:\n");
        printf("  patterns  - Show integration patterns\n");
        printf("  demo      - Run live demo\n");
        printf("\nExamples:\n");
        printf("  %s patterns\n", argv[0]);
        printf("  %s demo tcp://127.0.0.1:8080 5\n", argv[0]);
        printf("  %s demo tls://httpbin.org:443 3\n", argv[0]);
        return 1;
    }
    
    const char* mode = argv[1];
    
    if (strcmp(mode, "patterns") == 0) {
        demo_integration_patterns();
    } else if (strcmp(mode, "demo") == 0) {
        const char* url = argc > 2 ? argv[2] : "tcp://127.0.0.1:8080";
        int max_messages = argc > 3 ? atoi(argv[3]) : 3;
        
        if (max_messages <= 0) {
            max_messages = 3;
        }
        
        demo_non_blocking_client(url, max_messages);
    } else {
        printf("❌ Unknown mode: %s\n", mode);
        return 1;
    }
    
    return 0;
}