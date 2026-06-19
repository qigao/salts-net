/**
 * Platform abstraction implementation - minimal but sufficient
 * "Write portable code, but test on real systems" - Linus
 */
#include "platform.h"
#include "memory_pool.h"
#include "sds.h"
#include "tlog.h"
#include "turbo_thread.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #include <iphlpapi.h>
  #include <winternl.h>
#else
  #include <ifaddrs.h>
  #include <pwd.h>
  #include <signal.h>
  #include <net/if.h>
  #include <sys/utsname.h>
  #include <time.h>
  #include <unistd.h>
#endif

static int turbo_platform_copy_string(char *buffer, size_t buffer_size, const char *value) {
  size_t len;
  if (!buffer || buffer_size == 0 || !value) {
    return -EINVAL;
  }
  len = strlen(value);
  if (len + 1 > buffer_size) {
    return -ENOSPC;
  }
  memcpy(buffer, value, len + 1);
  return 0;
}

static void turbo_platform_ascii_lowercase(char *value) {
  if (!value) {
    return;
  }
  while (*value) {
    if (*value >= 'A' && *value <= 'Z') {
      *value = (char)(*value - 'A' + 'a');
    }
    ++value;
  }
}

static void turbo_platform_zero_cpu_info(turbo_platform_cpu_info_t *info) {
  if (!info) {
    return;
  }
  memset(info, 0, sizeof(*info));
  turbo_platform_copy_string(info->model, sizeof(info->model), "unknown");
}

static void turbo_platform_zero_interface(turbo_platform_network_interface_t *iface) {
  if (!iface) {
    return;
  }
  memset(iface, 0, sizeof(*iface));
}

static int turbo_platform_is_loopback_address(const char *address) {
  if (!address || address[0] == '\0') {
    return 0;
  }
  return strcmp(address, "127.0.0.1") == 0 || strcmp(address, "::1") == 0;
}

#ifdef _WIN32
static int turbo_platform_get_windows_version(char *buffer, size_t buffer_size) {
  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  typedef LONG (WINAPI *rtl_get_version_fn)(PRTL_OSVERSIONINFOW);
  rtl_get_version_fn rtl_get_version;
  RTL_OSVERSIONINFOW version_info;
  int written;

  if (!ntdll) {
    return -ENOENT;
  }

  rtl_get_version = (rtl_get_version_fn)GetProcAddress(ntdll, "RtlGetVersion");
  if (!rtl_get_version) {
    return -ENOENT;
  }

  memset(&version_info, 0, sizeof(version_info));
  version_info.dwOSVersionInfoSize = sizeof(version_info);
  if (rtl_get_version(&version_info) != 0) {
    return -EIO;
  }

  written = snprintf(buffer, buffer_size, "%lu.%lu.%lu",
                     (unsigned long)version_info.dwMajorVersion,
                     (unsigned long)version_info.dwMinorVersion,
                     (unsigned long)version_info.dwBuildNumber);
  if (written < 0 || (size_t)written >= buffer_size) {
    return -ENOSPC;
  }
  return 0;
}
#endif

// =============================================================================
// Time utilities - high-resolution native platform timing
// =============================================================================

#ifdef _WIN32
uint64_t turbo_hrtime(void) {
  /* Benign race: worst case two threads both init freq to the same value */
  static volatile uint64_t freq = 0;
  uint64_t f = freq;
  if (f == 0) {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    f = li.QuadPart;
    freq = f;
  }
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  if (f == 0) return 0;
  uint64_t whole = (li.QuadPart / f) * 1000000000ULL;
  uint64_t part = (li.QuadPart % f) * 1000000000ULL / f;
  return whole + part;
}

uint64_t turbo_realtime_ms(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER uli;
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  // 100ns intervals since Jan 1, 1601.
  // Subtract EPOCH_DIFF (116444736000000000ns) to get Unix epoch in 100ns intervals.
  return (uli.QuadPart - 116444736000000000ULL) / 10000ULL;
}

