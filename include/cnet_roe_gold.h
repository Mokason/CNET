/* Operator gold curate — write evolve gold_file, never seal.
 *
 * gold last <answer>  uses last organic miss query
 * gold <query> | <answer>
 *
 * Same identity as roe_evolve_tick (cnet_roe_gold_id.h). Refusal/ABSTAIN
 * cannot become gold. claimed_cert is not a field here — this module does
 * not promote. Kick evolve is optional and still gold_file-only.
 */
#ifndef CNET_ROE_GOLD_H
#define CNET_ROE_GOLD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_GOLD_Q 256
#define CNET_GOLD_A 4096
#define CNET_GOLD_PATH 1024
#define CNET_GOLD_REASON 96

typedef enum {
    CNET_GOLD_CMD_NONE = 0,
    CNET_GOLD_CMD_LAST = 1,
    CNET_GOLD_CMD_QA = 2
} CnetGoldCmd;

typedef struct {
    CnetGoldCmd cmd;
    char query[CNET_GOLD_Q];
    char answer[CNET_GOLD_A];
} CnetGoldReq;

int cnet_gold_parse(const char *q, CnetGoldReq *out); /* 1 if gold verb */
const char *cnet_gold_refusal(const char *answer);    /* NULL if allowed */
int cnet_gold_operator_not_faq(const char *query, const char *answer);
int cnet_gold_last_miss(const char *miss_log, char *query, size_t cap);
int cnet_gold_write(const char *packs, const char *query, const char *answer,
                    const char *blocklist_path, int force, char *path_out,
                    size_t cap, char *reason, size_t rcap);
int cnet_gold_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_ROE_GOLD_H */
