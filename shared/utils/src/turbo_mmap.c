/**
 * @file turbo_mmap.c
 * @brief Cross-platform Memory-Mapped File I/O Implementation
 *
 * Windows: CreateFileMapping + MapViewOfFile
 * POSIX:   mmap + munmap
 */

#include "platform.h"
#include "turbo_mmap.h"
#include <string.h>

#ifdef _WIN32
  #include <io.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// =============================================================================
// Platform-specific helpers
// =============================================================================

static size_t get_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwAllocationGranularity;
#else
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096;
#endif
}

static int64_t align_offset(int64_t offset, size_t page_size) {
    return (offset / (int64_t)page_size) * (int64_t)page_size;
}

static int64_t get_file_size_fd(intptr_t fd) {
#ifdef _WIN32
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)fd, &size)) {
        return -1;
    }
    return (int64_t)size.QuadPart;
#else
    struct stat st;
    if (fstat((int)fd, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_size;
#endif
}

// =============================================================================
// Core Implementation
// =============================================================================

CXX_C_API void turbo_mmap_init(turbo_mmap_t *mmap_ptr) {
    if (!mmap_ptr) return;
    memset(mmap_ptr, 0, sizeof(*mmap_ptr));
#ifdef _WIN32
    mmap_ptr->file_handle = INVALID_HANDLE_VALUE;
    mmap_ptr->map_handle = NULL;
#else
    mmap_ptr->fd = -1;
    mmap_ptr->owns_fd = false;
#endif
}

CXX_C_API size_t turbo_mmap_page_size(void) {
    return get_page_size();
}

CXX_C_API size_t turbo_mmap_page_count(size_t size) {
    size_t page_size = get_page_size();
    return (size + page_size - 1) / page_size;
}

CXX_C_API size_t turbo_mmap_pages(const turbo_mmap_t *mmap_ptr) {
    if (!mmap_ptr || !mmap_ptr->is_mapped) return 0;
    size_t page_size = get_page_size();
    return (mmap_ptr->length + page_size - 1) / page_size;
}

CXX_C_API int turbo_mmap_open(turbo_mmap_t *mmap_ptr, const char *path, int access) {
    return turbo_mmap_open_range(mmap_ptr, path, 0, 0, access);
}

CXX_C_API int turbo_mmap_open_range(turbo_mmap_t *mmap_ptr, const char *path,
                                    int64_t offset, size_t length, int access) {
    if (!mmap_ptr || !path) {
        return TURBO_MMAP_EINVAL;
    }
    if (mmap_ptr->is_mapped) {
        return TURBO_MMAP_EEXIST;
    }

    turbo_mmap_init(mmap_ptr);

#ifdef _WIN32
    // Open file
    DWORD desired_access = GENERIC_READ;
    DWORD share_mode = FILE_SHARE_READ;
    if (access & TURBO_MMAP_WRITE) {
        desired_access |= GENERIC_WRITE;
        share_mode |= FILE_SHARE_WRITE;
    }

    HANDLE hFile = CreateFileA(path, desired_access, share_mode, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return TURBO_MMAP_ENOENT;
        }
        if (err == ERROR_ACCESS_DENIED) {
            return TURBO_MMAP_EACCES;
        }
        return TURBO_MMAP_EIO;
    }

    mmap_ptr->file_handle = hFile;
    int result = turbo_mmap_from_fd(mmap_ptr, (intptr_t)hFile, offset, length, access);
    if (result != TURBO_MMAP_OK) {
        CloseHandle(hFile);
        mmap_ptr->file_handle = INVALID_HANDLE_VALUE;
    }
    return result;

#else
    // Open file
    int flags = (access & TURBO_MMAP_WRITE) ? O_RDWR : O_RDONLY;
    int fd = open(path, flags);
    if (fd < 0) {
        if (errno == ENOENT) return TURBO_MMAP_ENOENT;
        if (errno == EACCES) return TURBO_MMAP_EACCES;
        return TURBO_MMAP_EIO;
    }

    mmap_ptr->fd = fd;
    mmap_ptr->owns_fd = true;
    int result = turbo_mmap_from_fd(mmap_ptr, fd, offset, length, access);
    if (result != TURBO_MMAP_OK) {
        close(fd);
        mmap_ptr->fd = -1;
        mmap_ptr->owns_fd = false;
    }
    return result;
#endif
}

