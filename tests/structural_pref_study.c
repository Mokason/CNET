/*
 * structural_pref_study.c -- the budgeted Structural Preference Sidecar
 * (Derivation Lock, SHADOW_ONLY) sweep study.
 * (spec: docs/superpowers/specs/2026-06-19-structural-preference-sidecar-design.md)
 *
 * For each fixture (attractor, lure) it runs the full perturbation grid
 *   K candidate-order permutations x beam in {1,2,4,8,0} x memo {off,on}
 * re-deriving the SAME single-root dag_plan task per cell and recording whether
 * the SAME canonical plan digest reappears. It reports, per fixture:
 *   baseline_digest, K (cells that planned), R (cells matching baseline),
 *   D (distinct digests), derivation_lock_residual = 1 - R/K, and the boring
 *   score = reproduced_fraction - distinct_valid_penalty.
 *
 * The no-authority flags are constants (this is SHADOW_ONLY by construction --
 * the sidecar plans only against permuted COPIES of the entry array; it never
 * touches the validator, planner, registry, or reliability counters):
 *   authority = 0, planner_influence = 0, registry_mutation = 0,
 *   reliability_mutation = 0, validity_gate_unchanged = 1.
 * laws_checked = laws_held = 0 for these synthetic fixtures (laws/compression
 * deferred); the compression term is omitted.
 *
 * Output: a printed per-cell trace + per-fixture summary table, plus a CSV
 * (one row per perturbation cell) under artifacts/structural_pref/. The mkdir
 * is best-effort (_mkdir on _WIN32), matching belowbeam_chars_study.c /
 * circuit_demo.c; an fopen failure is non-fatal (table only).
 *
 * Budgeted; NOT part of make test.
 */
#include "structural_pref_common.h"

#include <stdio.h>
#ifdef _WIN32
#include <direct.h>  /* for _mkdir on MinGW/Windows (matches circuit_demo.c) */
#endif

/* The no-authority constants advertised for every fixture (SHADOW_ONLY). */
typedef struct {
    int valid;
    int advisory_only;
    int authority;
    int planner_influence;
    int prune_influence;
    int certification_influence;
    int registry_mutation;
    int reliability_mutation;
    int contract_mutation;
    int weight_mutation;
    int validity_gate_unchanged;
    int laws_checked;
    int laws_held;
} NoAuthority;

static NoAuthority no_authority_flags(void) {
    NoAuthority f;
    f.valid = 1;
    f.advisory_only = 1;
    f.authority = 0;
    f.planner_influence = 0;
    f.prune_influence = 0;
    f.certification_influence = 0;
    f.registry_mutation = 0;
    f.reliability_mutation = 0;
    f.contract_mutation = 0;
    f.weight_mutation = 0;
    f.validity_gate_unchanged = 1;
    f.laws_checked = 0;
    f.laws_held = 0;
    return f;
}

/* Append the per-cell CSV rows for one fixture (NON-FATAL: csv may be NULL). */
static void csv_cells(FILE *csv, const char *fixture, const SpcSweep *s) {
    size_t i;
    if (csv == NULL) {
        return;
    }
    for (i = 0; i < s->cell_count; ++i) {
        const SpcCell *c = &s->cells[i];
        fprintf(csv, "%s,%zu,%zu,%d,%d,%016llx,%d\n",
                fixture, c->perm_idx, c->beam, c->memo_on, c->planned,
                (unsigned long long)c->digest, c->matches_baseline);
    }
}

/* Print the per-cell trace for one fixture. */
static void print_cells(const char *fixture, const SpcSweep *s) {
    size_t i;
    printf("\n  %s -- per-cell trace (perm x beam x memo):\n", fixture);
    printf("    %-5s %-5s %-5s %-8s %-18s %s\n",
           "perm", "beam", "memo", "planned", "digest", "match");
    for (i = 0; i < s->cell_count; ++i) {
        const SpcCell *c = &s->cells[i];
        printf("    %-5zu %-5zu %-5d %-8d %016llx %d\n",
               c->perm_idx, c->beam, c->memo_on, c->planned,
               (unsigned long long)c->digest, c->matches_baseline);
    }
}

/* Print the no-authority block. */
static void print_no_authority(const NoAuthority *f) {
    printf("    no-authority: valid=%d advisory_only=%d authority=%d "
           "planner_influence=%d prune_influence=%d\n",
           f->valid, f->advisory_only, f->authority,
           f->planner_influence, f->prune_influence);
    printf("                  certification_influence=%d registry_mutation=%d "
           "reliability_mutation=%d contract_mutation=%d weight_mutation=%d\n",
           f->certification_influence, f->registry_mutation,
           f->reliability_mutation, f->contract_mutation, f->weight_mutation);
    printf("                  validity_gate_unchanged=%d laws_checked=%d "
           "laws_held=%d\n",
           f->validity_gate_unchanged, f->laws_checked, f->laws_held);
}

/* The summary-table row for one fixture. */
typedef struct {
    const char *name;
    uint64_t baseline_digest;
    size_t K, R, D;
    double residual;
    double score;
} SummaryRow;

static void print_summary_header(void) {
    printf("\n=== Summary table ===\n");
    printf("  %-10s %-18s %-4s %-4s %-4s %-10s %-8s\n",
           "fixture", "baseline_digest", "K", "R", "D", "residual", "score");
}

static void print_summary_row(const SummaryRow *r) {
    printf("  %-10s %016llx %-4zu %-4zu %-4zu %-10.4f %-8.4f\n",
           r->name, (unsigned long long)r->baseline_digest,
           r->K, r->R, r->D, r->residual, r->score);
}