int turbo_gettimeofday(turbo_timeval_t *tv, turbo_timezone_t *tz) {
  UNUSED(tz);
  if (tv) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    // 100ns intervals since Jan 1, 1601.
    uint64_t intervals = uli.QuadPart - 116444736000000000ULL;
    tv->tv_sec = (int64_t)(intervals / 10000000ULL);
    tv->tv_usec = (int32_t)((intervals % 10000000ULL) / 10ULL);
  }
  return 0;
}
#else
uint64_t turbo_hrtime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t turbo_realtime_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int turbo_gettimeofday(turbo_timeval_t *tv, turbo_timezone_t *tz) {
  UNUSED(tz);
  struct timeval system_tv;
  int result = gettimeofday(&system_tv, NULL);
  if (result == 0 && tv) {
    tv->tv_sec = (int64_t)system_tv.tv_sec;
    tv->tv_usec = (int32_t)system_tv.tv_usec;
  }
  return result;
}
#endif

uint64_t turbo_monotonic_ms(void) { return turbo_ns_to_ms(turbo_hrtime()); }

uint64_t turbo_uptime_ms(void) {
  /* Benign race: worst case start_time shifts by nanoseconds */
  static volatile uint64_t start_time = 0;
  uint64_t st = start_time;
  if (st == 0) {
    st = turbo_hrtime();
    start_time = st;
  }
  return turbo_ns_to_ms(turbo_hrtime() - st);
}

static int turbo_is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int turbo_days_in_month(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && turbo_is_leap_year(year)) return 29;
  return days[month - 1];
}

static int64_t turbo_days_before_year(int year) {
  int64_t y = (int64_t)year - 1;
  return y * 365 + y / 4 - y / 100 + y / 400;
}

static int64_t turbo_days_before_month(int year, int month) {
  static const int days_before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int64_t days;
  if (month < 1 || month > 12) return -1;
  days = days_before[month - 1];
  if (month > 2 && turbo_is_leap_year(year)) days++;
  return days;
}

int turbo_gmtime(time_t t, struct tm *out) {
  if (!out) return -EINVAL;
#ifdef _WIN32
  return gmtime_s(out, &t) == 0 ? 0 : -EINVAL;
#else
  return gmtime_r(&t, out) ? 0 : -EINVAL;
#endif
}

int turbo_localtime(time_t t, struct tm *out) {
  if (!out) return -EINVAL;
#ifdef _WIN32
  return localtime_s(out, &t) == 0 ? 0 : -EINVAL;
#else
  return localtime_r(&t, out) ? 0 : -EINVAL;
#endif
}

time_t turbo_timegm(const struct tm *tm_value) {
  int year;
  int month;
  int day;
  int64_t days;
  int64_t seconds;

  if (!tm_value) {
    errno = EINVAL;
    return (time_t)-1;
  }

  year = tm_value->tm_year + 1900;
  month = tm_value->tm_mon + 1;
  day = tm_value->tm_mday;
  if (month < 1 || month > 12 || day < 1 || day > turbo_days_in_month(year, month) ||
      tm_value->tm_hour < 0 || tm_value->tm_hour > 23 ||
      tm_value->tm_min < 0 || tm_value->tm_min > 59 ||
      tm_value->tm_sec < 0 || tm_value->tm_sec > 60) {
    errno = EINVAL;
    return (time_t)-1;
  }

  days = turbo_days_before_year(year) - turbo_days_before_year(1970);
  days += turbo_days_before_month(year, month);
  days += day - 1;
  seconds = days * 86400 + tm_value->tm_hour * 3600 + tm_value->tm_min * 60 + tm_value->tm_sec;
  return (time_t)seconds;
}

time_t turbo_mktime(struct tm *tm_value) {
  if (!tm_value) {
    errno = EINVAL;
    return (time_t)-1;
  }
  return mktime(tm_value);
}

int turbo_strftime_utc(time_t t, const char *format, char *buffer, size_t buffer_size) {
  struct tm tm_value;
  size_t written;

  if (!format || !buffer || buffer_size == 0) return -EINVAL;
  if (turbo_gmtime(t, &tm_value) != 0) return -EINVAL;
  written = strftime(buffer, buffer_size, format, &tm_value);
  if (written == 0) return -ENOSPC;
  return (int)written;
}

int turbo_strftime_local(time_t t, const char *format, char *buffer, size_t buffer_size) {
  struct tm tm_value;
  size_t written;

  if (!format || !buffer || buffer_size == 0) return -EINVAL;
  if (turbo_localtime(t, &tm_value) != 0) return -EINVAL;
  written = strftime(buffer, buffer_size, format, &tm_value);
  if (written == 0) return -ENOSPC;
  return (int)written;
}

