/* Shell-free directory creation and transactional file publication.
 *
 * Both exist because the benchmark's outputs are evidence: a path that reaches
 * a shell is an injection surface, and a half-written manifest or results file
 * that a later run reads back is worse than no file at all.
 */
#ifndef VD_IO_H
#define VD_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_PATH_MAX 1024

/* Reject paths this code will not create: empty, over VD_PATH_MAX, containing a
   NUL-adjacent oddity, a ".." component, or a shell metacharacter. Returns 0 if
   acceptable, -1 otherwise. */
int vd_path_ok(const char *path);

/* Recursive mkdir without invoking a shell. Bounded by VD_PATH_MAX and by
   VD_MKDIR_MAX_DEPTH components. Existing directories are fine; an existing
   non-directory is an error. Returns 0 on success, -1 on failure. */
#define VD_MKDIR_MAX_DEPTH 32
int vd_mkdir_p(const char *path);

/* Write buf to path atomically: O_NOFOLLOW|O_EXCL temp beside the target,
   checked write loop, fsync, close, rename, then fsync of the directory.
   Never follows a symlink at the destination. Returns 0 on success, -1 on any
   failure with the temp removed. */
int vd_publish_file(const char *path, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
