#ifndef TLS_TEST_SUPPORT_H
#define TLS_TEST_SUPPORT_H

#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#ifdef _WIN32
typedef SOCKET test_socket_t;
#  define TEST_INVALID_SOCKET INVALID_SOCKET
#  define test_close_socket closesocket
#else
typedef int test_socket_t;
#  define TEST_INVALID_SOCKET (-1)
#  define test_close_socket close
#endif

static const char s_test_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC7TCCAdWgAwIBAgIUT4pOT+qAkLpsC1bUF3bYRrTHssQwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDMyMzA4MDcwMloXDTM2MDMy\n"
    "MDA4MDcwMlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAQ8AMIIBCgKCAQEAtNuutQlZVrXBW97HX5HfMXbMkES9n2eglXQRzU7Qg4Mm\n"
    "KtAprpkBVSFHeAti0NyPgasoaoJTBi1xBhDsGTWTto0TJVHhW5QcYSPRc8x/acWQ\n"
    "NxBSMdWf8Rp9QxbaECyQbWr+QDb/c1a9QU0fGFntQBnLfk9lLJG7MRTwg38ufnSk\n"
    "OqqyAtbT4V5ZwImkOo9MdECcZMvRDYnvH1atIUvGRI7O3M466jGe+5WN4E42h8VN\n"
    "PSJw2IBvbFxePZ3yMWpiVRkbsWlq1hJIHGvnGD+4IPGr2nB/FmR+P969KFm/gSvG\n"
    "L9tYFRw36Cfa+cnwWAYNpLspwOaaAQcpeMN8tAGIPwIDAQABozcwNTAUBgNVHREE\n"
    "DTALgglsb2NhbGhvc3QwHQYDVR0OBBYEFHSKGrYW6d59EU5htbnpgVhPLaQiMA0G\n"
    "CSqGSIb3DQEBCwUAA4IBAQBhIzu8IJ7Pm30nKOfvwgQRKbJDWIBKZz/NYoIP5Ljm\n"
    "fZG+ZZT0BnuCObKTvPwAWERwbIn5cIDNCkVKhQoJc4+KqR9fXptxML+Q3e4lCVo3\n"
    "5jjQpG/r18aZxhfroinp6iCfGcECw/JAXPxC8jOhEgVOPQd/LybM9vO8vraH/dIR\n"
    "YRmIoBvGw+wQMt/PcV0GxYLo6LsYJFs0FuJyiufJ2auNtmW5h8qOdtnagmeo0ehp\n"
    "g5VqPlB3EMa/01r9WmfNQJcBbEF8ONhhPXZCV4uplsXGtN8+Xxrzb3SAYQR9xFry\n"
    "x9YTzT8UMLc26vY1RiF6uwODUJzmSaqmefmapVsWrgi3\n"
    "-----END CERTIFICATE-----\n";