int turbo_platform_os_name(char *buffer, size_t buffer_size) {
#ifdef _WIN32
  return turbo_platform_copy_string(buffer, buffer_size, "windows");
#else
  struct utsname info;
  if (uname(&info) != 0) {
    return -errno;
  }
  if (turbo_platform_copy_string(buffer, buffer_size, info.sysname) != 0) {
    return -ENOSPC;
  }
  if (strcmp(buffer, "Darwin") == 0) {
    return turbo_platform_copy_string(buffer, buffer_size, "macos");
  }
  turbo_platform_ascii_lowercase(buffer);
  return 0;
#endif
}

int turbo_platform_os_version(char *buffer, size_t buffer_size) {
#ifdef _WIN32
  return turbo_platform_get_windows_version(buffer, buffer_size);
#else
  struct utsname info;
  if (uname(&info) != 0) {
    return -errno;
  }
  return turbo_platform_copy_string(buffer, buffer_size, info.release);
#endif
}

int turbo_platform_arch(char *buffer, size_t buffer_size) {
#ifdef _WIN32
  SYSTEM_INFO system_info;
  const char *arch = "unknown";

  GetNativeSystemInfo(&system_info);
  switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      arch = "x86_64";
      break;
    case PROCESSOR_ARCHITECTURE_INTEL:
      arch = "x86";
      break;
    case PROCESSOR_ARCHITECTURE_ARM64:
      arch = "arm64";
      break;
    case PROCESSOR_ARCHITECTURE_ARM:
      arch = "arm";
      break;
    default:
      break;
  }

  return turbo_platform_copy_string(buffer, buffer_size, arch);
#else
  struct utsname info;
  if (uname(&info) != 0) {
    return -errno;
  }
  return turbo_platform_copy_string(buffer, buffer_size, info.machine);
#endif
}

int turbo_platform_username(char *buffer, size_t buffer_size) {
#ifdef _WIN32
  DWORD size = (DWORD)buffer_size;
  const DWORD env_len = GetEnvironmentVariableA("USERNAME", buffer, size);
  if (env_len > 0 && env_len < size) {
    return 0;
  }
  return -ENOENT;
#else
  const char *user = getenv("USER");
  if (user && user[0] != '\0') {
    return turbo_platform_copy_string(buffer, buffer_size, user);
  }
  {
    struct passwd *pwd = getpwuid(getuid());
    if (!pwd || !pwd->pw_name) {
      return -ENOENT;
    }
    return turbo_platform_copy_string(buffer, buffer_size, pwd->pw_name);
  }
#endif
}

int turbo_platform_hostname(char *buffer, size_t buffer_size) {
#ifdef _WIN32
  DWORD size = (DWORD)buffer_size;
  if (!GetComputerNameA(buffer, &size)) {
    return -ENOENT;
  }
  return 0;
#else
  if (gethostname(buffer, buffer_size) != 0) {
    return -errno;
  }
  buffer[buffer_size - 1] = '\0';
  return 0;
#endif
}

int turbo_platform_cpu_info(turbo_platform_cpu_info_t *info) {
  if (!info) {
    return -EINVAL;
  }

  turbo_platform_zero_cpu_info(info);

#ifdef _WIN32
  {
    SYSTEM_INFO system_info;
    const DWORD env_len =
        GetEnvironmentVariableA("PROCESSOR_IDENTIFIER", info->model, sizeof(info->model));
    GetNativeSystemInfo(&system_info);
    info->core_count = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (info->core_count <= 0) {
      info->core_count = (int)system_info.dwNumberOfProcessors;
    }
    if (env_len == 0 || env_len >= sizeof(info->model)) {
      turbo_platform_copy_string(info->model, sizeof(info->model), "unknown");
    }
    info->speed_mhz = 0.0;
    return 0;
  }
#else
  {
    long core_count = sysconf(_SC_NPROCESSORS_ONLN);
    struct utsname uts;
    if (core_count > 0) {
      info->core_count = (int)core_count;
    }
    if (uname(&uts) == 0) {
      turbo_platform_copy_string(info->model, sizeof(info->model), uts.machine);
    }
    info->speed_mhz = 0.0;
    return 0;
  }
#endif
}