CXX_C_API int turbo_mmap_from_fd(turbo_mmap_t *mmap_ptr, intptr_t fd,
                                 int64_t offset, size_t length, int access) {
    if (!mmap_ptr) {
        return TURBO_MMAP_EINVAL;
    }

    // Get file size
    int64_t file_size = get_file_size_fd(fd);
    if (file_size < 0) {
        return TURBO_MMAP_EIO;
    }
    if (file_size == 0) {
        return TURBO_MMAP_EEMPTY;
    }

    // Calculate mapping range
    if (offset < 0 || offset >= file_size) {
        return TURBO_MMAP_EINVAL;
    }

    if (length == 0) {
        length = (size_t)(file_size - offset);
    }
    if (offset + (int64_t)length > file_size) {
        length = (size_t)(file_size - offset);
    }

    // Align offset to page boundary
    size_t page_size = get_page_size();
    int64_t aligned_offset = align_offset(offset, page_size);
    size_t offset_diff = (size_t)(offset - aligned_offset);
    size_t mapped_length = length + offset_diff;

    mmap_ptr->offset = offset;
    mmap_ptr->aligned_offset = aligned_offset;
    mmap_ptr->length = length;
    mmap_ptr->mapped_length = mapped_length;
    mmap_ptr->access = access;

#ifdef _WIN32
    // Create file mapping
    DWORD protect = (access & TURBO_MMAP_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE hMap = CreateFileMappingA((HANDLE)fd, NULL, protect, 0, 0, NULL);
    if (!hMap) {
        return TURBO_MMAP_ENOMEM;
    }

    // Map view
    DWORD map_access = (access & TURBO_MMAP_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
    DWORD offset_high = (DWORD)(aligned_offset >> 32);
    DWORD offset_low = (DWORD)(aligned_offset & 0xFFFFFFFF);

    void *ptr = MapViewOfFile(hMap, map_access, offset_high, offset_low, mapped_length);
    if (!ptr) {
        CloseHandle(hMap);
        return TURBO_MMAP_ENOMEM;
    }

    mmap_ptr->map_handle = hMap;
    mmap_ptr->data = (char *)ptr + offset_diff;
    mmap_ptr->is_mapped = true;

#else
    // POSIX mmap
    int prot = PROT_READ;
    if (access & TURBO_MMAP_WRITE) prot |= PROT_WRITE;
    if (access & TURBO_MMAP_EXEC) prot |= PROT_EXEC;

    void *ptr = mmap(NULL, mapped_length, prot, MAP_SHARED, (int)fd, aligned_offset);
    if (ptr == MAP_FAILED) {
        if (errno == ENOMEM) return TURBO_MMAP_ENOMEM;
        if (errno == EACCES) return TURBO_MMAP_EACCES;
        return TURBO_MMAP_EIO;
    }

    mmap_ptr->data = (char *)ptr + offset_diff;
    mmap_ptr->is_mapped = true;
#endif

    return TURBO_MMAP_OK;
}

CXX_C_API int turbo_mmap_sync(turbo_mmap_t *mmap_ptr, bool async) {
    if (!mmap_ptr || !mmap_ptr->is_mapped) {
        return TURBO_MMAP_EINVAL;
    }
    return turbo_mmap_sync_range(mmap_ptr, 0, mmap_ptr->length, async);
}

CXX_C_API int turbo_mmap_sync_range(turbo_mmap_t *mmap_ptr, size_t offset,
                                    size_t length, bool async) {
    if (!mmap_ptr || !mmap_ptr->is_mapped) {
        return TURBO_MMAP_EINVAL;
    }
    if (offset + length > mmap_ptr->length) {
        return TURBO_MMAP_EINVAL;
    }

    // Get actual mapped base (before user offset adjustment)
    size_t offset_diff = (size_t)(mmap_ptr->offset - mmap_ptr->aligned_offset);
    char *base = (char *)mmap_ptr->data - offset_diff;

#ifdef _WIN32
    (void)async; // Windows FlushViewOfFile is always sync
    if (!FlushViewOfFile(base + offset_diff + offset, length)) {
        return TURBO_MMAP_EIO;
    }
    return TURBO_MMAP_OK;
#else
    int flags = async ? MS_ASYNC : MS_SYNC;
    if (msync(base + offset_diff + offset, length, flags) != 0) {
        return TURBO_MMAP_EIO;
    }
    return TURBO_MMAP_OK;
#endif
}

CXX_C_API void turbo_mmap_unmap(turbo_mmap_t *mmap_ptr) {
    if (!mmap_ptr || !mmap_ptr->is_mapped) {
        return;
    }

    // Get actual mapped base
    size_t offset_diff = (size_t)(mmap_ptr->offset - mmap_ptr->aligned_offset);
    char *base = (char *)mmap_ptr->data - offset_diff;

#ifdef _WIN32
    UnmapViewOfFile(base);
    if (mmap_ptr->map_handle) {
        CloseHandle(mmap_ptr->map_handle);
        mmap_ptr->map_handle = NULL;
    }
#else
    munmap(base, mmap_ptr->mapped_length);
#endif

    mmap_ptr->data = NULL;
    mmap_ptr->is_mapped = false;
}

CXX_C_API void turbo_mmap_close(turbo_mmap_t *mmap_ptr) {
    if (!mmap_ptr) return;

    turbo_mmap_unmap(mmap_ptr);

#ifdef _WIN32
    if (mmap_ptr->file_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(mmap_ptr->file_handle);
        mmap_ptr->file_handle = INVALID_HANDLE_VALUE;
    }
#else
    if (mmap_ptr->owns_fd && mmap_ptr->fd >= 0) {
        close(mmap_ptr->fd);
    }
    mmap_ptr->fd = -1;
    mmap_ptr->owns_fd = false;
#endif
}

// =============================================================================
// Utilities
// =============================================================================

CXX_C_API int turbo_mmap_advise(turbo_mmap_t *mmap_ptr, turbo_mmap_advice_t advice) {
    if (!mmap_ptr || !mmap_ptr->is_mapped) {
        return TURBO_MMAP_EINVAL;
    }

#ifdef _WIN32
    // Windows doesn't have madvise equivalent
    (void)advice;
    return TURBO_MMAP_OK;
#else
    int posix_advice;
    switch (advice) {
        case TURBO_MMAP_SEQUENTIAL: posix_advice = MADV_SEQUENTIAL; break;
        case TURBO_MMAP_RANDOM:     posix_advice = MADV_RANDOM; break;
        case TURBO_MMAP_WILLNEED:   posix_advice = MADV_WILLNEED; break;
        case TURBO_MMAP_DONTNEED:   posix_advice = MADV_DONTNEED; break;
        default:                    posix_advice = MADV_NORMAL; break;
    }

    size_t offset_diff = (size_t)(mmap_ptr->offset - mmap_ptr->aligned_offset);
    char *base = (char *)mmap_ptr->data - offset_diff;

    if (madvise(base, mmap_ptr->mapped_length, posix_advice) != 0) {
        return TURBO_MMAP_EIO;
    }
    return TURBO_MMAP_OK;
#endif
}

CXX_C_API int turbo_mmap_lock(turbo_mmap_t *mmap_ptr) {
    if (!mmap_ptr || !mmap_ptr->is_mapped) {
        return TURBO_MMAP_EINVAL;
    }

    size_t offset_diff = (size_t)(mmap_ptr->offset - mmap_ptr->aligned_offset);
    char *base = (char *)mmap_ptr->data - offset_diff;

#ifdef _WIN32
    if (!VirtualLock(base, mmap_ptr->mapped_length)) {
        return TURBO_MMAP_EIO;
    }
    return TURBO_MMAP_OK;
#else
    if (mlock(base, mmap_ptr->mapped_length) != 0) {
        return TURBO_MMAP_EIO;
    }
    return TURBO_MMAP_OK;
#endif
}

CXX_C_API int turbo_mmap_unlock(turbo_mmap_t *mmap_ptr) {
    if (!mmap_ptr || !mmap_ptr->is_mapped) {
        return TURBO_MMAP_EINVAL;
    }

    size_t offset_diff = (size_t)(mmap_ptr->offset - mmap_ptr->aligned_offset);
    char *base = (char *)mmap_ptr->data - offset_diff;

#ifdef _WIN32
    if (!VirtualUnlock(base, mmap_ptr->mapped_length)) {
        return TURBO_MMAP_EIO;
    }
    return TURBO_MMAP_OK;
#else
    if (munlock(base, mmap_ptr->mapped_length) != 0) {
        return TURBO_MMAP_EIO;
    }
    return TURBO_MMAP_OK;
#endif
}

CXX_C_API const char *turbo_mmap_strerror(int err) {
    switch (err) {
        case TURBO_MMAP_OK:     return "Success";
        case TURBO_MMAP_EINVAL: return "Invalid argument";
        case TURBO_MMAP_ENOENT: return "File not found";
        case TURBO_MMAP_EACCES: return "Permission denied";
        case TURBO_MMAP_ENOMEM: return "Out of memory";
        case TURBO_MMAP_EEXIST: return "Already mapped";
        case TURBO_MMAP_EIO:    return "I/O error";
        case TURBO_MMAP_ENOSYS: return "Not supported";
        case TURBO_MMAP_EEMPTY: return "File is empty";
        default:               return "Unknown error";
    }
}
