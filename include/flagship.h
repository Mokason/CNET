#ifndef CNET_FLAGSHIP_H
#define CNET_FLAGSHIP_H

/* Thermal-governed flagship harness: the duty-cycled, crash-resumable,
 * hours-long compounding run. Extracts CONDITIONAL NEXT-TOKEN slices from a
 * model-backed oracle: for each conditioning token t in a closed vocab V, one
 * unit  qnext_after_<t> : ONEHOT|V| "w_cur" -> ONEHOT|V| "wa<t>q<t>"
 * computing argmax_{v in V} P(v | [t, w]) — a genuinely distinct function per
 * t, mined exhaustively (|V| oracle calls each), certified (PROOF on the
 * enumerated domain), sealed into the unified base.
 *
 * The BASE IS THE CHECKPOINT: resume = skip units already present; the gap
 * ledger sidecar carries DEFERRED knowledge across restarts (a re-noted
 * DEFERRED gap reopens). Everything the hardware budget demands is here:
 * duty-cycled work pulses, GPU-temperature pause/resume (amdgpu sysfs on
 * discrete cards, else nvidia-smi; absent -> duty cycle only), BELOW_NORMAL
 * process priority, a wall-clock budget,
 * and a stop file ("<base_path>.stop") for clean user interruption.
 *
 * Tag note ("wa<t>q<t>", token id doubled): systematic tag families collide
 * with the base's Damerau-1 near-miss guard — "wa100" vs "wa101" IS a typo
 * distance. Doubling the id guarantees pairwise distance >= 2, so intentional
 * families pass while real typos still refuse. (A family-minting concept is
 * the honest v2 fix.)
 *
 * The oracle is INJECTED (FlagshipOracleMaker): tests use a synthetic
 * deterministic "model"; the real CLI (tests/flagship_run.c) binds a
 * CCE-loaded transformer. The harness itself never touches CCE. */

#include <stdio.h>

#include "base.h"
#include "acquire.h"

/* Task shapes. ARGMAX = campaign 1 (1-field in, argmax out, PROOF-eligible).
   PAIR = 2-field conditioning (domain V^2 >> mine budget -> the SAMPLED tier
   runs for real; tags "w_pair" -> "pr<t>q<t>"). TOPK = ranked preference,
   "the soul": ordered top-k as k ONEHOT fields (tags "w_cur" -> "tk<t>q<t>";
   enumerable, PROOF-eligible, thin ranking margins). Family prefixes are
   Damerau-spaced >= 2 from each other AND from campaign 1's "wa". */
typedef enum {
    FLAGSHIP_TASK_ARGMAX = 0,
    FLAGSHIP_TASK_PAIR = 1,
    FLAGSHIP_TASK_TOPK = 2
} FlagshipTask;

typedef struct {
    CnetOracleFn fn;
    void *ctx;
    /* 0/1 = serial. >1 = fn is safe for this many concurrent callers (it
       dispatches per-thread state, e.g. one model instance per GPU); the
       harness passes it through to acquire_oracle_set_parallel. */
    size_t width;
    CnetOracleBatchFn fn_batch;  /* optional batched probe (see acquire.h) */
    size_t batch_hint;
} FlagshipOracle;

/* Prepare the oracle for conditioning token vocab[k] (= token_id). Returns 0
   with *out filled (ctx must stay valid until the next maker call), -1 when
   no oracle is available for this token (counted, skipped). */
typedef int (*FlagshipOracleMaker)(void *maker_ctx, size_t k, int token_id,
                                   FlagshipOracle *out);

typedef struct {
    /* task scope */
    const int *vocab_tokens;   /* closed token set V (ids into the model) */
    size_t vocab_size;         /* |V| = ONEHOT width; must be >= acq.min_evidence */
    size_t max_units;          /* conditioning tokens attempted this run (0 = all) */
    /* persistence (the base is the checkpoint) */
    const char *base_path;     /* loaded when present -> resume */
    const char *ledger_path;   /* gap ledger sidecar (loaded when present) */
    size_t checkpoint_every;   /* save base+ledger every N attempts (default 4) */
    /* thermal governor */
    int gpu_temp_limit_c;      /* pause above this; 0 = no temp reads (default 80) */
    double duty_fraction;      /* work share of wall time, (0,1] (default 0.75) */
    unsigned cooldown_ms;      /* sleep quantum while cooling (default 5000) */
    double max_wall_seconds;   /* clean stop after this (0 = unbounded) */
    int below_normal_priority; /* default 1: keep the PC usable */
    /* task shape */
    FlagshipTask task;         /* default ARGMAX */
    size_t topk;               /* TOPK rank depth (default 3) */
    /* conformal probe (PAIR units that certify SAMPLED; report-only) */
    double conformal_alpha;    /* target miscoverage (default 0.05; 0 = off) */
    size_t conformal_n;        /* calib set size == test set size (default 256) */
    /* acquisition knobs (base pointer is set internally to the run's base) */
    AcquireConfig acq;
} FlagshipConfig;

void flagship_config_defaults(FlagshipConfig *cfg);

#define FLAGSHIP_MAX_REASONS 16

typedef struct {
    size_t attempted;        /* acquire_now calls made this run */
    size_t acquired;         /* gaps CLOSED (unit certified + sealed into base) */
    size_t deferred;         /* gaps DEFERRED */
    size_t skipped_resume;   /* units already in the base (prior runs) */
    size_t no_oracle;        /* maker had no oracle for the token */
    size_t registry_skipped; /* base units that failed certify-on-load */
    /* defer tally by reason atom */
    char reasons[FLAGSHIP_MAX_REASONS][ACQUIRE_REASON_MAX];
    size_t reason_counts[FLAGSHIP_MAX_REASONS];
    size_t reason_kinds;
    /* tier table */
    size_t proof_count;      /* acquired with CERT_PROVEN */
    size_t sampled_count;    /* acquired with CERT_SAMPLED (Wilson-gated) */
    double bounds[1024];     /* Wilson floors of the SAMPLED units */
    size_t bound_count;
    double margins[1024];    /* certified min-margins of ALL acquired units */
    size_t margin_count;
    /* conformal probe aggregate (PAIR + SAMPLED units; report-only) */
    size_t conf_units;       /* units probed */
    size_t conf_answered;    /* test queries answered (singleton set) */
    size_t conf_abstained;   /* test queries abstained */
    size_t conf_wrong;       /* answered AND wrong (empirical risk numerator) */
    /* governor telemetry */
    double wall_seconds;
    double slept_seconds;    /* duty-cycle + cooling sleep, total */
    int max_gpu_temp_seen;   /* -1 = never read */
    int stopped;             /* 0 = swept all tokens, 1 = wall budget, 2 = stop file */
} FlagshipReport;

/* Run the harness. Loads base+ledger when the files exist (resume), lowers
   process priority, sweeps conditioning tokens through acquire_now with the
   governor between attempts, checkpoints periodically and at the end.
   Returns 0 (the report carries the outcome), -1 on bad args / I/O that
   prevents even starting. */
int flagship_run(FlagshipConfig *cfg, FlagshipOracleMaker maker,
                 void *maker_ctx, FlagshipReport *rep);

void flagship_print_report(const FlagshipReport *rep, FILE *out);

#endif /* CNET_FLAGSHIP_H */