int turbo_platform_memory_info(turbo_platform_memory_info_t *info) {
  if (!info) {
    return -EINVAL;
  }

  memset(info, 0, sizeof(*info));

#ifdef _WIN32
  {
    MEMORYSTATUSEX mem;
    memset(&mem, 0, sizeof(mem));
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) {
      return -EIO;
    }
    info->total_memory = mem.ullTotalPhys;
    info->free_memory = mem.ullAvailPhys;
    info->available_memory = mem.ullAvailPhys;
    return 0;
  }
#else
  {
    const long page_size = sysconf(_SC_PAGE_SIZE);
    const long total_pages = sysconf(_SC_PHYS_PAGES);
    const long avail_pages = sysconf(_SC_AVPHYS_PAGES);
    if (page_size <= 0 || total_pages <= 0 || avail_pages < 0) {
      return -EIO;
    }
    info->total_memory = (uint64_t)page_size * (uint64_t)total_pages;
    info->free_memory = (uint64_t)page_size * (uint64_t)avail_pages;
    info->available_memory = info->free_memory;
    return 0;
  }
#endif
}

int turbo_platform_load_average(turbo_platform_load_average_t *info) {
  if (!info) {
    return -EINVAL;
  }

  memset(info, 0, sizeof(*info));

#ifdef _WIN32
  return 0;
#else
  {
    double load[3] = {0.0, 0.0, 0.0};
    if (getloadavg(load, 3) < 0) {
      return -errno;
    }
    info->one_minute = load[0];
    info->five_minutes = load[1];
    info->fifteen_minutes = load[2];
    return 0;
  }
#endif
}

#ifdef _WIN32
static void turbo_extract_windows_unicast(IP_ADAPTER_ADDRESSES *adapter, 
                                          IP_ADAPTER_UNICAST_ADDRESS *unicast,
                                          turbo_platform_network_interface_t *iface) {
  DWORD name_len;
  DWORD addr_len = (DWORD)sizeof(iface->address);

  turbo_platform_zero_interface(iface);
  name_len = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, iface->name,
                                 (int)sizeof(iface->name), NULL, NULL);
  if (name_len == 0) {
    turbo_platform_copy_string(iface->name, sizeof(iface->name), "unknown");
  }

  if (WSAAddressToStringA(unicast->Address.lpSockaddr, (DWORD)unicast->Address.iSockaddrLength,
                          NULL, iface->address, &addr_len) != 0) {
    turbo_platform_copy_string(iface->address, sizeof(iface->address), "unknown");
  }

  if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
    ULONG prefix = unicast->OnLinkPrefixLength;
    ULONG mask = prefix == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - prefix));
    struct in_addr mask_addr;
    mask_addr.S_un.S_addr = mask;
    InetNtopA(AF_INET, &mask_addr, iface->netmask, (DWORD)sizeof(iface->netmask));
  } else if (unicast->Address.lpSockaddr->sa_family == AF_INET6) {
    turbo_platform_copy_string(iface->netmask, sizeof(iface->netmask), "::");
  }

  iface->is_internal =
      (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) ||
      turbo_platform_is_loopback_address(iface->address);
}
#else
static void *turbo_get_sockaddr_ptr(struct sockaddr *sa) {
  if (!sa) return NULL;
  if (sa->sa_family == AF_INET) {
    return (void *)&((struct sockaddr_in *)sa)->sin_addr;
  } else if (sa->sa_family == AF_INET6) {
    return (void *)&((struct sockaddr_in6 *)sa)->sin6_addr;
  }
  return NULL;
}
#endif

