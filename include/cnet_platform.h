/* cnet_platform.h -- the ONE place platform differences are allowed to live.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * `make verify` was dead on Windows/MinGW for 186 commits (2026-07-09 .. 07-19)
 * because POSIX APIs were called with the *include* guarded but the *call site*
 * not: cce_gguf.c had `#include <sys/mman.h>` inside an `#else` branch while the
 * mmap/madvise/munmap block below it compiled unconditionally. The gate died at
 * its third prerequisite (cce_dll), which reads like "a build error" rather than
 * "25 suites are unverified" -- so nobody noticed for months.
 *
 * A shim was written to fix it (commit fb8b83c) but never merged; master kept
 * growing call-site #ifdefs instead (72 of them across 31 files by 2026-08-12),
 * and the build was still red. This file is that shim, restored.
 *
 * HOW TO USE IT
 * -------------
 * Add a new platform primitive HERE. Do not open a fresh `#ifdef _WIN32` at a
 * call site. Call sites should read as straight-line portable C.
 *
 * To find breaks fast, syntax-sweep rather than waiting for make to hit them
 * one file at a time:
 *   for f in $(make -n cce_dll | tr ' ' '\n' | grep '^src/.*\.c$'); do \
 *       gcc -std=c11 -Iinclude -fsyntax-only "$f"; done
 *
 * GUARD SPELLING. Always `defined(_WIN32) || defined(__WIN32__) ||
 * defined(__MINGW32__)`. Bare `_WIN32` was used in some places and the wider
 * spelling in others in the same file, which is how registry.c ended up with a
 * Windows branch calling _mkdir without ever including <direct.h>.
 */
#ifndef CNET_PLATFORM_H
#define CNET_PLATFORM_H

#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
#  define CNET_PLATFORM_WINDOWS 1
#else
#  define CNET_PLATFORM_WINDOWS 0
#endif

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#if CNET_PLATFORM_WINDOWS
#  include <io.h>       /* _commit, _open, _fileno */
#  include <direct.h>   /* _mkdir (MinGW's mkdir() takes ONE argument) */
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

/* ---------------------------------------------------------------------------
 * Capability flags. Guard optional fast paths on these, never on the OS name:
 * the question a call site actually has is "can I mmap here", not "am I on
 * Windows".
 * ------------------------------------------------------------------------- */

/* mmap/munmap/madvise AND fmemopen. Deliberately one flag: the GGUF fast path
 * needs the whole set (it wraps the mapping in a FILE* via fmemopen), so a
 * platform with mmap but no fmemopen must still take the fallback. */
#if CNET_PLATFORM_WINDOWS
#  define CNET_HAVE_MMAP 0
#else
#  define CNET_HAVE_MMAP 1
#endif

/* Named pipes created with mkfifo(). Windows named pipes exist but are a
 * different object with a different namespace (\\.\pipe\...), not something
 * open()/fopen() reaches by path -- so tests that use a FIFO as a blocking sink
 * must SKIP on Windows rather than pretend to run. */
#if CNET_PLATFORM_WINDOWS
#  define CNET_HAVE_MKFIFO 0
#else
#  define CNET_HAVE_MKFIFO 1
#endif

/* fork/exec/waitpid. Windows has no fork(); code needing it must be excluded
 * from the Windows link set rather than shimmed -- a fake fork() would be worse
 * than an honest absence. */
#if CNET_PLATFORM_WINDOWS
#  define CNET_HAVE_FORK_EXEC 0
#else
#  define CNET_HAVE_FORK_EXEC 1
#endif

/* libcurl. The Makefile probes for it and always passes -DCNET_HAVE_CURL=0/1
 * explicitly; this default only covers hand-run compiler invocations (syntax
 * sweeps, one-off gcc calls). It defaults to 0 -- absent -- so that forgetting
 * the flag degrades to "HTTP not compiled in" rather than to a link error or,
 * worse, a build that silently assumes a library that is not there. */
#ifndef CNET_HAVE_CURL
#  define CNET_HAVE_CURL 0
#endif

/* ---------------------------------------------------------------------------
 * open() flag portability.
 *
 * These are CNET_-prefixed on purpose. Defining the bare O_* names ourselves
 * would silently redefine system macros, and -- worse -- `#define O_NOFOLLOW 0`
 * makes a security-hardening flag vanish while the call site still LOOKS
 * hardened. On a project whose thesis is the honesty of its claims, a flag that
 * reads as protection but expands to nothing is the wrong failure mode.
 * ------------------------------------------------------------------------- */

