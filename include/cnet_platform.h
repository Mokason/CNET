#ifndef CNET_PLATFORM_H
#define CNET_PLATFORM_H

/* Single POSIX/Windows compatibility layer for the tree.
 *
 * Before this header there were eight hand-rolled copies of the same two
 * shims under four different macro names (WS_MKDIR, CNET_MKDIR, MKDIR,
 * TOPO_MKDIR, plus inline #ifdefs in cce_safetensors.c and agent_memory.c),
 * and three call sites that had no shim at all — which is why
 * cce_gguf.c/cce_kv_page.c/cce_qgkp.c stopped compiling on MinGW.
 *
 * Add a new primitive here rather than opening another #ifdef at a call site.
 */

#include <errno.h>   /* mkdir callers universally test errno == EEXIST */
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
   /* No mmap/madvise/fmemopen on Windows. Callers must treat mapping as an
      optional fast path and keep the stdio fallback compiled in. */
#  define CNET_HAVE_MMAP 0
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define CNET_HAVE_MMAP 1
#endif

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/* Linux's NTP-unadjusted monotonic clock. Elsewhere the plain monotonic clock
   is the closest equivalent — both are monotonic, which is all callers want. */
#if !defined(CLOCK_MONOTONIC_RAW) && defined(CLOCK_MONOTONIC)
#  define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

/* Create a single directory. `mode` is the POSIX permission bits; Windows has
   no equivalent and ignores it. Returns 0 on success and -1 with errno set on
   failure, exactly like mkdir(2) — callers still check errno == EEXIST.
   The mode stays a parameter because call sites differ (0755 vs 0777) and
   collapsing them would silently widen or narrow directory permissions. */
static inline int cnet_mkdir(const char *path, int mode) {
#ifdef _WIN32
    (void)mode;
    return _mkdir(path);
#else
    return mkdir(path, (mode_t)mode);
#endif
}

/* Force a file descriptor's data out to disk. 0 on success, -1 on failure. */
static inline int cnet_fsync(int fd) {
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}

/* setenv(3)/unsetenv(3). Windows has only _putenv_s, which always overwrites
   and deletes on an empty value, so the POSIX overwrite==0 case is emulated. */
static inline int cnet_setenv(const char *name, const char *value, int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

static inline int cnet_unsetenv(const char *name) {
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

/* mkdtemp(3): `tmpl` must end in "XXXXXX" and is rewritten in place. Returns
   tmpl on success, NULL on failure. Windows splits this into name-then-create,
   so unlike POSIX it is not atomic — fine for test scratch dirs, which is the
   only thing that calls it here. */
static inline char *cnet_mkdtemp(char *tmpl) {
#ifdef _WIN32
    if (!_mktemp(tmpl)) return NULL;
    if (_mkdir(tmpl) != 0) return NULL;
    return tmpl;
#else
    return mkdtemp(tmpl);
#endif
}

/* Online CPU count, >= 1. Reads the environment on Windows rather than pulling
   windows.h into every translation unit that includes this header. */
static inline long cnet_cpu_count(void) {
#ifdef _WIN32
    const char *n = getenv("NUMBER_OF_PROCESSORS");
    long c = n ? atol(n) : 0;
    return c > 0 ? c : 1;
#else
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return c > 0 ? c : 1;
#endif
}

#endif /* CNET_PLATFORM_H */
