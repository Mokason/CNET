/* SHA-256 for benchmark evidence binding.
 *
 * In-process and shell-free on purpose: the manifest binds artefact hashes, so
 * shelling out to sha256sum would make the integrity check depend on PATH and
 * on argument quoting. Known-answer tested in
 * tests/vision_detection_integrity_test.c.
 */
#ifndef VD_SHA256_H
#define VD_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t s[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} VdSha256;

void vd_sha256_init(VdSha256 *c);
void vd_sha256_update(VdSha256 *c, const void *data, size_t len);
/* writes 64 lowercase hex chars + NUL into out (>= 65 bytes) */
void vd_sha256_hex(VdSha256 *c, char *out);

/* Hash a whole file. Returns 0 on success, -1 if it cannot be read in full.
   out must hold >= 65 bytes. */
int vd_sha256_file(const char *path, char *out);

/* Hash an already-open descriptor from offset 0 using pread, leaving the file
   position untouched. Scored evidence is hashed from the SAME descriptor it was
   read through, so a pathname swapped underneath cannot substitute the bytes. */
int vd_sha256_fd(int fd, char *out);

#ifdef __cplusplus
}
#endif

#endif
