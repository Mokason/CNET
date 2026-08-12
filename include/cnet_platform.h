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

#endif /* CNET_PLATFORM_H */
