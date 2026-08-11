/* ROE-ASI — Regulated Open-Ended Artificial Specialized Intelligence (concept).
 *
 * Local CERT-like skills first. On miss: LOOKUP → LLM teacher → ASK_USER.
 * LLM/lookup output is UNTRUSTED until shell verify + promote.
 * Never self-CERT from a single unverified model reply. Floors not lowered.
 */
#ifndef CNET_ROE_ASI_H
#define CNET_ROE_ASI_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_asi_improve.h"
#include "cnet_roe_net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_NAME_MAX 64
#define ROE_TEXT_MAX 256
#define ROE_ANSWER_MAX 4096
#define ROE_MAX_SKILLS 128
#define ROE_MAX_LOOKUP 64
#define ROE_MAX_TEACH 64
#define ROE_MAX_LOG 256
#define ROE_PATH_MAX 768

typedef enum {
    ROE_SRC_LOCAL = 1,
    ROE_SRC_LOOKUP = 2,
    ROE_SRC_LLM = 3,
    ROE_SRC_ABSTAIN = 4,
    ROE_SRC_ASK_USER = 5
} RoeSource;

typedef enum {
    ROE_OK = 0,
    ROE_ABSTAIN = 1,
    ROE_ERR = -1
} RoeStatus;

typedef struct {
    char id[ROE_NAME_MAX];
    char intent_key[ROE_NAME_MAX]; /* matched intent bucket */
    char pattern[ROE_TEXT_MAX];    /* substring trigger */
    char answer[ROE_ANSWER_MAX];
    uint32_t privilege;            /* 0 = lowest */
    int certified;                 /* 1 = shell-admitted local skill */
    int active;
    uint64_t hits;
    uint64_t promotes_from;        /* teach id provenance */
} RoeSkill;

typedef struct {
    char pattern[ROE_TEXT_MAX];
    char snippet[ROE_ANSWER_MAX];
    int active;
} RoeLookupEntry;

/* Fixed curriculum for mock LLM teacher (external teacher stand-in). */
typedef struct {
    char pattern[ROE_TEXT_MAX];
    char intent_key[ROE_NAME_MAX];
    char answer[ROE_ANSWER_MAX];
    int active;
} RoeTeachEntry;

typedef struct {
    char query[ROE_TEXT_MAX];
    char answer[ROE_ANSWER_MAX];
    int source; /* RoeSource */
    int ok;
    int verified; /* shell verified against teacher gold or local CERT */
    uint64_t tokens_est;
    uint64_t tick;
} RoeTurnLog;

typedef struct {
    RoeSkill skills[ROE_MAX_SKILLS];
    size_t n_skills;
    RoeLookupEntry lookup[ROE_MAX_LOOKUP];
    size_t n_lookup;
    RoeTeachEntry teach[ROE_MAX_TEACH];
    size_t n_teach;

    CnetAsiLib gate; /* privilege/exec/conformal catalog mirror */
    int use_gate;

    RoeNet *net; /* optional live backends; not owned */
    char catalog_dir[ROE_PATH_MAX];

    RoeTurnLog log[ROE_MAX_LOG];
    size_t log_i, log_n;

    /* pending promote candidates from miss path (not CERT yet) */
    struct {
        char intent_key[ROE_NAME_MAX];
        char pattern[ROE_TEXT_MAX];
        char answer[ROE_ANSWER_MAX];
        int votes; /* verified successes */
        int active;
    } pending[ROE_MAX_SKILLS];
    size_t n_pending;
    int promote_votes_needed; /* default 2 */

    uint64_t tick;
    /* economics */
    uint64_t n_turns;
    uint64_t n_local_hit;
    uint64_t n_lookup_hit;
    uint64_t n_llm_call;
    uint64_t n_abstain;
    uint64_t n_ask_user;
    uint64_t n_promote;
    uint64_t n_verify_fail;
    uint64_t tokens_local;   /* ~0 */
    uint64_t tokens_lookup;  /* small */
    uint64_t tokens_llm;     /* large */
    uint64_t tokens_baseline_llm; /* if every turn was LLM */
    uint64_t n_live_lookup;
    uint64_t n_live_llm;
} RoeAsi;

typedef struct {
    char answer[ROE_ANSWER_MAX];
    int source;
    int verified;
    uint64_t tokens_est;
    char skill_id[ROE_NAME_MAX];
    /* Self-model inventory (filled by roe_turn / roe_reply_fill_inventory). */
    char source_name[16];      /* LOCAL|LOOKUP|LLM|ABSTAIN|ASK_USER|... */
    char inventory_line[384];  /* compact economics + skill */
} RoeReply;

/* Human-readable RoeSource name. */
const char *roe_source_name(int source);

/* Fill source_name + inventory_line from current RoeAsi economics. */
void roe_reply_fill_inventory(const RoeAsi *R, RoeReply *out);

void roe_init(RoeAsi *R);
void roe_reset_stats(RoeAsi *R);

/* Seed / train data */
int roe_add_skill(RoeAsi *R, const char *id, const char *intent_key,
                  const char *pattern, const char *answer, uint32_t privilege,
                  int certified);
int roe_add_lookup(RoeAsi *R, const char *pattern, const char *snippet);
int roe_add_teach(RoeAsi *R, const char *pattern, const char *intent_key,
                  const char *answer);

/* Core turn: shell-regulated assist */
int roe_turn(RoeAsi *R, const char *query, RoeReply *out);

/* After a turn, if source was LOOKUP/LLM and gold known, verify + maybe promote.
 * gold_answer NULL → try match teach table. Returns 1 if promoted. */
int roe_feedback_verify(RoeAsi *R, const char *query, const char *gold_answer,
                        int user_accept);

/* One train epoch over queries[]; uses teach table as external teacher gold. */
typedef struct {
    int turns;
    int promotes;
    double local_hit_rate;
    double token_save_ratio; /* 1 - tokens_used/tokens_baseline */
    uint64_t tokens_used;
    uint64_t tokens_baseline;
} RoeTrainReport;

int roe_train_epoch(RoeAsi *R, const char **queries, int nq, RoeTrainReport *rep);

void roe_dump_stats(const RoeAsi *R, char *buf, size_t cap);

/* Live backends (borrowed pointer; caller owns RoeNet). */
void roe_set_net(RoeAsi *R, RoeNet *net);

/* Persist certified skills as ROE packs (text capsules) under dir. */
int roe_set_catalog_dir(RoeAsi *R, const char *dir);
int roe_save_catalog(const RoeAsi *R);
int roe_load_catalog(RoeAsi *R);
/* Export one skill pack dir: dir/skills/<id>/{SKILL.roe,manifest.roe} */
int roe_export_skill_pack(const RoeAsi *R, const char *skill_id, const char *out_dir);

#ifdef __cplusplus
}
#endif

#endif
