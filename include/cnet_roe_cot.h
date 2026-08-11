/* ROE chain-of-thought — multi-hop operational thinking in C (not Python).
 *
 * Token-free skeleton: PARSE → SPLIT → RETRIEVE → CHECK → ACT* → JOIN → VERIFY → SHOW
 * Leaf ACT may call ./bin/roe_front_door (still C). Teacher only on miss hops.
 *
 * Law: not consciousness, not AGI, never self-CERT from the chain alone.
 */
#ifndef CNET_ROE_COT_H
#define CNET_ROE_COT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_COT_MAX_HOPS 8
#define ROE_COT_TEXT 512
#define ROE_COT_LINE 384
#define ROE_COT_ANS 512
#define ROE_COT_PATH 1024
#define ROE_COT_ID 64
#define ROE_COT_CMD 2048

typedef enum {
    ROE_COT_PARSE = 1,
    ROE_COT_SPLIT = 2,
    ROE_COT_RETRIEVE = 3,
    ROE_COT_CHECK = 4,
    ROE_COT_ACT = 5,
    ROE_COT_JOIN = 6,
    ROE_COT_VERIFY = 7,
    ROE_COT_SHOW = 8,
    ROE_COT_STOP = 9
} RoeCotKind;

typedef enum {
    ROE_COT_OK = 0,
    ROE_COT_MISS = 1,
    ROE_COT_ABSTAIN = 2,
    ROE_COT_BUDGET = 3,
    ROE_COT_ERR = -1
} RoeCotStatus;

typedef struct {
    int kind; /* RoeCotKind */
    char label[48];
    char detail[ROE_COT_LINE];
    char pack[ROE_COT_ID];
    char skill[ROE_COT_ID];
    char source[32];
    char answer[ROE_COT_ANS];
    int status; /* RoeCotStatus */
    int tokens; /* always 0 for skeleton; leaf may note teacher */
} RoeCotHop;

typedef struct {
    double dopamine;
    double serotonin;
    double adenosine;
    int pause_grow;
    int control_mode;
    int impulse_mode;
    int da_prefer_local;
    int promotes_day;
    int promotes_cap;
    int teacher_hour;
    int teacher_cap;
} RoeCotBody;

typedef struct {
    char query[ROE_COT_TEXT];
    char root[ROE_COT_PATH]; /* packs root */
    char gov[ROE_COT_PATH];  /* logs/governor */
    int max_hops;            /* default 4 */
    int allow_teacher;       /* 0 = LOCAL/ASK only on ACT */
    int run_act;             /* 1 = invoke front_door */
    RoeCotBody body;
    RoeCotHop hops[ROE_COT_MAX_HOPS];
    int n_hops;
    char final_answer[ROE_COT_ANS];
    char final_source[32];
    char chain_line[ROE_COT_TEXT];
    char summary[640];
    int stopped_early;
    char stop_reason[80];
    /* law */
    int never_self_cert;
    int not_conscious;
    int not_agi;
    int tokens_skeleton; /* always 0 */
} RoeCotChain;

void roe_cot_init(RoeCotChain *C);
void roe_cot_set_paths(RoeCotChain *C, const char *root, const char *gov);
void roe_cot_load_body(RoeCotChain *C); /* best-effort read neuromod/charter files */

/* Build full chain for query. Returns 0 ok. */
int roe_cot_run(RoeCotChain *C, const char *query);

/* Emit GPT-style panel to buf (or stdout if buf NULL via print helper). */
int roe_cot_format_panel(const RoeCotChain *C, char *buf, size_t cap);
void roe_cot_print_panel(const RoeCotChain *C);

/* Persist logs/governor/chain_last.txt + chain_last.json (minimal JSON). */
int roe_cot_persist(const RoeCotChain *C);

/* Selftest — returns 0 if all pass. */
int roe_cot_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_ROE_COT_H */
