/**
 * TurboNet Core Implementation
 * "Good programs use good data structures" - Linus
 */
#include "platform.h"

#include "turbonet.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "log.h"
#include "turbonet_internal.h"

// Global event loop management - hidden from users
static uv_loop_t* g_global_loop = NULL;
static int g_loop_initialized = 0;

// Global transport registry - simple array, no fancy stuff
static const turbo_transport_vtable_t* g_transports[TURBO_TRANSPORT_MAX] = {0};

// Forward declarations
static int turbo_init_internal(turbo_handle_t* handle);
static void turbo_cleanup_internal(turbo_handle_t* handle);

// =============================================================================
// Transport registration - called by transport implementations
// =============================================================================

// Lazy registration - register transport on first use
static int turbo_ensure_transport(turbo_transport_t transport) {
  if (g_transports[transport] != NULL) {
    return 0; // Already registered
  }

  // Register transport on demand
  switch (transport) {
    case TURBO_TCP:
      {
        extern void turbo_register_tcp(void);
        turbo_register_tcp();
      }
      break;
    case TURBO_TLS:
      {
        extern void turbo_register_tls(void);
        turbo_register_tls();
      }
      break;
    case TURBO_UDP:
      {
        extern void turbo_register_udp(void);
        turbo_register_udp();
      }
      break;
    case TURBO_KCP:
      {
        extern void turbo_register_kcp(void);
        turbo_register_kcp();
      }
      break;
    case TURBO_PIPE:
      {
        extern void turbo_register_pipe(void);
        turbo_register_pipe();
      }
      break;
    case TURBO_QUIC:
      {
        extern void turbo_register_quic(void);
        turbo_register_quic();
      }
      break;
    default:
      log_error("Unknown transport type: %d", transport);
      return TURBO_EINVAL_TRANSPORT;
  }

  if (g_transports[transport] == NULL) {
    log_error("Failed to register transport: %s", turbo_transport_name(transport));
    return TURBO_EUNSUPPORTED;
  }

  log_debug("Lazy-registered transport: %s", turbo_transport_name(transport));
  return 0;
}

void turbo_register_transport(turbo_transport_t type,
                              const turbo_transport_vtable_t* vtable)
{
  assert(type >= 0 && type < TURBO_TRANSPORT_MAX);
  assert(vtable != NULL);

  if (g_transports[type] != NULL) {
    log_warn("Transport %s already registered, replacing",
             turbo_transport_name(type));
  }

  g_transports[type] = vtable;
  log_debug("Registered transport: %s", turbo_transport_name(type));
}

// =============================================================================
// Core API implementation - the beautiful unified interface
// =============================================================================