/* Close-on-exec. _O_NOINHERIT is the exact Windows equivalent. */
#if defined(O_CLOEXEC)
#  define CNET_O_CLOEXEC O_CLOEXEC
#elif defined(_O_NOINHERIT)
#  define CNET_O_CLOEXEC _O_NOINHERIT
#else
#  define CNET_O_CLOEXEC 0
#endif

/* Refuse to follow a symlink at open() time.
 *
 * Windows CRT open() has no equivalent, so this is 0 there. That is SAFE ONLY
 * in combination with O_CREAT|O_EXCL, which is how every current caller uses
 * it: O_CREAT|O_EXCL fails if the path exists at all, symlink or not, so a
 * planted link causes an error return rather than a followed write. If you ever
 * use CNET_O_NOFOLLOW WITHOUT O_CREAT|O_EXCL, you must add an explicit
 * reparse-point check on Windows (see the lstat/GetFileAttributes component
 * walk in src/contract/mcp_utils.c for the pattern). */
#if defined(O_NOFOLLOW)
#  define CNET_O_NOFOLLOW O_NOFOLLOW
#  define CNET_HAVE_O_NOFOLLOW 1
#else
#  define CNET_O_NOFOLLOW 0
#  define CNET_HAVE_O_NOFOLLOW 0
#endif

/* ---------------------------------------------------------------------------
 * Primitives.
 * ------------------------------------------------------------------------- */

/* mkdir. `mode` stays a real parameter on purpose: call sites differ (0755 vs
 * 0777) and collapsing them would silently widen or narrow permissions on
 * POSIX. Windows ignores the mode, which is a property of the platform, not a
 * decision this shim gets to make. */
static inline int cnet_mkdir(const char *path, unsigned mode) {
#if CNET_PLATFORM_WINDOWS
    (void)mode;
    return _mkdir(path);
#else
    return mkdir(path, (mode_t)mode);
#endif
}

/* Flush a file descriptor's data to stable storage. _commit is the Windows
 * equivalent of fsync; both return 0 on success and -1 with errno set. */
static inline int cnet_fsync(int fd) {
#if CNET_PLATFORM_WINDOWS
    return _commit(fd);
#else
    return fsync(fd);
#endif
}

/* Exclusive private sibling temporary file. Never reuse another writer's
 * in-flight pathname or follow a planted symlink. */
static inline int cnet_mkstemp(char *tmpl) {
#if CNET_PLATFORM_WINDOWS
    if (!tmpl || _mktemp_s(tmpl, strlen(tmpl) + 1) != 0) return -1;
    return _open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_NOINHERIT, 0600);
#else
    int fd = mkstemp(tmpl);
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd); unlink(tmpl); return -1;
    }
    return fd;
#endif
}

/* mkdtemp(3): rewrite the trailing XXXXXX of `tmpl` in place and create that
 * directory, returning `tmpl` or NULL. Windows has no mkdtemp; _mktemp_s only
 * picks the name, so the directory still has to be created explicitly. Note
 * _mktemp_s offers fewer unique names than mkdtemp before it gives up -- fine
 * for test scratch dirs, which is the only thing that uses this. */
static inline char *cnet_mkdtemp(char *tmpl) {
#if CNET_PLATFORM_WINDOWS
    if (tmpl == NULL) return NULL;
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0) return NULL;
    if (_mkdir(tmpl) != 0) return NULL;
    return tmpl;
#else
    return mkdtemp(tmpl);
#endif
}

/* setenv(3). Windows _putenv_s always overwrites, so emulate the `overwrite`
 * flag explicitly. A NULL value is treated as "" rather than as unset, matching
 * how the current callers use it. */
static inline int cnet_setenv(const char *name, const char *value, int overwrite) {
#if CNET_PLATFORM_WINDOWS
    if (!overwrite) {
        const char *cur = getenv(name);
        if (cur != NULL && cur[0] != '\0') return 0;
    }
    return _putenv_s(name, value ? value : "");
#else
    return setenv(name, value ? value : "", overwrite);
#endif
}


/* ---------------------------------------------------------------------------
 * Windows system headers.
 *
 * WIN32_LEAN_AND_MEAN is REQUIRED, not a compile-time nicety: this header is
 * force-included into every C translation unit (see the CFLAGS line in the
 * Makefile), and a full <windows.h> pulls in <winsock.h>, which makes any later
 * <winsock2.h>/<ws2tcpip.h> a hard error. src/cnet_lookup.c includes exactly
 * that. NOMINMAX keeps the min/max function-like macros out of 235 translation
 * units.
 * ------------------------------------------------------------------------- */