int turbo_platform_network_interfaces(turbo_platform_network_interface_t *interfaces,
                                      size_t max_interfaces, size_t *count) {
  if (!interfaces || max_interfaces == 0 || !count) {
    return -EINVAL;
  }

  *count = 0;

#ifdef _WIN32
  {
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    ULONG family = AF_UNSPEC;
    ULONG buffer_size = 16384;
    IP_ADAPTER_ADDRESSES *addresses = NULL;
    IP_ADAPTER_ADDRESSES *adapter;
    DWORD rc;

    addresses = (IP_ADAPTER_ADDRESSES *)malloc(buffer_size);
    if (!addresses) {
      return -ENOMEM;
    }

    rc = GetAdaptersAddresses(family, flags, NULL, addresses, &buffer_size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
      IP_ADAPTER_ADDRESSES *bigger = (IP_ADAPTER_ADDRESSES *)realloc(addresses, buffer_size);
      if (!bigger) {
        free(addresses);
        return -ENOMEM;
      }
      addresses = bigger;
      rc = GetAdaptersAddresses(family, flags, NULL, addresses, &buffer_size);
    }

    if (rc != NO_ERROR) {
      free(addresses);
      return -EIO;
    }

    for (adapter = addresses; adapter && *count < max_interfaces; adapter = adapter->Next) {
      IP_ADAPTER_UNICAST_ADDRESS *unicast;
      for (unicast = adapter->FirstUnicastAddress; unicast && *count < max_interfaces;
           unicast = unicast->Next) {
        turbo_extract_windows_unicast(adapter, unicast, &interfaces[*count]);
        ++(*count);
      }
    }

    free(addresses);
    return *count > 0 ? 0 : -ENOENT;
  }
#else
  {
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;

    if (getifaddrs(&ifaddr) != 0) {
      return -errno;
    }

    for (ifa = ifaddr; ifa && *count < max_interfaces; ifa = ifa->ifa_next) {
      turbo_platform_network_interface_t *iface;
      void *addr_ptr;
      void *mask_ptr;
      int family;

      if (!ifa->ifa_addr || !ifa->ifa_name) {
        continue;
      }

      family = ifa->ifa_addr->sa_family;
      if (family != AF_INET && family != AF_INET6) {
        continue;
      }

      iface = &interfaces[*count];
      turbo_platform_zero_interface(iface);
      turbo_platform_copy_string(iface->name, sizeof(iface->name), ifa->ifa_name);
      iface->is_internal = (ifa->ifa_flags & IFF_LOOPBACK) ? 1 : 0;

      addr_ptr = turbo_get_sockaddr_ptr(ifa->ifa_addr);
      mask_ptr = turbo_get_sockaddr_ptr(ifa->ifa_netmask);

      if (!inet_ntop(family, addr_ptr, iface->address, sizeof(iface->address))) {
        turbo_platform_copy_string(iface->address, sizeof(iface->address), "unknown");
      }
      if (mask_ptr && !inet_ntop(family, mask_ptr, iface->netmask, sizeof(iface->netmask))) {
        turbo_platform_copy_string(iface->netmask, sizeof(iface->netmask), "unknown");
      }
      if (turbo_platform_is_loopback_address(iface->address)) {
        iface->is_internal = 1;
      }
      ++(*count);
    }

    freeifaddrs(ifaddr);
    return *count > 0 ? 0 : -ENOENT;
  }
#endif
}

// =============================================================================
// Native OS Timer - uses system timer facilities (most efficient)
// =============================================================================

#ifdef _WIN32
// Windows implementation using CreateTimerQueueTimer

struct turbo_native_timer_s {
  HANDLE timer_handle;
  turbo_timer_cb callback;
  void *data;
  uint64_t timeout;
  uint64_t repeat;
  int active;
  volatile DWORD callback_thread_id;
  int pending_free;
};

// Windows timer callback wrapper
static VOID CALLBACK native_timer_callback_win32(PVOID lpParameter, BOOLEAN TimerOrWaitFired) {
  UNUSED(TimerOrWaitFired);
  turbo_timer_t *timer = (turbo_timer_t *)lpParameter;
  if (!timer) {
    return;
  }

  timer->callback_thread_id = GetCurrentThreadId();
  if (timer->callback) {
    timer->callback(timer);
  }

  if (timer->pending_free) {
    free(timer);
  } else {
    timer->callback_thread_id = 0;
  }
}

turbo_timer_t *turbo_timer_create(void *loop) {
  UNUSED(loop);
  turbo_timer_t *timer = malloc(sizeof(turbo_timer_t));
  if (!timer) {
    return NULL;
  }

  memset(timer, 0, sizeof(*timer));
  return timer;
}

void turbo_timer_destroy(turbo_timer_t *timer) {
  if (!timer) {
    return;
  }

  turbo_timer_stop(timer);

  if (GetCurrentThreadId() == timer->callback_thread_id) {
    timer->pending_free = 1;
  } else {
    free(timer);
  }
}

