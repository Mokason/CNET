/* ROE self-model — instrumented inventory, not consciousness.
 *
 * Closes loops:
 *   1) per-turn source + skill inventory line
 *   2) skill-tree ranks bound to gate evidence files (never self-CERT)
 *   3) coverage + pack export of who-I-am-at-this-build
 *   4) lightweight 5-layer skill health (ROE inventory, not full registry)
 *   5) goal HAVE/MISS planner self-check
 *   6) optional agent thought reflection hook
 *
 * Law: never self-CERT; ranks/claims only from verified files or shell admit.
 */
#ifndef CNET_ROE_SELF_H
#define CNET_ROE_SELF_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_roe_asi.h"
#include "cnet_roe_doc.h"
#include "cnet_roe_goal.h"
#include "cnet_roe_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_SELF_MAX_SKILLS 128
#define ROE_SELF_MAX_NODES 128
#define ROE_SELF_LINE 384
#define ROE_SELF_PATH 1024
#define ROE_SELF_SUMMARY 512
#define ROE_SELF_ABI_MAGIC "ROE_SELF_MODEL"
#define ROE_SELF_ABI_VER 1

typedef enum {
    ROE_HL_PASS = 0,
    ROE_HL_FAIL = 1,
    ROE_HL_SKIP = 2
} RoeHealthVerdict;

typedef struct {
    char skill_id[ROE_NAME_MAX];
    int certified;
    int active;
    uint64_t hits;
    RoeHealthVerdict layer[5]; /* REGISTRY LOADABLE EXECUTION SEMANTIC UTILITY */
    char reason[5][24];
    int deepest_pass;      /* -1 none */
    int production_ready;  /* all five PASS */
} RoeSkillHealth;

typedef struct {
    char node_id[ROE_TREE_NAME];
    int rank;
    int max_rank;
    char evidence[96]; /* gate marker or "none" */
} RoeTreeEvidenceRow;

typedef struct {
    /* economics / inventory from RoeAsi */
    uint64_t n_turns;
    uint64_t n_local_hit;
    uint64_t n_lookup_hit;
    uint64_t n_llm_call;
    uint64_t n_abstain;
    uint64_t n_ask_user;
    uint64_t n_promote;
    uint64_t n_verify_fail;
    uint64_t tokens_used;
    uint64_t tokens_baseline;
    double local_hit_rate;
    double token_save_ratio;
    size_t n_skills_cert;
    size_t n_skills_active;
    size_t n_pending;

    /* doc coverage (optional; zeros if no doc) */
    int has_doc;
    RoeDocCoverage coverage;

    /* goal probe (optional) */
    int has_goal;
    int goal_have;
    int goal_miss;
    int goal_learned;
    int goal_blocked;
    char goal_text[ROE_GOAL_TEXT];

    /* tree */
    int has_tree;
    RoeTreeSnapshot tree_snap;
    RoeTreeEvidenceRow tree_rows[ROE_SELF_MAX_NODES];
    size_t n_tree_rows;

    /* skill health */
    RoeSkillHealth health[ROE_SELF_MAX_SKILLS];
    size_t n_health;
    size_t n_production_ready;

    /* doctrine flags (always true claims we encode) */
    int never_self_cert; /* 1 */
    int second_brain;    /* 0 */
    char law_line[160];

    char summary[ROE_SELF_SUMMARY];
    char inventory_line[ROE_SELF_LINE]; /* one-line for turn echo */
} RoeSelfReport;

typedef struct {
    RoeAsi *roe;           /* required borrowed */
    RoeDocAsset *doc;      /* optional */
    RoeGoalEngine *goal;   /* optional */
    RoeTree *tree;         /* optional; if NULL, seed OCR tree into owned */
    RoeTree owned_tree;
    int own_tree;
    char out_dir[ROE_SELF_PATH];
    char repo_root[ROE_SELF_PATH]; /* for gate evidence probes; "" = cwd */
    int record_thoughts;   /* 1 → agent_memory thought if available */
} RoeSelfModel;

/* Source name helper (LOCAL/LOOKUP/LLM/ABSTAIN/ASK_USER/UNK). */
const char *roe_source_name(int source);

/* Fill RoeReply inventory_line + source_name after a turn (or manually). */
void roe_reply_fill_inventory(const RoeAsi *R, RoeReply *out);

void roe_self_init(RoeSelfModel *M);
void roe_self_bind_roe(RoeSelfModel *M, RoeAsi *R);
void roe_self_bind_doc(RoeSelfModel *M, RoeDocAsset *D);
void roe_self_bind_goal(RoeSelfModel *M, RoeGoalEngine *G);
void roe_self_bind_tree(RoeSelfModel *M, RoeTree *T); /* borrow */
void roe_self_set_out_dir(RoeSelfModel *M, const char *dir);
void roe_self_set_repo_root(RoeSelfModel *M, const char *root);
void roe_self_set_record_thoughts(RoeSelfModel *M, int on);

/* Seed default OCR surpass tree into owned tree (if no tree bound). */
int roe_self_ensure_ocr_tree(RoeSelfModel *M);

/* Bind ranks from on-disk gate evidence under repo_root (never invent CERT). */
int roe_self_apply_gate_evidence(RoeSelfModel *M);

/* Lightweight 5-layer health over RoeAsi skills. */
int roe_self_health_scan(const RoeAsi *R, RoeSkillHealth *out, size_t cap,
                         size_t *n_out, size_t *n_ready);

/* Full snapshot into report. */
int roe_self_snapshot(RoeSelfModel *M, RoeSelfReport *rep);

/* Optional goal planner self-check (no auto_learn). */
int roe_self_goal_probe(RoeSelfModel *M, const char *goal, RoeSelfReport *rep);

/* Persist pack: SELF.abi + self_report.json + MANIFEST + optional tree. */
int roe_self_export_pack(const RoeSelfModel *M, const RoeSelfReport *rep,
                         const char *out_dir);
int roe_self_pack_validate(const char *pack_dir);

/* Human dump. */
void roe_self_dump(const RoeSelfReport *rep, char *buf, size_t cap);

/* Optional: record a reflection thought (agent_memory). Returns 0 ok, -1 skip. */
int roe_self_record_thought(const RoeSelfModel *M, const RoeSelfReport *rep);

#ifdef __cplusplus
}
#endif

#endif /* CNET_ROE_SELF_H */
