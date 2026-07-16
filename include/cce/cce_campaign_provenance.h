#ifndef CCE_CAMPAIGN_PROVENANCE_H
#define CCE_CAMPAIGN_PROVENANCE_H

#include <stddef.h>

/* Compute the lowercase SHA-256 digest of a file. hex_out needs 65 bytes. */
int cce_sha256_file_hex(const char *path, char hex_out[65]);

/* Fail-closed validation for a model-backed campaign replay manifest.
 * Returns 0 only when source revision/dirty state and every required artifact
 * fingerprint match. The diagnostic buffer always explains a refusal. */
int cce_campaign_provenance_verify(const char *manifest_path,
                                   const char *executable_path,
                                   const char *live_build_rev,
                                   int live_source_dirty,
                                   char *error,
                                   size_t error_cap);

#endif /* CCE_CAMPAIGN_PROVENANCE_H */