int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb, uint64_t timeout, uint64_t repeat) {
  if (!timer || !cb) {
    return -1;
  }

  // Stop existing timer if running
  turbo_timer_stop(timer);

  timer->callback = cb;
  timer->timeout = timeout;
  timer->repeat = repeat;

  // CreateTimerQueueTimer parameters:
  // - NULL = use default timer queue
  // - DueTime in ms
  // - Period in ms (0 for one-shot)
  // - WT_EXECUTEDEFAULT = execute in timer thread pool
  BOOL result = CreateTimerQueueTimer(&timer->timer_handle,
                                      NULL, // Use default timer queue
                                      native_timer_callback_win32, timer, (DWORD)timeout,
                                      (DWORD)repeat, WT_EXECUTEDEFAULT);

  if (!result) {
    TLOG_ERROR("CreateTimerQueueTimer failed: {}", GetLastError());
    return -1;
  }

  timer->active = 1;
  return 0;
}

int turbo_timer_stop(turbo_timer_t *timer) {
  if (!timer || !timer->active) {
    return 0;
  }

  timer->active = 0;

  // Atomically swap the handle with NULL to ensure we only call DeleteTimerQueueTimer once
  HANDLE h = InterlockedExchangePointer(&timer->timer_handle, NULL);
  if (h) {
    // Cannot wait for completion (INVALID_HANDLE_VALUE) if called from within the callback
    // as it would cause a deadlock or crash (double-deletion in some cases).
    HANDLE completion = INVALID_HANDLE_VALUE;
    if (GetCurrentThreadId() == timer->callback_thread_id) {
      completion = NULL;
    }

    if (!DeleteTimerQueueTimer(NULL, h, completion)) {
      DWORD err = GetLastError();
      if (err != ERROR_IO_PENDING) {
        TLOG_ERROR("DeleteTimerQueueTimer failed: {}", err);
      }
    }
  }
  return 0;
}

#else
// POSIX implementation using a process-local timer manager thread.
//
// Rationale:
// timer_delete(2) specifies that treatment of any pending notification is
// unspecified. The old SIGEV_THREAD backend could therefore free the timer
// object while glibc still had a callback thread pending, which produced
// intermittent use-after-free reports during socket shutdown under ASan/LSan.
//
// The manager below owns all armed POSIX timers, tracks due times with
// CLOCK_MONOTONIC, and spawns a detached worker thread for each firing. This
// preserves the "callback runs off-thread" contract without relying on the
// unspecified lifetime semantics of timer_delete().

  #include <time.h>

struct turbo_native_timer_s {
  turbo_timer_cb callback;
  void *data;
  uint64_t timeout;
  uint64_t repeat;
  uint64_t due_ms;
  int active;
  int destroying;
  int queued;
  int callbacks_inflight;
  turbo_mutex_t lock;
  turbo_cond_t cond;
  struct turbo_native_timer_s *next;
};

typedef struct {
  turbo_mutex_t lock;
  turbo_cond_t cond;
  turbo_timer_t *head;
  int initialized;
  int init_failed;
} turbo_posix_timer_manager_t;

typedef struct {
  turbo_timer_t *timer;
  turbo_timer_cb callback;
} turbo_posix_timer_task_t;

static turbo_posix_timer_manager_t g_turbo_posix_timer_manager;
static turbo_once_t g_turbo_posix_timer_manager_once = TURBO_ONCE_INIT;

static void turbo_posix_timer_queue_remove_locked(turbo_timer_t *timer) {
  turbo_timer_t **cursor;

  if (timer == NULL || !timer->queued) {
    return;
  }

  cursor = &g_turbo_posix_timer_manager.head;
  while (*cursor != NULL) {
    if (*cursor == timer) {
      *cursor = timer->next;
      timer->next = NULL;
      timer->queued = 0;
      return;
    }
    cursor = &(*cursor)->next;
  }
}

static void turbo_posix_timer_queue_insert_locked(turbo_timer_t *timer) {
  turbo_timer_t **cursor;

  if (timer == NULL) {
    return;
  }

  turbo_posix_timer_queue_remove_locked(timer);
  cursor = &g_turbo_posix_timer_manager.head;
  while (*cursor != NULL && (*cursor)->due_ms <= timer->due_ms) {
    cursor = &(*cursor)->next;
  }
  timer->next = *cursor;
  *cursor = timer;
  timer->queued = 1;
}