static const char s_test_key_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC02661CVlWtcFb\n"
    "3sdfkd8xdsyQRL2fZ6CVdBHNTtCDgyYq0CmumQFVIUd4C2LQ3I+BqyhqglMGLXEG\n"
    "EOwZNZO2jRMlUeFblBxhI9FzzH9pxZA3EFIx1Z/xGn1DFtoQLJBtav5ANv9zVr1B\n"
    "TR8YWe1AGct+T2UskbsxFPCDfy5+dKQ6qrIC1tPhXlnAiaQ6j0x0QJxky9ENie8f\n"
    "Vq0hS8ZEjs7czjrqMZ77lY3gTjaHxU09InDYgG9sXF49nfIxamJVGRuxaWrWEkgc\n"
    "a+cYP7gg8avacH8WZH4/3r0oWb+BK8Yv21gVHDfoJ9r5yfBYBg2kuynA5poBByl4\n"
    "w3y0AYg/AgMBAAECggEAEJkoy4yexQp2mHaLAwZhiX9G/uaQJepeHoPsg6nRZoB0\n"
    "JvG7zD5WlPgyQEjV5NKZM7lVmDt7Cydt0V9e4QwTERSZcToL3gUV0FnNMJIlZLuw\n"
    "+fIRg76rUyFZ5aevPlTDXIdj64N1+6E2SqFH/UrOL1fZXoTthXhKdGgLkBtCqnA6\n"
    "DlHQX3lehrnV+MG5fTxPc8lro/s4UVAoBMhc4dP5U1W5Xt5c6RsdcWYytidRYj8t\n"
    "XMkyjST/F2NV80+8WGp/YFE0dHyxGWvLGNmkOUuI4EMwzzSadsIM+PQO/YP1KwHA\n"
    "0DYHuEFvPCLjPsD+7IUnZgifQe45/FJoJMp5hSmzgQKBgQD7XEl2mLR3iqup2dF+\n"
    "PD3zA2J48jdiJdbK7vRLXpdV5WP2/s90GZFKLadg7UWmx9zWkC4B92atNJV0/+8o\n"
    "wE4Zd8PG62QZ3o1T4QpYMem9PAq5OxqwYBxMZ2Y5Mf+54Gp0SXB+AbXPlYI/LIwP\n"
    "i/2Iq+bAjGmuGuloJNWD3Wl3DwKBgQC4MkMYvf5aSqbL8GE5ndKY06HzbxwcMoh3\n"
    "Hia5LRMw5dG3J2JwdruiE4V3gQyqz0NzYrrqqkyYxh3aJW934qj6JVMVw/xWx2n5\n"
    "xB4X4hcCKrO2piROmOuXBEt1T36C+fShNb8g+RNY0edoiw+OKTa3rzlQhggTkoGs\n"
    "Iy7oyxtb0QKBgGKkgfP304LCOcHrSCppC8qtflyGebObs+Jpyhc15OABqKxKrTEb\n"
    "w4e/yNrh4p6j+od9h4CgDXxVkX2b3sg4R6348SzEPcFlNENBomSgGeF4iaDNkBi9\n"
    "bv2Q6m3xsDDK4BwIogvhMe9n9fhCzChhwLp8846GzAZWa1jCc8RPBM+DAoGAQxRy\n"
    "4QDYL5O+OMka7zutpWB1O008hHxWvGKroYZr1cPsYvIh5GkpHfZUBdhmf5Ips0zC\n"
    "W5GXgY+s8XPuq09NUIPlRSjxrbzDuGUWvIXm8TAR8LOCx2jja0TyIg/IN/TFhSwo\n"
    "pd5vkEopJyZ1jMUvmydiDRQyvsX9GW5auAa3uPECgYBxuBJ6Vji7pxlqjG3aB0je\n"
    "+JexLyzdckU7EKTxpTSU1o/p17QpT26KF+DPMc2kg+PBK+Sjm0m4Uxdzq/OXNMMA\n"
    "zhR6Vjo1nPWsKgzK03hGzaJVMkHekgCidY9R+MZEeDAhHDIia9XyAS1qCoGAJ6WC\n"
    "oYB4EuDLFhurWiLO+diuMg==\n"
    "-----END PRIVATE KEY-----\n";

/**
 * @brief Initialize the socket runtime on platforms that require it.
 */
static int tls_test_init_socket_runtime(void) {
#ifdef _WIN32
  static int ready = 0;
  if (ready) return 0;

  {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return -1;
    }
  }

  ready = 1;
#endif
  return 0;
}

/**
 * @brief Wait briefly until the socket is readable.
 */
static int tls_test_wait_readable(test_socket_t sock) {
  fd_set readfds;
  struct timeval tv;

  FD_ZERO(&readfds);
  FD_SET(sock, &readfds);
  tv.tv_sec = 5;
  tv.tv_usec = 0;

  return select((int)(sock + 1), &readfds, NULL, NULL, &tv) > 0;
}

/**
 * @brief Create a loopback listener bound to an ephemeral port.
 */