#if CNET_PLATFORM_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  include <windows.h>
#  include <sys/locking.h>
/* windows.h defines IN, OUT and OPTIONAL as empty legacy SAL annotation macros.
 * They expand to nothing -- they are not types, functions or constants -- and
 * they silently rename any ordinary local called IN or OUT. Eleven files in
 * this tree use exactly those names for layer dimensions. Undefining them costs
 * nothing (no Windows API depends on them expanding to anything) and keeps a
 * force-included header from editing other people's variable names.
 *
 * A file that genuinely wants the winioctl/wincrypt/shell layers that
 * WIN32_LEAN_AND_MEAN excludes should include that specific header itself, as
 * tools/gate_evidence.c does for <winioctl.h>. */
#  undef IN
#  undef OUT
#  undef OPTIONAL
#  undef near
#  undef far
#endif

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#if !CNET_PLATFORM_WINDOWS
#  include <dlfcn.h>
#  include <sys/file.h>
#endif

/* ---------------------------------------------------------------------------
 * More open() flags and capability probes.
 * ------------------------------------------------------------------------- */

/* O_DIRECTORY: fail the open when the path is not a directory. The Windows CRT
 * open() cannot open a directory at all, so 0 is the honest mapping -- the open
 * fails on its own, which is the outcome the flag exists to produce. Code that
 * wants "is this a directory" as a question rather than as an open()
 * precondition should use cnet_lstat + S_ISDIR. */
#if defined(O_DIRECTORY)
#  define CNET_O_DIRECTORY O_DIRECTORY
#else
#  define CNET_O_DIRECTORY 0
#endif

/* major()/minor() decomposition of a dev_t. Windows st_dev is a drive number
 * with no major/minor structure, so callers must branch rather than be handed a
 * fabricated split. */
#if CNET_PLATFORM_WINDOWS
#  define CNET_HAVE_SYSMACROS 0
#else
#  define CNET_HAVE_SYSMACROS 1
#endif

/* Whether directory traversal can be pinned to an open descriptor (dirfd +
 * fstatat), which is what makes a walk immune to the directory being swapped
 * underneath it. Windows has no dirfd, so cnet_fstatat_nofollow re-resolves by
 * path there and the TOCTOU protection is genuinely weaker. Security-critical
 * walks should branch on this rather than assume. */
#if CNET_PLATFORM_WINDOWS
#  define CNET_HAVE_DIRFD_RACE_FREE 0
#else
#  define CNET_HAVE_DIRFD_RACE_FREE 1
#endif

/* fork/exec/waitpid availability, spelled as a header question. */
#if CNET_PLATFORM_WINDOWS
#  define CNET_HAVE_SYS_WAIT 0
#else
#  define CNET_HAVE_SYS_WAIT 1
#endif

/* ---------------------------------------------------------------------------
 * Primitives, part 2.
 * ------------------------------------------------------------------------- */

/* lstat(2): stat a path WITHOUT following a final symlink.
 *
 * Every caller in this tree uses it the same way -- lstat then S_ISREG/S_ISDIR
 * -- to refuse a symlink pretending to be a regular file. The Windows CRT stat
 * follows reparse points, so emulating lstat with plain stat would silently
 * delete that check while the call site still looked hardened. Instead a
 * reparse point FAILS here with ELOOP, which lands every existing caller on the
 * exact branch it already takes for "not a regular file". */
static inline int cnet_lstat(const char *path, struct stat *out) {
#if CNET_PLATFORM_WINDOWS
    DWORD attributes;
    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        errno = ELOOP;
        return -1;
    }
    return stat(path, out);
#else
    return lstat(path, out);
#endif
}

/* dirfd(3). Windows has no descriptor behind a DIR*; 0 is returned so the usual
 * "if (fd < 0) bail" guard does not reject a directory that opened fine. The
 * value is only ever handed back to cnet_fstatat_nofollow, which ignores it on
 * Windows. See CNET_HAVE_DIRFD_RACE_FREE. */
static inline int cnet_dirfd(DIR *directory) {
#if CNET_PLATFORM_WINDOWS
    return directory == NULL ? -1 : 0;
#else
    return dirfd(directory);
#endif
}

/* fstatat(dir_fd, name, AT_SYMLINK_NOFOLLOW). dir_path is unused on POSIX and
 * dir_fd is unused on Windows; both are parameters so the call site compiles
 * unchanged on either platform. */