static void turbo_posix_timer_finish_callback(turbo_timer_t *timer) {
  if (timer == NULL) {
    return;
  }

  turbo_mutex_lock(&timer->lock);
  timer->callbacks_inflight--;
  if (timer->destroying && timer->callbacks_inflight == 0) {
    turbo_cond_signal(&timer->cond);
  }
  turbo_mutex_unlock(&timer->lock);
}

static void turbo_posix_timer_callback_thread(void *arg) {
  turbo_posix_timer_task_t *task = (turbo_posix_timer_task_t *)arg;
  turbo_timer_t *timer;
  turbo_timer_cb callback;

  if (task == NULL) {
    return;
  }

  timer = task->timer;
  callback = task->callback;
  free(task);

  if (callback != NULL) {
    callback(timer);
  }
  turbo_posix_timer_finish_callback(timer);
}

static void turbo_posix_timer_dispatch(turbo_timer_t *timer, turbo_timer_cb callback) {
  turbo_posix_timer_task_t *task;
  turbo_thread_t worker = NULL;

  if (timer == NULL || callback == NULL) {
    turbo_posix_timer_finish_callback(timer);
    return;
  }

  task = (turbo_posix_timer_task_t *)malloc(sizeof(*task));
  if (task == NULL) {
    callback(timer);
    turbo_posix_timer_finish_callback(timer);
    return;
  }

  task->timer = timer;
  task->callback = callback;
  if (turbo_thread_create(&worker, turbo_posix_timer_callback_thread, task) != 0) {
    free(task);
    callback(timer);
    turbo_posix_timer_finish_callback(timer);
    return;
  }
  turbo_thread_destroy(&worker);
}

static void turbo_posix_timer_manager_thread(void *arg) {
  UNUSED(arg);

  for (;;) {
    turbo_timer_t *timer = NULL;
    turbo_timer_cb callback = NULL;
    uint64_t now_ms;

    turbo_mutex_lock(&g_turbo_posix_timer_manager.lock);
    for (;;) {
      uint64_t wait_ms;

      timer = g_turbo_posix_timer_manager.head;
      if (timer == NULL) {
        turbo_cond_wait(&g_turbo_posix_timer_manager.cond, &g_turbo_posix_timer_manager.lock);
        continue;
      }

      now_ms = turbo_monotonic_ms();
      if (timer->due_ms <= now_ms) {
        break;
      }

      wait_ms = timer->due_ms - now_ms;
      if (turbo_cond_timedwait(&g_turbo_posix_timer_manager.cond,
                               &g_turbo_posix_timer_manager.lock,
                               wait_ms * 1000000ULL) != 0) {
        continue;
      }
    }

    timer = g_turbo_posix_timer_manager.head;
    g_turbo_posix_timer_manager.head = timer->next;
    timer->next = NULL;
    timer->queued = 0;

    turbo_mutex_lock(&timer->lock);
    if (!timer->destroying && timer->active && timer->callback != NULL) {
      callback = timer->callback;
      timer->callbacks_inflight++;
      if (timer->repeat > 0U) {
        uint64_t next_due_ms = timer->due_ms + timer->repeat;

        if (next_due_ms <= now_ms) {
          uint64_t skipped = ((now_ms - timer->due_ms) / timer->repeat) + 1U;
          next_due_ms = timer->due_ms + skipped * timer->repeat;
        }
        timer->due_ms = next_due_ms;
        turbo_posix_timer_queue_insert_locked(timer);
        turbo_cond_signal(&g_turbo_posix_timer_manager.cond);
      } else {
        timer->active = 0;
        timer->due_ms = 0U;
      }
    }
    turbo_mutex_unlock(&timer->lock);
    turbo_mutex_unlock(&g_turbo_posix_timer_manager.lock);

    if (callback != NULL) {
      turbo_posix_timer_dispatch(timer, callback);
    }
  }
}

