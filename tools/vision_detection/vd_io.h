/* Shell-free directory creation and transactional file publication.
 *
 * Both exist because the benchmark's outputs are evidence: a path that reaches
 * a shell is an injection surface, and a half-written manifest or results file
 * that a later run reads back is worse than no file at all.
 */
#ifndef VD_IO_H
#define VD_IO_H

#include <stddef.h>
#include <sys/types.h>

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

/* ---------------- component-wise no-follow traversal ---------------------
   Every path component is opened with O_DIRECTORY|O_NOFOLLOW from a trusted
   start, so a symlink planted at ANY intermediate position is refused, not just
   at the leaf. ".." components, non-directories, over-long paths and excessive
   depth are rejected at every level. */
int vd_open_dir_nofollow(const char *path);        /* dirfd, or -1 */
int vd_mkdir_p_nofollow(const char *path);         /* component-wise mkdirat */

/* Open a regular file below dirfd with O_NOFOLLOW and validate it really is a
   regular file. Returns the fd, or -1. */
int vd_openat_regular(int dirfd, const char *name, off_t *size_out);

/* A FILE* over a dup of fd, positioned at 0. Caller fcloses it; fd is untouched. */
void *vd_fdopen_ro(int fd);

/* Open a regular file at `path` by traversing its parent component-wise with
   no-follow semantics and openat-ing the leaf. Returns the fd and its size. The
   caller then parses AND hashes that one descriptor, so a pathname swapped
   afterwards cannot substitute the bytes. */
int vd_open_file_nofollow(const char *path, off_t *size_out, off_t max_bytes);

/* Write buf to path atomically: O_NOFOLLOW|O_EXCL temp beside the target,
   checked write loop, fsync, close, rename, then fsync of the directory.
   Never follows a symlink at the destination. Returns 0 on success, -1 on any
   failure with the temp removed. */
int vd_publish_file(const char *path, const void *buf, size_t len);

/* ---------------- directory-atomic cache publication ---------------------
   A cache is only meaningful whole. These build it in a fresh, exclusively
   created sibling staging directory, create every member with
   openat(O_CREAT|O_EXCL|O_NOFOLLOW) so a planted symlink can never be written
   through, and publish the finished directory in one step. A partially written
   staging directory is never visible at the destination. */
typedef struct {
    int parent_fd;              /* destination's parent, opened O_NOFOLLOW */
    int dir_fd;                 /* the staging directory */
    int lock_fd;                /* parent-local publication lock, held through */
    char base[VD_PATH_MAX];     /* destination basename */
    char stage[VD_PATH_MAX];    /* staging basename */
    int done;
    int created;                /* staging directory exists on disk */
    /* Staging is never deleted. If a stage was created and not published, its
       unique path is reported here for offline operator cleanup. */
    int quarantined;
    char quarantine[VD_PATH_MAX];
} VdStage;

/* Name of the left-behind cache after a quarantined publish, or NULL. */
const char *vd_stage_quarantine(const VdStage *st);

/* Test-only hook, fired inside vd_stage_commit so the continuity and rollback
   paths can be exercised deterministically instead of raced. */
typedef enum {
    VD_HOOK_AFTER_VALIDATE = 1,   /* stage identity checked, not yet published */
    VD_HOOK_AFTER_EXCHANGE = 2    /* published, visibility not yet confirmed */
} VdStageHookPhase;
void vd_stage_set_hook(void (*fn)(VdStageHookPhase, void *), void *ctx);

/* Rejects a symlinked parent or a symlinked destination outright. */
int vd_stage_begin(const char *dest, VdStage *st);

/* Publish the staged cache into an ABSENT destination, and only that.
   RENAME_NOREPLACE from the held parent dirfd; if the destination exists, or
   RENAME_NOREPLACE is unavailable, this fails closed. There is deliberately no
   replace mode: nothing here ever swaps out or deletes an existing cache, so a
   publication can never destroy one. Callers that want to re-prep choose a
   fresh output path. Returns 0 only when the complete cache is visible. */
int vd_stage_commit(VdStage *st);
void vd_stage_abort(VdStage *st);

/* Buffered, fully checked writer for one member of the staging directory. */
typedef struct {
    int fd;
    unsigned char *buf;
    size_t cap, n;
    int err;
    void *sha;                  /* VdSha256, hashed as it is written */
    char hex[65];
    unsigned long long written;
} VdOut;

int vd_out_open(VdStage *st, const char *name, VdOut *o);
int vd_out_write(VdOut *o, const void *p, size_t n);
int vd_out_finish(VdOut *o);    /* flush + fsync + close; 0 only if all ok */
/* Digest of everything written. Valid only after a successful vd_out_finish. */
int vd_out_digest(VdOut *o, char *hex_out);

#ifdef __cplusplus
}
#endif

#endif
