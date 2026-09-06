#ifndef CNET_CORE_ALLOCATOR_H
#define CNET_CORE_ALLOCATOR_H
#include "cnet_core_cell.h"
#include <stddef.h>
#include <stdint.h>
#define CNET_ALLOCATOR_FEATURE_VERSION 1
#define CNET_ALLOCATOR_MAX_TASKS 32
#define CNET_ALLOCATOR_MAX_ROWS 2048
#define CNET_ALLOCATOR_MAX_EPISODES 256
#define CNET_ALLOCATOR_FAMILIES 4
enum { CNET_ALLOCATOR_LEARNED=0, CNET_ALLOCATOR_CURSOR=1,
       CNET_ALLOCATOR_DEMAND_COST=2, CNET_ALLOCATOR_COMPLETION=3 };
/* Pre-decision inputs ONLY. Features in [0,1]: observed unmet demand touched
 * by the approved plan; already-certified fraction of plan dependencies;
 * missing evidence rows / 2048. Outcomes never enter this record. Cost is
 * exact prospective tool-work units, age is completed scheduling ticks. */
typedef struct { uint64_t id; float features[3]; unsigned cost,age; } CnetAllocatorTask;
typedef struct { unsigned count,work,index[8]; } CnetAllocatorChoice;
/* Tasks are in the incumbent's directory/queue enumeration order. cursor is
 * its next index. All policies first service overdue tasks (age>=wait_limit),
 * oldest first, then apply their ranking. Equal scores use cursor order.
 * A task must fit the entire work budget; no partial job is selected. */
int cnet_core_allocator_choose(const CnetCoreCell *cell,const CnetAllocatorTask *tasks,
    size_t n,unsigned cursor,unsigned jobs,unsigned work,unsigned wait_limit,
    unsigned policy,CnetAllocatorChoice *out);
/* Trusted evaluator records, never accepted by choose. Each mask records
 * independently tool-verified newly answered probes after executing that task
 * alone against the SAME incumbent. The evaluator must establish independence
 * before taking a union: no unrecorded cross-task synergy or interference.
 * Families: direct, shared-prefix, chain-completion, evidence-rejection. */
typedef struct {
    uint64_t id;
    unsigned family,n,cursor,population,jobs,work,wait_limit;
    CnetAllocatorTask task[CNET_ALLOCATOR_MAX_TASKS];
    uint64_t coverage[CNET_ALLOCATOR_MAX_TASKS];
    char receipt[CNET_ALLOCATOR_MAX_TASKS][65];
} CnetAllocatorEpisode;
typedef struct {
    size_t episodes,family_episodes[CNET_ALLOCATOR_FAMILIES];
    unsigned comparators;
    double coverage[5],gain[4],paired95_lower[4];
    double family_gain[CNET_ALLOCATOR_FAMILIES][4];
    int passed;
} CnetAllocatorGateReport;
int cnet_core_allocator_episode_validate(const CnetAllocatorEpisode *episode);
/* Fixed prospective gate: >=32 independent episodes, >=4 in EACH of the four
 * families; mean gain >=.05 AND paired 95% lower >0 against all three policies;
 * no per-family mean regression. Uses conservative t31=2.040 for n>=32.
 * Independence, receipt authenticity and fresh-split custody are owner duties;
 * hashes provide integrity only. No serving/capsule gate is relaxed here. */
int cnet_core_allocator_evaluate(const CnetCoreCell *cell,
    const CnetAllocatorEpisode *episodes,size_t n,CnetAllocatorGateReport *out);
/* On later promotions active is REQUIRED by the durable owner. In addition
 * to all original controls, require the same >=.05/positive paired95 gain and
 * family non-regression against the actual active cell (fourth comparator).
 * NULL denotes first promotion with the rotating deterministic incumbent. */
int cnet_core_allocator_evaluate_against(const CnetCoreCell *cell,const CnetCoreCell *active,
    const CnetAllocatorEpisode *episodes,size_t n,CnetAllocatorGateReport *out);
#endif