static void turbo_posix_timer_manager_init_once(void) {
  turbo_thread_t worker = NULL;

  memset(&g_turbo_posix_timer_manager, 0, sizeof(g_turbo_posix_timer_manager));
  turbo_mutex_init(&g_turbo_posix_timer_manager.lock);
  turbo_cond_init(&g_turbo_posix_timer_manager.cond);
  if (turbo_thread_create(&worker, turbo_posix_timer_manager_thread, NULL) != 0) {
    g_turbo_posix_timer_manager.init_failed = 1;
    turbo_cond_destroy(&g_turbo_posix_timer_manager.cond);
    turbo_mutex_destroy(&g_turbo_posix_timer_manager.lock);
    return;
  }

  turbo_thread_destroy(&worker);
  g_turbo_posix_timer_manager.initialized = 1;
}

static int turbo_posix_timer_manager_ensure_started(void) {
  turbo_once(&g_turbo_posix_timer_manager_once, turbo_posix_timer_manager_init_once);
  return g_turbo_posix_timer_manager.initialized && !g_turbo_posix_timer_manager.init_failed
             ? 0
             : -1;
}

turbo_timer_t *turbo_timer_create(void *loop) {
  UNUSED(loop);
  turbo_timer_t *timer = malloc(sizeof(turbo_timer_t));

  if (turbo_posix_timer_manager_ensure_started() != 0) {
    return NULL;
  }

  if (!timer) {
    return NULL;
  }

  memset(timer, 0, sizeof(*timer));
  turbo_mutex_init(&timer->lock);
  turbo_cond_init(&timer->cond);
  return timer;
}

void turbo_timer_destroy(turbo_timer_t *timer) {
  if (!timer) {
    return;
  }

  turbo_mutex_lock(&g_turbo_posix_timer_manager.lock);
  turbo_mutex_lock(&timer->lock);
  timer->destroying = 1;
  timer->callback = NULL;
  timer->active = 0;
  timer->due_ms = 0U;
  turbo_posix_timer_queue_remove_locked(timer);
  turbo_cond_signal(&g_turbo_posix_timer_manager.cond);
  turbo_mutex_unlock(&g_turbo_posix_timer_manager.lock);
  while (timer->callbacks_inflight > 0) {
    turbo_cond_wait(&timer->cond, &timer->lock);
  }
  turbo_mutex_unlock(&timer->lock);

  turbo_cond_destroy(&timer->cond);
  turbo_mutex_destroy(&timer->lock);
  free(timer);
}

int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb, uint64_t timeout, uint64_t repeat) {
  uint64_t due_ms;

  if (!timer || !cb) {
    return -1;
  }

  due_ms = turbo_monotonic_ms() + timeout;
  turbo_mutex_lock(&g_turbo_posix_timer_manager.lock);
  turbo_mutex_lock(&timer->lock);
  if (timer->destroying) {
    turbo_mutex_unlock(&timer->lock);
    turbo_mutex_unlock(&g_turbo_posix_timer_manager.lock);
    return -1;
  }
  turbo_posix_timer_queue_remove_locked(timer);
  timer->callback = cb;
  timer->timeout = timeout;
  timer->repeat = repeat;
  timer->due_ms = due_ms;
  timer->active = 1;
  turbo_posix_timer_queue_insert_locked(timer);
  turbo_mutex_unlock(&timer->lock);
  turbo_cond_signal(&g_turbo_posix_timer_manager.cond);
  turbo_mutex_unlock(&g_turbo_posix_timer_manager.lock);
  return 0;
}

int turbo_timer_stop(turbo_timer_t *timer) {
  if (!timer) {
    return 0;
  }

  turbo_mutex_lock(&g_turbo_posix_timer_manager.lock);
  turbo_mutex_lock(&timer->lock);
  turbo_posix_timer_queue_remove_locked(timer);
  timer->active = 0;
  timer->due_ms = 0U;
  turbo_mutex_unlock(&timer->lock);
  turbo_cond_signal(&g_turbo_posix_timer_manager.cond);
  turbo_mutex_unlock(&g_turbo_posix_timer_manager.lock);
  return 0;
}

#endif

// Common functions for both platforms

void turbo_timer_set_data(turbo_timer_t *timer, void *data) {
  if (timer) {
    timer->data = data;
  }
}

void *turbo_timer_get_data(turbo_timer_t *timer) { return timer ? timer->data : NULL; }

uint64_t turbo_timer_get_repeat(turbo_timer_t *timer) { return timer ? timer->repeat : 0; }
