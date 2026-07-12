#ifndef CNET_GAP_LANE_H
#define CNET_GAP_LANE_H

/* The gap lane: the 24/7 learning runtime.
 *
 * Everything below composes machinery that already exists and is already
 * gated — the lane adds no new authority. It is the loop that keeps a
 * deployed soul learning:
 *
 *   detect   serving failures become ledger gaps. soul_route no-plans are
 *            appended to a GAP INBOX (one line per miss, O_APPEND) by the
 *            serving processes; the lane ingests the inbox into the
 *            persistent CNET_GAPS ledger. The health pass runs each tick,
 *            and what heal cannot fix (adapters, no verified targets)
 *            becomes a HEALTH gap; entries whose measured reliability sinks
 *            below an opt-in floor become LOW_RELIABILITY gaps.
 *   acquire  acquire_drain per open gap: mine exemplars from a bound oracle
 *            (the local model), train a candidate with DYNAMIC STRUCTURE
 *            GROWTH (btn_init init_hidden -> max_hidden, btn_train_dynamic),
 *            certify PROOF or SAMPLED-behind-Wilson, seal into the CNB base,
 *            admit to the live registry, replan. DEFER stays total.
 *   persist  checkpoint = atomic cnb_save (tmp+rename) + atomic ledger save
 *            (tmp+rename — the lane fixes the ledger's non-atomic save).
 *            The base is the checkpoint; a fresh open resumes from disk and
 *            the serving side picks up new units on its next respawn.
 *
 * Re-note policy: an inbox NO_PLAN is live demand and uses the ledger's
 * native coalesce/reopen semantics. Scan-generated HEALTH/LOW_RELIABILITY
 * notes are state re-observations, not fresh demand — the scan refuses to
 * re-note a subject that already has an OPEN or DEFERRED record, so a
 * deferred rebuild (e.g. incumbent_healthy, oracle_unfit) is not retried
 * every tick.
 *
 * Acceptance gate: make gap_lane (GAP_LANE_PASS). Daemon: bin/gap_lane_run
 * (make gap_lane_run_build), stop file <base>.stop, resumable.
 */

#include <stddef.h>

#include "cnet_export.h"
#include "acquire.h"
#include "base.h"
#include "router.h"
#include "contract/contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    CnetBase base;             /* sealed truth; grows as gaps close */
    PrimitiveRegistry reg;     /* live planner (require_certified) */
    AcquireLedger ledger;      /* persistent gaps */
    OracleRegistry oracles;    /* bind the local model + tools here */
    AcquireConfig acq;         /* training recipe; acq.base wired to &base */
    char base_path[512];
    char ledger_path[512];
    char inbox_path[512];      /* "" = inbox disabled */
    /* Opt-in low-reliability floor (0 = off): entries with at least
       low_rel_min_evidence outcomes and reliability < low_rel_floor are
       noted as LOW_RELIABILITY gaps by gap_lane_scan. */
    double low_rel_floor;
    size_t low_rel_min_evidence;
    /* Run the specialist health pass at the start of every scan (default 1
       from gap_lane_open). Heal fixes what it can; the bridge gaps the rest. */
    int health_pass_enabled;
    /* contract cache for the health pass (materialized from the base) */
    Contract **contracts;
    char (*contract_names)[CNB_NAME_MAX];
    size_t contract_count, contract_cap;
    /* Provenance reconcile queue: ledger indices of CLOSED records whose
       teacher descriptor / unit->descriptor relation are not yet in the
       base. Fed O(1) per closure by the drain's on_close hook; drained on
       every gap_lane_drain. provenance_done marks are PERSISTED (ledger
       v3), so a restart restores reconciliation state instead of
       re-verifying. */
    size_t *prov_pending;
    size_t prov_pending_count, prov_pending_cap;
    /* Set = rebuild the queue with one full ledger scan before processing
       (the open() migration/repair pass; also re-set after a reconcile
       write failure so the next drain retries). Steady state never
       rescans: closures arrive through the queue. */
    int provenance_dirty;
    int loaded;
} GapLane;

/* Per-tick outcome, for logs and gates. */
typedef struct {
    size_t inbox_ingested;     /* inbox lines turned into ledger notes */
    size_t inbox_malformed;    /* inbox lines refused (counted, skipped) */
    size_t health_noted;       /* RESET-after-heal entries noted as HEALTH */
    size_t low_rel_noted;      /* entries noted as LOW_RELIABILITY */
    size_t healed;             /* fixed by the health pass, no gap needed */
    AcquireReport drain;       /* examined / closed / deferred / skipped */
    size_t provenance_reconciled; /* records newly marked done this tick */
    size_t recipe_reopened;    /* recipe-stale deferrals retried this tick */
    int checkpointed;          /* 1 if base+ledger were persisted */
} GapLaneTickReport;