/* Run + report one fixture; fill the summary row. Returns 0 on success. */
static int run_fixture(const char *name, SpcFixture *f, FILE *csv,
                       SummaryRow *row) {
    SpcSweep s;
    NoAuthority na = no_authority_flags();

    if (spc_run_sweep(&f->reg, f->sources, 1, f->goal, &s) != 0) {
        fprintf(stderr, "sweep failed for fixture %s\n", name);
        return -1;
    }

    printf("\n--- Fixture: %s ---\n", name);
    printf("    baseline_digest=%016llx baseline_valid=%d\n",
           (unsigned long long)s.baseline_digest, s.baseline_planned);
    printf("    perturbations_tried(K)=%zu reproduced_structure(R)=%zu "
           "distinct_valid_seen(D)=%zu invalid_or_no_plan(X)=%zu\n",
           s.K, s.R, s.D, s.X);
    printf("    derivation_lock_residual = 1 - R/K = %.4f\n",
           s.derivation_lock_residual);
    printf("    structural_preference_score = %.4f\n", spc_score(&s));
    print_no_authority(&na);
    print_cells(name, &s);
    csv_cells(csv, name, &s);

    /* Step A: the ORDER_ONLY ranking (advisory; the planner is untouched). */
    {
        SpcRanked r;
        if (structural_pref_rank(&f->reg, f->sources, 1, f->goal, &r) == 0) {
            size_t i;
            printf("\n    RECOMMENDATION ranking (most-attractor-like first; "
                   "SHADOW_PLUS_RECOMMEND, planner_influence=0):\n");
            printf("    %-4s %-18s %-6s %-22s %s\n",
                   "rank", "digest", "count", "recipe", "default?");
            for (i = 0; i < r.count; ++i) {
                const SpcRankedEntry *e = &r.entries[i];
                printf("    %-4zu %016llx %-6zu perm=%zu beam=%zu memo=%d  %s\n",
                       i, (unsigned long long)e->digest, e->count,
                       e->perm_idx, e->beam, e->memo_on,
                       (e->digest == r.baseline_digest) ? "<- planner default" : "");
            }
            printf("    recommended rank-0 digest = %016llx "
                   "(planner default sits at rank %d)\n",
                   (unsigned long long)r.entries[0].digest, r.baseline_rank);
        }
    }

    row->name = name;
    row->baseline_digest = s.baseline_digest;
    row->K = s.K;
    row->R = s.R;
    row->D = s.D;
    row->residual = s.derivation_lock_residual;
    row->score = spc_score(&s);
    return 0;
}

int main(void) {
    SpcFixture attractor, lure;
    SummaryRow rows[2];
    size_t n_rows = 0;
    FILE *csv;
    int wrote_csv = 0;

    printf("=== Structural Preference Sidecar -- Derivation Lock (SHADOW_ONLY) ===\n");
    printf("Among already-valid single-root structures: re-derive the same "
           "dag_plan task under\n");
    printf("candidate-order permutations x beam {1,2,4,8,0} x memo {off,on}; "
           "count how often the\n");
    printf("SAME canonical plan digest reappears. residual = 1 - R/K "
           "(0 = attractor, ->1 = fragile).\n");
    printf("Grid: %u perms x %zu beams x %u memo = %u cells per fixture.\n",
           SPC_PERM_COUNT, (size_t)SPC_BEAM_COUNT, SPC_MEMO_COUNT,
           (unsigned)SPC_CELL_COUNT);

    /* CSV is best-effort (matches belowbeam_chars_study.c / circuit_demo.c):
       mkdir failures are ignored (the dir may already exist); only an fopen
       failure suppresses the CSV. */
#ifdef _WIN32
    (void)_mkdir("artifacts");
    (void)_mkdir("artifacts/structural_pref");
#else
    (void)system("mkdir -p artifacts/structural_pref 2>/dev/null || true");
#endif
    csv = fopen("artifacts/structural_pref/structural_pref.csv", "w");
    if (csv == NULL) {
        printf("(note: could not open artifacts/structural_pref/"
               "structural_pref.csv for writing; continuing, table only)\n");
    } else {
        fprintf(csv, "fixture,perm_idx,beam,memo,planned,digest_hex,"
                     "matches_baseline\n");
        wrote_csv = 1;
    }

    if (spc_make_attractor(&attractor) != 0) {
        fprintf(stderr, "attractor fixture build failed\n");
    } else {
        if (run_fixture("attractor", &attractor, csv, &rows[n_rows]) == 0) {
            ++n_rows;
        }
        spc_fixture_free(&attractor);
    }

    if (spc_make_lure(&lure) != 0) {
        fprintf(stderr, "lure fixture build failed\n");
    } else {
        if (run_fixture("lure", &lure, csv, &rows[n_rows]) == 0) {
            ++n_rows;
        }
        spc_fixture_free(&lure);
    }

    print_summary_header();
    {
        size_t i;
        for (i = 0; i < n_rows; ++i) {
            print_summary_row(&rows[i]);
        }
    }

    if (csv != NULL) {
        fclose(csv);
    }
    if (wrote_csv) {
        printf("\nWrote artifacts/structural_pref/structural_pref.csv\n");
    }

    printf("\nHonest scope: this measures STRUCTURAL PREFERENCE -- whether a "
           "valid structure is\nstable/reproducible (attractor-like) under "
           "bounded re-derivation -- NOT human taste.\n");
    printf("Step A: rank-0 is the recommended canonical structure -- ADVISORY "
           "only; the planner is\nunchanged (planner_influence=0). Consuming the "
           "order IN the planner is Step B (frozen artifact).\n");
    return 0;
}