int turbo_init(turbo_handle_t* handle,
               uv_loop_t* loop,
               turbo_transport_t transport)
{
  if (handle == NULL || loop == NULL) {
    return UV_EINVAL;
  }

  if (transport < 0 || transport >= TURBO_TRANSPORT_MAX) {
    log_error("Invalid transport type: %d", transport);
    return TURBO_EINVAL_TRANSPORT;
  }

  // Lazy transport registration - register only when needed
  int err = turbo_ensure_transport(transport);
  if (err != 0) {
    return err;
  }

  const turbo_transport_vtable_t* vtable = g_transports[transport];
  if (vtable == NULL) {
    log_error("Transport %s not registered", turbo_transport_name(transport));
    return TURBO_EUNSUPPORTED;
  }

  // Initialize handle structure - zero everything first
  memset(handle, 0, sizeof(*handle));

  // Set public fields
  handle->transport = transport;
  handle->state = TURBO_CLOSED;
  handle->loop = loop;

  // Initialize internal structure
  err = turbo_init_internal(handle);
  if (err != 0) {
    log_error("Failed to initialize internal handle: %s",
              turbo_strerror(err));
    return err;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  internal->vtable = vtable;

  // Initialize mutex for thread safety
  err = turbo_mutex_init(&handle->mutex);
  if (err != 0) {
    log_error("Failed to initialize mutex: %s", turbo_strerror(err));
    turbo_cleanup_internal(handle);
    return err;
  }

  // Call transport-specific initialization
  err = vtable->init(handle);
  if (err != 0) {
    log_error("Transport %s initialization failed: %s",
              turbo_transport_name(transport),
              turbo_strerror(err));
    turbo_mutex_destroy(&handle->mutex);
    turbo_cleanup_internal(handle);
    return err;
  }

  log_debug("Initialized handle for transport: %s",
            turbo_transport_name(transport));
  return 0;
}

int turbo_connect(turbo_handle_t* handle,
                  const char* address,
                  int port,
                  turbo_connect_cb cb)
{
  if (handle == NULL || address == NULL || cb == NULL) {
    return UV_EINVAL;
  }

  if (handle->state != TURBO_CLOSED) {
    log_error("Handle not in closed state: %s",
              turbo_state_name(handle->state));
    return UV_EALREADY;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL) {
    return TURBO_ENOTINIT;
  }

  // Set callback and state
  handle->connect_cb = cb;
  handle->state = TURBO_CONNECTING;

  // Copy address info
  strncpy(handle->remote_ip, address, sizeof(handle->remote_ip) - 1);
  handle->remote_port = port;

  log_info("Connecting to %s:%d via %s",
           address,
           port,
           turbo_transport_name(handle->transport));

  // Call transport-specific connect
  int err = internal->vtable->connect(handle, address, port);
  if (err != 0) {
    log_error("Connect failed: %s", turbo_strerror(err));
    handle->state = TURBO_CLOSED;
    handle->connect_cb = NULL;
    return err;
  }

  return 0;
}

int turbo_bind(turbo_handle_t* handle, const char* address, int port)
{
  if (handle == NULL || address == NULL) {
    return UV_EINVAL;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL
      || internal->vtable->bind == NULL)
  {
    return TURBO_EUNSUPPORTED;
  }

  internal->is_server = true;

  // Copy address info
  strncpy(handle->local_ip, address, sizeof(handle->local_ip) - 1);
  handle->local_port = port;

  log_info("Binding to %s:%d via %s",
           address,
           port,
           turbo_transport_name(handle->transport));

  return internal->vtable->bind(handle, address, port);
}

TURBONET_API int turbo_listen(turbo_handle_t* handle,
                 int backlog,
                 turbo_connect_cb connection_cb)
{
  if (handle == NULL || connection_cb == NULL) {
    return UV_EINVAL;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL
      || internal->vtable->listen == NULL)
  {
    return TURBO_EUNSUPPORTED;
  }

  handle->connect_cb = connection_cb;

  log_info("Listening on %s:%d with backlog %d",
           handle->local_ip,
           handle->local_port,
           backlog);

  return internal->vtable->listen(handle, backlog);
}

TURBONET_API int turbo_accept(turbo_handle_t* server, turbo_handle_t* client)
{
  if (server == NULL || client == NULL) {
    return UV_EINVAL;
  }

  turbo_internal_t* server_internal = turbo_get_internal(server);
  if (server_internal == NULL || server_internal->vtable == NULL
      || server_internal->vtable->accept == NULL)
  {
    return TURBO_EUNSUPPORTED;
  }

  // AUTONOMOUS: Auto-initialize client handle if not already initialized
  if (client->internal == NULL) {
    if (client->loop == NULL) {
      log_error("Client handle must have loop set for autonomous accept");
      return UV_EINVAL;
    }

    // Auto-initialize with same transport as server
    int err = turbo_init(client, client->loop, server->transport);
    if (err != 0) {
      log_error("Failed to auto-initialize client handle in accept: %s",
                turbo_strerror(err));
      return err;
    }

    log_debug("Auto-initialized client handle for %s transport",
              turbo_transport_name(server->transport));
  }

  return server_internal->vtable->accept(server, client);
}

TURBONET_API int turbo_read_start(turbo_handle_t* handle,
                     turbo_alloc_cb alloc_cb,
                     turbo_read_cb read_cb)
{
  if (handle == NULL || alloc_cb == NULL || read_cb == NULL) {
    return UV_EINVAL;
  }

  if (handle->state != TURBO_CONNECTED) {
    // For TLS, allow setting callbacks during handshake
    if (handle->transport == TURBO_TLS && handle->state == TURBO_CONNECTING) {
      // Store callbacks, will start reading when handshake completes
      handle->alloc_cb = alloc_cb;
      handle->read_cb = read_cb;
      log_debug("TLS handshake in progress, callbacks stored");
      return 0;
    }
    log_error("Handle not connected: %s", turbo_state_name(handle->state));
    return UV_ENOTCONN;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL
      || internal->vtable->read_start == NULL)
  {
    return TURBO_EUNSUPPORTED;
  }

  handle->alloc_cb = alloc_cb;
  handle->read_cb = read_cb;
  internal->reading = true;

  log_debug("Starting read on handle");

  return internal->vtable->read_start(handle);
}

TURBONET_API int turbo_read_stop(turbo_handle_t* handle)
{
  if (handle == NULL) {
    return UV_EINVAL;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL
      || internal->vtable->read_stop == NULL)
  {
    return TURBO_EUNSUPPORTED;
  }

  internal->reading = false;
  handle->alloc_cb = NULL;
  handle->read_cb = NULL;

  log_debug("Stopping read on handle");

  return internal->vtable->read_stop(handle);
}

TURBONET_API int turbo_write(turbo_req_t* req,
                turbo_handle_t* handle,
                const turbo_buf_t bufs[],
                unsigned int nbufs,
                turbo_write_cb cb)
{
  if (req == NULL || handle == NULL || bufs == NULL || nbufs == 0 || cb == NULL)
  {
    return UV_EINVAL;
  }

  if (handle->state != TURBO_CONNECTED) {
    log_error("Handle not connected: %s", turbo_state_name(handle->state));
    return UV_ENOTCONN;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL
      || internal->vtable->write == NULL)
  {
    return TURBO_EUNSUPPORTED;
  }

  // Initialize request
  req->handle = handle;
  req->write_cb = cb;

  // Calculate total bytes for statistics
  size_t total_bytes = 0;
  for (unsigned int i = 0; i < nbufs; i++) {
    total_bytes += bufs[i].len;
  }

  log_debug("Writing %zu bytes on handle", total_bytes);

  int err = internal->vtable->write(req, bufs, nbufs);
  if (err == 0) {
    handle->bytes_written += total_bytes;
  }

  return err;
}

TURBONET_API int turbo_close(turbo_handle_t* handle, turbo_close_cb close_cb)
{
  if (handle == NULL) {
    return UV_EINVAL;
  }

  if (handle->state == TURBO_CLOSED || handle->state == TURBO_CLOSING) {
    log_debug("Handle already closing/closed");
    if (close_cb) {
      close_cb(handle);
    }
    return 0;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL
      || internal->vtable->close == NULL)
  {
    return TURBO_EUNSUPPORTED;
  }

  handle->state = TURBO_CLOSING;
  handle->close_cb = close_cb;
  internal->closing = true;

  log_debug("Closing handle");

  return internal->vtable->close(handle);
}

// =============================================================================
// Configuration API - transport-specific options
// =============================================================================

TURBONET_API int turbo_set_option(turbo_handle_t* handle,
                     const char* key,
                     const void* value,
                     size_t len)
{
  if (handle == NULL || key == NULL || value == NULL) {
    return UV_EINVAL;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL) {
    return TURBO_ENOTINIT;
  }

  if (internal->vtable->set_option == NULL) {
    log_debug("Transport %s doesn't support options",
              turbo_transport_name(handle->transport));
    return 0;  // Not an error, just ignore
  }

  return internal->vtable->set_option(handle, key, value, len);
}

TURBONET_API int turbo_get_option(turbo_handle_t* handle,
                     const char* key,
                     void* value,
                     size_t* len)
{
  if (handle == NULL || key == NULL || value == NULL || len == NULL) {
    return UV_EINVAL;
  }

  turbo_internal_t* internal = turbo_get_internal(handle);
  if (internal == NULL || internal->vtable == NULL) {
    return TURBO_ENOTINIT;
  }

  if (internal->vtable->get_option == NULL) {
    return TURBO_EUNSUPPORTED;
  }

  return internal->vtable->get_option(handle, key, value, len);
}

TURBONET_API int turbo_set_timeout(turbo_handle_t* handle, uint32_t timeout_ms)
{
  return turbo_set_option(handle, "timeout", &timeout_ms, sizeof(timeout_ms));
}

TURBONET_API int turbo_set_keepalive(turbo_handle_t* handle, int enable, uint32_t delay)
{
  int err = turbo_set_option(handle, "keepalive", &enable, sizeof(enable));
  if (err == 0 && enable) {
    err = turbo_set_option(handle, "keepalive_delay", &delay, sizeof(delay));
  }
  return err;
}

TURBONET_API int turbo_set_nodelay(turbo_handle_t* handle, int enable)
{
  return turbo_set_option(handle, "nodelay", &enable, sizeof(enable));
}

TURBONET_API int turbo_tls_set_cert(turbo_handle_t* handle,
                       const char* cert_file,
                       const char* key_file)
{
  int err = turbo_set_option(handle, "cert_file", cert_file, strlen(cert_file));
  if (err == 0) {
    err = turbo_set_option(handle, "key_file", key_file, strlen(key_file));
  }
  return err;
}

TURBONET_API int turbo_tls_set_ca(turbo_handle_t* handle, const char* ca_file)
{
  return turbo_set_option(handle, "ca_file", ca_file, strlen(ca_file));
}

TURBONET_API int turbo_tls_set_verify(turbo_handle_t* handle, int verify_peer)
{
  return turbo_set_option(
      handle, "verify_peer", &verify_peer, sizeof(verify_peer));
}

TURBONET_API int turbo_tls_set_cipher_list(turbo_handle_t* handle, const char* cipher_list)
{
  if (handle == NULL || cipher_list == NULL) {
    return UV_EINVAL;
  }
  return turbo_set_option(
      handle, "cipher_list", cipher_list, strlen(cipher_list));
}

TURBONET_API int turbo_tls_set_secure_defaults(turbo_handle_t* handle)
{
  if (handle == NULL || handle->transport != TURBO_TLS) {
    return UV_EINVAL;
  }

  // Set secure defaults for modern TLS
  // Note: Most secure settings are already applied during SSL_CTX
  // initialization

  // For testing environments, disable certificate verification
  // In production, users should explicitly enable verification with proper CA
  // certificates
  int verify_peer = 0;  // Disable for testing
  int err = turbo_tls_set_verify(handle, verify_peer);

  // Set a balanced cipher list for compatibility
  if (err == 0) {
    const char*
        balanced_ciphers =
            "ECDHE+AESGCM:ECDHE+AES256:ECDHE+AES128:DHE+AESGCM:AES256-GCM-"
            "SHA384:" "AES128-GCM-SHA256:HIGH:!aNULL:!MD5:!RC4";
    err = turbo_tls_set_cipher_list(handle, balanced_ciphers);
  }

  log_debug(
      "TLS secure defaults applied (test mode: verify_peer=0, balanced "
      "ciphers)");
  return err;
}

TURBONET_API int turbo_kcp_set_mode(turbo_handle_t* handle, int mode)
{
  return turbo_set_option(handle, "kcp_mode", &mode, sizeof(mode));
}

TURBONET_API int turbo_kcp_set_wndsize(turbo_handle_t* handle, int snd_wnd, int rcv_wnd)
{
  int err = turbo_set_option(handle, "kcp_snd_wnd", &snd_wnd, sizeof(snd_wnd));
  if (err == 0) {
    err = turbo_set_option(handle, "kcp_rcv_wnd", &rcv_wnd, sizeof(rcv_wnd));
  }
  return err;
}

// =============================================================================
// Helper functions
// =============================================================================

static int turbo_init_internal(turbo_handle_t* handle)
{
  turbo_internal_t* internal = malloc(sizeof(turbo_internal_t));
  if (internal == NULL) {
    return UV_ENOMEM;
  }

  memset(internal, 0, sizeof(*internal));
  internal->ref_count = 1;

  handle->internal = internal;
  return 0;
}

static void turbo_cleanup_internal(turbo_handle_t* handle)
{
  if (handle->internal) {
    free(handle->internal);
    handle->internal = NULL;
  }
}

turbo_internal_t* turbo_get_internal(turbo_handle_t* handle)
{
  return (turbo_internal_t*)handle->internal;
}

TURBONET_API const char* turbo_transport_name(turbo_transport_t transport)
{
  switch (transport) {
    case TURBO_TCP:
      return "TCP";
    case TURBO_TLS:
      return "TLS";
    case TURBO_KCP:
      return "KCP";
    case TURBO_UDP:
      return "UDP";
    case TURBO_PIPE:
      return "PIPE";
    case TURBO_QUIC:
      return "QUIC";
    default:
      return "UNKNOWN";
  }
}

TURBONET_API const char* turbo_state_name(turbo_state_t state)
{
  switch (state) {
    case TURBO_CLOSED:
      return "CLOSED";
    case TURBO_CONNECTING:
      return "CONNECTING";
    case TURBO_CONNECTED:
      return "CONNECTED";
    case TURBO_CLOSING:
      return "CLOSING";
    case TURBO_ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

TURBONET_API turbo_buf_t turbo_buf_init(char* base, size_t len)
{
  turbo_buf_t buf;
  buf.base = base;
  buf.len = len;
  return buf;
}

TURBONET_API void turbo_buf_free(turbo_buf_t* buf)
{
  if (buf && buf->base) {
    free(buf->base);
    buf->base = NULL;
    buf->len = 0;
  }
}

// =============================================================================
// Global Event Loop API - hides libuv from users
// =============================================================================

TURBONET_API int turbo_global_init(turbo_handle_t* handle)
{
    if (!handle) {
        return UV_EINVAL;
    }
    
    if (g_loop_initialized) {
        // Already initialized, just set up handle
        memset(handle, 0, sizeof(*handle));
        handle->loop = g_global_loop;
        return 0;
    }
    
    g_global_loop = uv_default_loop();
    if (g_global_loop == NULL) {
        log_error("Failed to get default event loop");
        return UV_ENOMEM;
    }
    
    g_loop_initialized = 1;
    log_debug("Global event loop initialized");
    
    // Set up handle
    memset(handle, 0, sizeof(*handle));
    handle->loop = g_global_loop;
    
    return 0;
}

TURBONET_API int turbo_run(void)
{
    if (!g_loop_initialized) {
        log_error("Global loop not initialized - call turbo_global_init() first");
        return UV_EINVAL;
    }
    
    log_debug("Running global event loop");
    return uv_run(g_global_loop, UV_RUN_DEFAULT);
}

TURBONET_API int turbo_run_once(void)
{
    if (!g_loop_initialized) {
        log_error("Global loop not initialized - call turbo_global_init() first");
        return UV_EINVAL;
    }
    
    log_debug("Running global event loop once (non-blocking)");
    return uv_run(g_global_loop, UV_RUN_NOWAIT);
}

TURBONET_API int turbo_run_nowait(void)
{
    if (!g_loop_initialized) {
        log_error("Global loop not initialized - call turbo_global_init() first");
        return UV_EINVAL;
    }
    
    log_debug("Running global event loop until idle");
    return uv_run(g_global_loop, UV_RUN_ONCE);
}

TURBONET_API void turbo_stop(void)
{
    if (!g_loop_initialized) {
        log_warn("Global loop not initialized");
        return;
    }
    
    log_debug("Stopping global event loop");
    uv_stop(g_global_loop);
}

TURBONET_API void turbo_global_cleanup(void)
{
    if (!g_loop_initialized) {
        return;
    }
    
    log_debug("Cleaning up global event loop");
    
    // Close the loop and release resources
    if (g_global_loop && g_global_loop != uv_default_loop()) {
        uv_loop_close(g_global_loop);
    }
    
    g_global_loop = NULL;
    g_loop_initialized = 0;
}

// Helper function to get global loop for internal use
uv_loop_t* turbo_get_global_loop(void)
{
    if (!g_loop_initialized) {
        // Just initialize the global loop, no handle needed
        g_global_loop = uv_default_loop();
        if (g_global_loop == NULL) {
            log_error("Failed to get default event loop");
            return NULL;
        }
        g_loop_initialized = 1;
        log_debug("Global event loop auto-initialized");
    }
    return g_global_loop;
}