/* Open (or resume) a lane. base_path absent = fresh base; ledger_path
   absent = empty ledger. Registry is rebuilt by certification replay
   (cnb_load_registry) and set require_certified. inbox_path may be NULL
   (inbox disabled). Returns 0, or <0 (nothing to free on failure). */
CNET_API int gap_lane_open(GapLane *L, const char *base_path,
                           const char *ledger_path, const char *inbox_path);

/* Serve one task through the lane: strict certified route when a plan
   exists; otherwise the miss is NOTED in the ledger and, if a bound oracle
   matches the signature, answered by the oracle with the (input, target)
   pair harvested onto the gap. Exactly acquire_execute_or_fallback over
   the lane's state. Returns 0 on an answered task, -1 otherwise. */
CNET_API int gap_lane_execute(GapLane *L, Port input_port, Port goal_port,
                              const double *input, size_t in_len,
                              double *output, size_t out_cap);

/* Detect: ingest the inbox (rename-then-read, so concurrent appenders are
   never truncated), run the health pass (when enabled), then bridge
   unhealed RESET entries to HEALTH gaps and sub-floor reliability to
   LOW_RELIABILITY gaps (per the re-note policy above). Fills the detect
   fields of *r (may be NULL). Returns 0, or <0 on bad args. */
CNET_API int gap_lane_scan(GapLane *L, GapLaneTickReport *r);

/* Acquire: drain every OPEN gap through the bound oracles (mine, train
   with dynamic growth, certify, seal into the base, admit, replan; DEFER
   total), then reconcile CLOSED-gap teacher provenance into the base: the
   teacher descriptor plus the minted unit's DIRECT unit->descriptor
   relation. Persistence failure is returned and remains retryable on the
   next drain. Fills r->drain (r may be NULL). Returns 0, or <0. */
CNET_API int gap_lane_drain(GapLane *L, GapLaneTickReport *r);

/* Persist: atomic cnb_save + atomic ledger save (both tmp+rename).
   Returns 0, or <0. */
CNET_API int gap_lane_checkpoint(GapLane *L);

/* One 24/7 iteration: scan -> drain -> checkpoint (checkpoint only when
   something changed or force_checkpoint). Fills *r (may be NULL).
   Returns 0, or <0. */
CNET_API int gap_lane_tick(GapLane *L, GapLaneTickReport *r,
                           int force_checkpoint);

CNET_API void gap_lane_close(GapLane *L);

/* Parse a token-id file (one non-negative integer per line, blank lines
   skipped): the corpus-drawn WINDOW (CNET_WINDOW_FILE, flagship's
   convention — e.g. english_window_256.txt) and the corpus-drawn teaching
   CONTEXT prefix (CNET_LANE_CONTEXT_FILE) both use it. Returns the number
   of ids read (<= cap), or -1 on a missing/malformed file — a refused
   file binds nothing rather than teaching under a wrong alphabet. */
CNET_API int gap_lane_load_ids(const char *path, int *out, int cap);

/* Streamed SHA-256 over a file's BYTES, truncated to the identity field's
   64 bits: the artifact identity of a teacher model (what the file IS, not
   where it lives — renaming or replacing the artifact changes provenance
   truthfully). Truncation bounds collisions at ~2^32 (birthday); a
   full-width identity needs an oracle-ABI revision and remains open work.
   Multi-GB GGUFs hash once at daemon startup. Returns 0 with *out set
   (never 0), or -1 on an unreadable OR EMPTY file — an unhashable or
   empty artifact binds no identity. */
CNET_API int gap_lane_digest_file(const char *path, unsigned long long *out);

/* Serving-side inbox append (used by soul_route via CNET_GAP_INBOX): one
   O_APPEND line "NO_PLAN <fam> <w> <c> <tag|-> <fam> <w> <c> <tag|->".
   Small single-line appends are atomic on POSIX. Returns 0, or -1. */
CNET_API int gap_inbox_note_no_plan(const char *inbox_path,
                                    Port input_port, Port goal_port);

#ifdef __cplusplus
}
#endif

#endif /* CNET_GAP_LANE_H */