static int tls_test_prepare_listener(test_socket_t *listen_socket, unsigned short *port) {
  struct sockaddr_in addr;
  socklen_t addr_len = (socklen_t)sizeof(addr);
  int yes = 1;

  if (!listen_socket || !port) return -1;
  if (tls_test_init_socket_runtime() != 0) return -1;

  *listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*listen_socket == TEST_INVALID_SOCKET) return -1;

  setsockopt(*listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);

  if (bind(*listen_socket, (const struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
  if (listen(*listen_socket, 1) != 0) return -1;
  if (getsockname(*listen_socket, (struct sockaddr *)&addr, &addr_len) != 0) return -1;

  *port = ntohs(addr.sin_port);
  return 0;
}

/**
 * @brief Persist the embedded test certificate so the client can trust it.
 */
static int tls_test_write_ca_file(char *path, size_t path_len) {
  FILE *fp;

  if (!path || path_len == 0) return -1;

#ifdef _WIN32
  {
    char temp_dir[MAX_PATH];
    DWORD dir_len = GetTempPathA((DWORD)sizeof(temp_dir), temp_dir);
    if (dir_len == 0 || dir_len >= sizeof(temp_dir)) return -1;
    if (GetTempFileNameA(temp_dir, "tn", 0, path) == 0) return -1;
  }
#else
  {
    char tmpl[] = "/tmp/turbonet_tls_ca_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    test_close_socket(fd);
    strncpy(path, tmpl, path_len - 1);
    path[path_len - 1] = '\0';
  }
#endif

  fp = fopen(path, "wb");
  if (!fp) return -1;
  if (fwrite(s_test_cert_pem, 1, strlen(s_test_cert_pem), fp) != strlen(s_test_cert_pem)) {
    fclose(fp);
    return -1;
  }

  fclose(fp);
  return 0;
}

/**
 * @brief Remove the temporary CA bundle file.
 */
static void tls_test_remove_file(const char *path) {
  if (!path || path[0] == '\0') return;

#ifdef _WIN32
  DeleteFileA(path);
#else
  unlink(path);
#endif
}

/**
 * @brief Point TLS verification at the temporary CA bundle.
 */
static int tls_test_set_ca_file_env(const char *path) {
  if (!path) return -1;

#ifdef _WIN32
  if (_putenv_s("TURBONET_TLS_CA_FILE", path) != 0) return -1;
  if (_putenv_s("TURBONET_TLS_CA_PATH", "") != 0) return -1;
#else
  if (setenv("TURBONET_TLS_CA_FILE", path, 1) != 0) return -1;
  if (unsetenv("TURBONET_TLS_CA_PATH") != 0) return -1;
#endif

  return 0;
}

/**
 * @brief Clear test-specific CA environment variables.
 */
static void tls_test_clear_ca_env(void) {
#ifdef _WIN32
  _putenv_s("TURBONET_TLS_CA_FILE", "");
  _putenv_s("TURBONET_TLS_CA_PATH", "");
#else
  unsetenv("TURBONET_TLS_CA_FILE");
  unsetenv("TURBONET_TLS_CA_PATH");
#endif
}

/**
 * @brief Build a server-side TLS context from the embedded PEM blobs.
 */
static SSL_CTX *tls_test_create_server_ctx(void) {
  SSL_CTX *ctx = NULL;
  BIO *cert_bio = NULL;
  BIO *key_bio = NULL;
  X509 *cert = NULL;
  EVP_PKEY *key = NULL;

  ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) goto fail;

  cert_bio = BIO_new_mem_buf(s_test_cert_pem, -1);
  key_bio = BIO_new_mem_buf(s_test_key_pem, -1);
  if (!cert_bio || !key_bio) goto fail;

  cert = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
  key = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
  if (!cert || !key) goto fail;

  if (SSL_CTX_use_certificate(ctx, cert) != 1) goto fail;
  if (SSL_CTX_use_PrivateKey(ctx, key) != 1) goto fail;
  if (SSL_CTX_check_private_key(ctx) != 1) goto fail;

  X509_free(cert);
  EVP_PKEY_free(key);
  BIO_free(cert_bio);
  BIO_free(key_bio);
  return ctx;

fail:
  X509_free(cert);
  EVP_PKEY_free(key);
  BIO_free(cert_bio);
  BIO_free(key_bio);
  SSL_CTX_free(ctx);
  return NULL;
}

#endif
