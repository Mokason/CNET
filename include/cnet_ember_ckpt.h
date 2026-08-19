#ifndef CNET_EMBER_CKPT_H
#define CNET_EMBER_CKPT_H

/* Ember session checkpoint index (DS4-kvstore shaped, text residual only).
 * Prefix-hash of rendered transcript bytes → disk payload.
 * Neural HOT/WARM/COLD stays in cce_kv_pager; this is the *session* index.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_EMBER_SHA_HEX 41
#define CNET_EMBER_CKPT_PATH 512
#define CNET_EMBER_CKPT_MAX 256

typedef enum {
    CNET_EMBER_CKPT_UNKNOWN = 0,
    CNET_EMBER_CKPT_COLD = 1,
    CNET_EMBER_CKPT_CONTINUED = 2,
    CNET_EMBER_CKPT_EVICT = 3,
    CNET_EMBER_CKPT_COMPACT = 4,
    CNET_EMBER_CKPT_SHUTDOWN = 5
} CnetEmberCkptReason;

typedef struct {
    char sha[CNET_EMBER_SHA_HEX];
    char path[CNET_EMBER_CKPT_PATH];
    uint8_t reason;
    uint32_t turns;
    uint32_t hits;
    uint64_t created_at;
    uint64_t last_used;
    uint64_t text_bytes;
    uint64_t file_size;
} CnetEmberCkptEntry;

typedef struct {
    char dir[CNET_EMBER_CKPT_PATH];
    uint64_t budget_bytes; /* default 64 MiB */
    int min_chars;         /* min transcript len to store */
    CnetEmberCkptEntry *v;
    int n;
    int cap;
    int open;
} CnetEmberCkptStore;

void cnet_ember_ckpt_opts_default(CnetEmberCkptStore *s);
int cnet_ember_ckpt_open(CnetEmberCkptStore *s, const char *dir,
                         uint64_t budget_mb);
void cnet_ember_ckpt_close(CnetEmberCkptStore *s);

/* SHA1 hex of exact prefix bytes (41 chars incl NUL). */
void cnet_ember_sha1_hex(const void *ptr, size_t len, char out[CNET_EMBER_SHA_HEX]);

/* Longest prefix match for prompt text. Returns index or -1. */
int cnet_ember_ckpt_find_prefix(CnetEmberCkptStore *s, const char *text,
                                size_t text_len);

/* Load payload text for entry index into buf. Returns bytes or -1. */
int cnet_ember_ckpt_load(CnetEmberCkptStore *s, int idx, char *buf, size_t cap,
                         size_t *out_len);

/* Store checkpoint of text. reason from enum. Evicts by budget. */
int cnet_ember_ckpt_store(CnetEmberCkptStore *s, const char *text, size_t text_len,
                          uint32_t turns, CnetEmberCkptReason reason);

const char *cnet_ember_ckpt_reason_name(CnetEmberCkptReason r);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EMBER_CKPT_H */
