/* ROE Skill Tree — game-like compounding competence.
 *
 * Flat skills = inventory. Tree = progression:
 *   unlock node (after verify/promote) → grant points + tags
 *   child requires parent ranks
 *   compound power = sum of active node weights * synergy multipliers
 *
 * Shell law unchanged: unlock only via verified events, never self-CERT.
 */
#ifndef CNET_ROE_TREE_H
#define CNET_ROE_TREE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_TREE_NAME 64
#define ROE_TREE_DESC 192
#define ROE_TREE_MAX_NODES 128
#define ROE_TREE_MAX_PREREQ 4
#define ROE_TREE_MAX_TAGS 8
#define ROE_TREE_TAG 32
#define ROE_TREE_PATH 768

typedef enum {
    ROE_TREE_OK = 0,
    ROE_TREE_LOCKED = 1,
    ROE_TREE_ERR = -1
} RoeTreeStatus;

typedef struct {
    char id[ROE_TREE_NAME];
    char title[ROE_TREE_NAME];
    char desc[ROE_TREE_DESC];
    char branch[ROE_TREE_NAME]; /* coding | debug | shell | meta */
    int max_rank;               /* usually 1..5 */
    int rank;                   /* 0 = locked/unlearned */
    int active;
    /* prereqs: other node ids must have rank >= need */
    char pre_id[ROE_TREE_MAX_PREREQ][ROE_TREE_NAME];
    int pre_rank[ROE_TREE_MAX_PREREQ];
    int n_pre;
    /* compound weights */
    double power;      /* base power per rank */
    double synergy;    /* multiplies branch total when ranked */
    char tags[ROE_TREE_MAX_TAGS][ROE_TREE_TAG];
    int n_tags;
    uint64_t unlocks;  /* times rank increased */
    uint64_t tick;
} RoeTreeNode;

typedef struct {
    RoeTreeNode nodes[ROE_TREE_MAX_NODES];
    size_t n_nodes;
    char catalog_dir[ROE_TREE_PATH];
    uint64_t tick;
    uint64_t n_unlock_ok;
    uint64_t n_unlock_blocked;
    uint64_t n_events;
    double peak_power;
} RoeTree;

typedef struct {
    double total_power;
    double coding_power;
    double debug_power;
    double shell_power;
    double meta_power;
    int nodes_unlocked; /* rank>0 */
    int nodes_total;
    int ranks_total;
    char summary[256];
} RoeTreeSnapshot;

void roe_tree_init(RoeTree *T);
void roe_tree_set_catalog(RoeTree *T, const char *dir);

/* Built-in ROE progression tree (coding → debug → project L3 → meta). */
int roe_tree_seed_default(RoeTree *T);

int roe_tree_add_node(RoeTree *T, const char *id, const char *title,
                      const char *branch, const char *desc, int max_rank,
                      double power, double synergy);

int roe_tree_add_prereq(RoeTree *T, const char *id, const char *pre_id,
                        int pre_rank_need);
int roe_tree_add_tag(RoeTree *T, const char *id, const char *tag);

/* Can this node gain a rank? */
int roe_tree_can_unlock(const RoeTree *T, const char *id);

/* Verified event → try rank++ (shell). Returns new rank or -1 blocked/err. */
int roe_tree_unlock(RoeTree *T, const char *id, const char *evidence_note);

/* Map domain events into tree unlocks (compound progression). */
int roe_tree_on_coding_skill(RoeTree *T, const char *skill_pattern);
int roe_tree_on_debug_l1(RoeTree *T, const char *error_name);
int roe_tree_on_debug_l3(RoeTree *T, const char *project_id);
int roe_tree_on_verify(RoeTree *T);

void roe_tree_snapshot(const RoeTree *T, RoeTreeSnapshot *S);
const RoeTreeNode *roe_tree_get(const RoeTree *T, const char *id);

int roe_tree_save(const RoeTree *T);
int roe_tree_load(RoeTree *T);

void roe_tree_dump(const RoeTree *T, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
