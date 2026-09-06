/* Durable markdown memory — store/recall notes. Never CERT.
 * Path: CNET_MEMORY_MD, else OBSIDIAN_VAULT_PATH/CNET/Marble-memory.md,
 * else $CNET_MINIMAL_ROOT/var/marble_memory.md.
 * Gate: make md_memory → MD_MEMORY_PASS
 */
#ifndef CNET_MD_MEMORY_H
#define CNET_MD_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_MD_MEM_PATH 512
#define CNET_MD_MEM_NOTE 512

int cnet_md_mem_path(char *out, size_t cap);
int cnet_md_mem_store(const char *peer, const char *note);
/* 0 = wrote a hit into out; 1 = no match; <0 error. claimed_cert stays 0. */
int cnet_md_mem_recall(const char *substr, char *out, size_t cap);
/* Log an unknown as GAP. Lookup draft is stored uncertified. Never CERT. */
int cnet_md_mem_gap(const char *peer, const char *query, const char *draft);
/* Last open gaps into out. 0 if any. */
int cnet_md_mem_list_gaps(char *out, size_t cap);
int cnet_md_mem_selftest(void);

#ifdef __cplusplus
}
#endif

#endif