static inline int cnet_fstatat_nofollow(int dir_fd, const char *dir_path,
                                        const char *name, struct stat *out) {
#if CNET_PLATFORM_WINDOWS
    char joined[4096];
    size_t dir_len, name_len;
    (void)dir_fd;
    if (dir_path == NULL || name == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    dir_len = strlen(dir_path);
    name_len = strlen(name);
    if (dir_len + name_len + 2u > sizeof joined) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(joined, dir_path, dir_len);
    joined[dir_len] = (char)0x5C; /* backslash */
    memcpy(joined + dir_len + 1u, name, name_len);
    joined[dir_len + 1u + name_len] = 0;
    return cnet_lstat(joined, out);
#else
    (void)dir_path;
    return fstatat(dir_fd, name, out, AT_SYMLINK_NOFOLLOW);
#endif
}

/* flock(2) advisory whole-file locking.
 *
 * The Windows CRT _locking() has no shared mode: every lock it takes is
 * exclusive. CNET_LOCK_SH therefore maps to an exclusive lock there. That is
 * the conservative direction -- readers serialise against each other where
 * POSIX would let them run concurrently -- so correctness is preserved and only
 * concurrency is lost. _locking works on a byte range from the current file
 * position, so the position is saved and restored around the call. */
#if defined(LOCK_SH)
#  define CNET_LOCK_SH LOCK_SH
#  define CNET_LOCK_EX LOCK_EX
#  define CNET_LOCK_UN LOCK_UN
#  define CNET_LOCK_NB LOCK_NB
#else
#  define CNET_LOCK_SH 1
#  define CNET_LOCK_EX 2
#  define CNET_LOCK_NB 4
#  define CNET_LOCK_UN 8
#endif

static inline int cnet_flock(int fd, int operation) {
#if CNET_PLATFORM_WINDOWS
    long saved;
    int mode, rc;
    if ((operation & CNET_LOCK_UN) != 0)
        mode = _LK_UNLCK;
    else if ((operation & CNET_LOCK_NB) != 0)
        mode = _LK_NBLCK;
    else
        mode = _LK_LOCK;
    saved = _lseek(fd, 0L, SEEK_CUR);
    if (saved < 0L) return -1;
    if (_lseek(fd, 0L, SEEK_SET) < 0L) return -1;
    rc = _locking(fd, mode, 1L);
    (void)_lseek(fd, saved, SEEK_SET);
    return rc;
#else
    return flock(fd, operation);
#endif
}

/* fmemopen(3), read paths only -- every caller in this tree passes "rb".
 * Windows gets a tmpfile() primed with the buffer, which reads byte-identically
 * and is deleted on close. A write mode would need the contents copied back on
 * flush and is refused outright rather than silently dropped. */
static inline FILE *cnet_fmemopen(void *buffer, size_t size, const char *mode) {
#if CNET_PLATFORM_WINDOWS
    FILE *stream;
    if (buffer == NULL || mode == NULL || strchr(mode, 'r') == NULL) {
        errno = EINVAL;
        return NULL;
    }
    stream = tmpfile();
    if (stream == NULL) return NULL;
    if (size > 0u && fwrite(buffer, 1u, size, stream) != size) {
        fclose(stream);
        errno = EIO;
        return NULL;
    }
    rewind(stream);
    return stream;
#else
    return fmemopen(buffer, size, mode);
#endif
}

/* unsetenv(3). _putenv_s with an empty value is how the Windows CRT drops a
 * variable from the environment block. */
static inline int cnet_unsetenv(const char *name) {
#if CNET_PLATFORM_WINDOWS
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

/* pread(2). The Windows emulation is seek-read-seek and is therefore NOT atomic
 * against another thread seeking the same descriptor; it is used here only on
 * descriptors owned by the calling thread. The file position is restored, so
 * callers cannot otherwise tell the difference. */
static inline long cnet_pread(int fd, void *buffer, size_t count,
                              long long offset) {
#if CNET_PLATFORM_WINDOWS
    long long saved;
    int got;
    saved = _lseeki64(fd, 0, SEEK_CUR);
    if (saved < 0) return -1;
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    got = _read(fd, buffer, (unsigned int)count);
    (void)_lseeki64(fd, saved, SEEK_SET);
    return (long)got;
#else
    return (long)pread(fd, buffer, count, (off_t)offset);
#endif
}

/* getline(3): read one line, growing *line to fit. Returns the length read
 * excluding the NUL, or -1 at EOF or on error. */
static inline long cnet_getline(char **line, size_t *capacity, FILE *stream) {
#if CNET_PLATFORM_WINDOWS
    size_t length = 0u;
    int ch;
    if (line == NULL || capacity == NULL || stream == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*line == NULL || *capacity == 0u) {
        char *fresh = (char *)malloc(128u);
        if (fresh == NULL) return -1;
        *line = fresh;
        *capacity = 128u;
    }
    for (;;) {
        ch = fgetc(stream);
        if (ch == EOF) {
            if (length == 0u) return -1;
            break;
        }
        if (length + 2u > *capacity) {
            size_t grown = *capacity * 2u;
            char *bigger = (char *)realloc(*line, grown);
            if (bigger == NULL) return -1;
            *line = bigger;
            *capacity = grown;
        }
        (*line)[length++] = (char)ch;
        if (ch == 10) break; /* newline */
    }
    (*line)[length] = 0;
    return (long)length;
#else
    return (long)getline(line, capacity, stream);
#endif
}

/* localtime_r(3). The Windows localtime_s takes its arguments in the opposite
 * order and returns errno_t rather than a pointer. */
static inline struct tm *cnet_localtime_r(const time_t *clock,
                                          struct tm *result) {
#if CNET_PLATFORM_WINDOWS
    if (clock == NULL || result == NULL) return NULL;
    return localtime_s(result, clock) == 0 ? result : NULL;
#else
    return localtime_r(clock, result);
#endif
}

/* geteuid(2). Windows has no uid. 0 is returned because the single caller uses
 * it for an "am I root" ownership check, and on Windows the thing standing
 * between that caller and the file is the ACL, not a uid. Code that needs a
 * real privilege answer must ask Windows directly rather than read this. */
static inline long cnet_geteuid(void) {
#if CNET_PLATFORM_WINDOWS
    return 0L;
#else
    return (long)geteuid();
#endif
}

/* fchmod(2). The Windows CRT can only set the read-only bit, and only by path,
 * so this reports success without acting -- matching _chmod's actual reach
 * rather than pretending POSIX mode bits apply to an NTFS ACL. */
static inline int cnet_fchmod(int fd, unsigned mode) {
#if CNET_PLATFORM_WINDOWS
    (void)fd;
    (void)mode;
    return 0;
#else
    return fchmod(fd, (mode_t)mode);
#endif
}

/* Online CPU count, for sizing worker pools. Returns at least 1. */
static inline int cnet_cpu_count(void) {
#if CNET_PLATFORM_WINDOWS
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors > 0 ? (int)info.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* Dynamic loading, mirroring dlopen/dlsym/dlclose. The function-pointer to
 * object-pointer conversion in cnet_dlsym is not strictly conforming C, which
 * is exactly why it lives here once instead of at every call site. */
static inline void *cnet_dlopen(const char *path) {
#if CNET_PLATFORM_WINDOWS
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW);
#endif
}

static inline void *cnet_dlsym(void *handle, const char *symbol) {
#if CNET_PLATFORM_WINDOWS
    union {
        FARPROC in;
        void *out;
    } bridge;
    if (handle == NULL || symbol == NULL) return NULL;
    bridge.in = GetProcAddress((HMODULE)handle, symbol);
    return bridge.out;
#else
    return dlsym(handle, symbol);
#endif
}

static inline int cnet_dlclose(void *handle) {
#if CNET_PLATFORM_WINDOWS
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
#else
    return dlclose(handle);
#endif
}


/* st_mtim / st_ctim: the POSIX nanosecond-resolution stat timestamps.
 *
 * The Windows CRT struct stat carries only whole-second st_mtime/st_ctime, so
 * tv_nsec is 0 there and any comparison is second-resolution. Callers using
 * these to notice a file changing underneath them keep working; they simply
 * cannot distinguish two writes inside the same second on Windows. That is a
 * real reduction in what the check can detect, which is why it is spelled out
 * here rather than hidden behind a struct copy. */
static inline struct timespec cnet_stat_mtim(const struct stat *st) {
    struct timespec ts;
#if CNET_PLATFORM_WINDOWS
    ts.tv_sec = st->st_mtime;
    ts.tv_nsec = 0;
#else
    ts = st->st_mtim;
#endif
    return ts;
}

static inline struct timespec cnet_stat_ctim(const struct stat *st) {
    struct timespec ts;
#if CNET_PLATFORM_WINDOWS
    ts.tv_sec = st->st_ctime;
    ts.tv_nsec = 0;
#else
    ts = st->st_ctim;
#endif
    return ts;
}

#endif /* CNET_PLATFORM_H */
