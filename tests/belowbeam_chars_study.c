/*
 * belowbeam_chars_study.c -- the budgeted Below-Beam Failure-Mode
 * Characterization SWEEP study (spec: docs/superpowers/specs/
 * 2026-06-19-belowbeam-failure-mode-characterization-design.md).
 *
 * v5.1's probe (proposal_sidecar_below_beam_probe) DETECTS a correct producer
 * ranked below the planner's beam. This study characterizes the SHAPE of that
 * blind spot to inform a later "v5.2": it builds blind spots from the TWO causes,
 * sweeps each cause's knob, reuses the probe VERBATIM, and reports a
 * deployment-independent reliability-margin boundary + the rank-gap distribution.
 *
 *   Cause 1 -- cold-start (under-ranked-correct): M=8 competitors FIXED with a
 *     spread of reliabilities; sweep delta's evidence n in {0,1,2,4,8,16,32,64,
 *     128}. As n rises delta's reliability climbs and its rank decreases (moves
 *     above competitors). Boundary = the n at which found flips 1 -> 0.
 *
 *   Cause 2 -- rank-poisoning (over-ranked-wrong): delta FIXED at moderate
 *     reliability (s=8 -> rel ~0.9); sweep the number m of injected-high
 *     competitors (rel ~0.99) in {0,1,2,3,4,6,8}. As m grows delta's rank rises
 *     past the beam. Boundary = the m at which found flips 0 -> 1.
 *
 * Beam size is the second axis: k in {2,4,8}. Per swept point we record
 * found_valid_below_beam, recovered_producer_rank, rank_gap = rank - k, and the
 * reliability margin (reconstructed by replicating rank_by_reliability). Output:
 * a printed table + per-cause summary, plus a CSV under artifacts/belowbeam_chars/.
 *
 * Budgeted; NOT part of make test. The probe restores BTN counters per call
 * (Delta-4) so the sweep leaves no reliability footprint. power_mode stays
 * DEFAULT (recon: a CNET_POWER_LOW cost tiebreak is the only thing that would
 * perturb pure-reliability ranking).
 */
#include "belowbeam_chars_common.h"

#include <stdio.h>
#ifdef _WIN32
#include <direct.h>  /* for _mkdir on MinGW/Windows (matches circuit_demo.c) */
#endif

/* The beam sizes swept as the second axis. */
static const size_t BEAMS[] = { 2, 4, 8 };
#define N_BEAMS (sizeof(BEAMS) / sizeof(BEAMS[0]))

/* Cold-start evidence knob (delta's output_successes). */
static const unsigned long COLDSTART_N[] = { 0, 1, 2, 4, 8, 16, 32, 64, 128 };
#define N_COLDSTART (sizeof(COLDSTART_N) / sizeof(COLDSTART_N[0]))

/* Rank-poisoning knob (number of injected-high competitors present). */
static const size_t POISON_M[] = { 0, 1, 2, 3, 4, 6, 8 };
#define N_POISON (sizeof(POISON_M) / sizeof(POISON_M[0]))

/* Cold-start fixes M=8 spread competitors regardless of beam. */
#define COLDSTART_COMPETITORS 8
/* Rank-poisoning fixes delta at moderate reliability. */
#define POISON_DELTA_SUCCESSES 8u

/* One swept point's measured fields (for both the table and the summary). */
typedef struct {
    size_t beam;
    double knob;          /* n (cold-start) or m (poisoning), as a double */
    double delta_rel;
    int rank;
    int rank_gap;         /* rank - beam */
    double margin;
    int found;
} SweepRow;

/* Run the probe over a built fixture + fill a SweepRow. The fixture's delta is
   always named "residue_step". Returns 0 on success, -1 if the probe errors. */
static int measure(BbcFixture *f, double knob, SweepRow *row) {
    CnetBelowBeamReport report;
    size_t i;
    double delta_rel = -1.0;

    memset(&report, 0, sizeof report);
    if (proposal_sidecar_below_beam_probe(&f->reg, f->sources, 2, f->goal,
                                          BBC_EXPECTED, &report) != 0) {
        return -1;
    }
    for (i = 0; i < f->reg.count; ++i) {
        if (strcmp(f->reg.entries[i].name, "residue_step") == 0) {
            delta_rel = btn_reliability(f->reg.entries[i].btn);
            break;
        }
    }
    row->beam = report.official_beam_limit;
    row->knob = knob;
    row->delta_rel = delta_rel;
    row->rank = report.recovered_producer_rank;
    row->rank_gap = report.recovered_producer_rank -
                    (int)report.official_beam_limit;
    row->margin = bbc_margin(&f->reg, "residue_step",
                             report.official_beam_limit);
    row->found = report.found_valid_below_beam;
    return 0;
}

/* Print one table row. */
static void print_row(const char *cause, const SweepRow *r) {
    printf("  %-13s %5zu  %8.0f  %10.4f  %5d  %8d  %9.4f  %5d\n",
           cause, r->beam, r->knob, r->delta_rel, r->rank, r->rank_gap,
           r->margin, r->found);
}

static void print_header(void) {
    printf("  %-13s %5s  %8s  %10s  %5s  %8s  %9s  %5s\n",
           "cause", "beam", "knob", "delta_rel", "rank", "rank_gap",
           "margin", "found");
}

/* Append a CSV row (NON-FATAL: csv may be NULL). */
static void csv_row(FILE *csv, const char *cause, const SweepRow *r) {
    if (csv == NULL) return;
    fprintf(csv, "%s,%zu,%.0f,%.6f,%d,%d,%.6f,%d\n",
            cause, r->beam, r->knob, r->delta_rel, r->rank, r->rank_gap,
            r->margin, r->found);
}

/* Per-cause rank-gap summary over the below-beam points (found == 1). */
static void summarize(const char *cause, const SweepRow *rows, size_t n) {
    /* Boundary: the knob value where `found` flips, reported per beam. For
       cold-start the flip is 1 -> 0 (the n that finally lifts delta into the
       beam); for poisoning it is 0 -> 1 (the m that finally pushes delta out).
       We just report, per beam, the first knob whose found differs from the
       previous knob at the same beam, plus the rank-gap stats over found==1. */
    size_t bi, i;
    int gaps[64];
    size_t n_gaps = 0;
    int gmin = 0, gmax = 0;
    double gmedian = 0.0;

    printf("\n  %s -- boundary knob per beam (where found flips), and rank-gap "
           "shape over below-beam points:\n", cause);

    for (bi = 0; bi < N_BEAMS; ++bi) {
        size_t beam = BEAMS[bi];
        int prev_found = -1;
        double boundary = -1.0;
        int have_boundary = 0;
        for (i = 0; i < n; ++i) {
            if (rows[i].beam != beam) continue;
            if (prev_found >= 0 && rows[i].found != prev_found &&
                !have_boundary) {
                boundary = rows[i].knob;
                have_boundary = 1;
            }
            prev_found = rows[i].found;
        }
        if (have_boundary) {
            printf("    beam=%zu: boundary knob = %.0f\n", beam, boundary);
        } else {
            printf("    beam=%zu: no flip across the swept knob range "
                   "(found constant)\n", beam);
        }
    }

    /* Rank-gap distribution over ALL below-beam points (found == 1). */
    for (i = 0; i < n; ++i) {
        if (rows[i].found == 1 && n_gaps < (sizeof(gaps) / sizeof(gaps[0]))) {
            gaps[n_gaps++] = rows[i].rank_gap;
        }
    }
    if (n_gaps == 0) {
        printf("    rank-gap (found==1): none (no below-beam points)\n");
        return;
    }
    /* simple insertion sort for min/median/max */
    for (i = 1; i < n_gaps; ++i) {
        int key = gaps[i];
        size_t j = i;
        while (j > 0 && gaps[j - 1] > key) { gaps[j] = gaps[j - 1]; --j; }
        gaps[j] = key;
    }
    gmin = gaps[0];
    gmax = gaps[n_gaps - 1];
    if (n_gaps % 2u == 1u) {
        gmedian = (double)gaps[n_gaps / 2];
    } else {
        gmedian = 0.5 * ((double)gaps[n_gaps / 2 - 1] + (double)gaps[n_gaps / 2]);
    }
    printf("    rank-gap (found==1, n=%zu): min=%d median=%.1f max=%d\n",
           n_gaps, gmin, gmedian, gmax);
}

static void sweep_coldstart(FILE *csv, SweepRow *rows, size_t *n_out) {
    size_t bi, ni, n = 0;
    BbcFixture f;

    printf("\n=== Cause 1: cold-start (under-ranked-correct) ===\n");
    printf("  M=%d spread competitors (successes 20+5*i); sweep delta's "
           "evidence n; beam in {2,4,8}.\n", COLDSTART_COMPETITORS);
    print_header();
    for (bi = 0; bi < N_BEAMS; ++bi) {
        for (ni = 0; ni < N_COLDSTART; ++ni) {
            if (bbc_make_coldstart(&f, COLDSTART_COMPETITORS,
                                   COLDSTART_N[ni], BEAMS[bi]) != 0) {
                fprintf(stderr, "cold-start fixture build failed "
                                "(beam=%zu n=%lu); aborting study\n",
                        BEAMS[bi], COLDSTART_N[ni]);
                bbc_fixture_free(&f);
                continue;
            }
            if (measure(&f, (double)COLDSTART_N[ni], &rows[n]) == 0) {
                print_row("cold-start", &rows[n]);
                csv_row(csv, "cold-start", &rows[n]);
                ++n;
            }
            bbc_fixture_free(&f);
        }
        printf("  -----\n");
    }
    *n_out = n;
}

static void sweep_poisoning(FILE *csv, SweepRow *rows, size_t *n_out) {
    size_t bi, mi, n = 0;
    BbcFixture f;

    printf("\n=== Cause 2: rank-poisoning (over-ranked-wrong) ===\n");
    printf("  delta FIXED at s=%u (rel ~0.9); sweep m injected-high competitors "
           "(successes 200+10*j, rel ~0.99); beam in {2,4,8}.\n",
           POISON_DELTA_SUCCESSES);
    print_header();
    for (bi = 0; bi < N_BEAMS; ++bi) {
        for (mi = 0; mi < N_POISON; ++mi) {
            if (bbc_make_poisoning(&f, POISON_M[mi], POISON_DELTA_SUCCESSES,
                                   BEAMS[bi]) != 0) {
                fprintf(stderr, "poisoning fixture build failed "
                                "(beam=%zu m=%zu); aborting study\n",
                        BEAMS[bi], POISON_M[mi]);
                bbc_fixture_free(&f);
                continue;
            }
            if (measure(&f, (double)POISON_M[mi], &rows[n]) == 0) {
                print_row("rank-poison", &rows[n]);
                csv_row(csv, "rank-poison", &rows[n]);
                ++n;
            }
            bbc_fixture_free(&f);
        }
        printf("  -----\n");
    }
    *n_out = n;
}

int main(void) {
    static SweepRow cold_rows[N_BEAMS * N_COLDSTART];
    static SweepRow poison_rows[N_BEAMS * N_POISON];
    size_t n_cold = 0, n_poison = 0;
    FILE *csv;
    int wrote_csv = 0;

    printf("=== Below-Beam Failure-Mode Characterization study ===\n");
    printf("Reuses the v5.1 probe verbatim; power_mode DEFAULT; Delta-4 keeps "
           "the sweep footprint-free.\n");
    printf("Correct producer = residue delta (b=%d,k=%d); goal ONEHOT %d "
           "\"r_next\"; expected argmax = %d.\n",
           BBC_B, BBC_K, BBC_K, BBC_EXPECTED);

    /* CSV is best-effort: mkdir + open may fail (missing/unwritable artifacts
       tree) -> non-fatal, print a note and continue (project convention). The
       _mkdir / mkdir -p idiom mirrors circuit_demo.c; failures are ignored
       (the dir may already exist) and only an fopen failure suppresses the CSV. */
#ifdef _WIN32
    (void)_mkdir("artifacts");
    (void)_mkdir("artifacts/belowbeam_chars");
#else
    (void)system("mkdir -p artifacts/belowbeam_chars 2>/dev/null || true");
#endif
    csv = fopen("artifacts/belowbeam_chars/belowbeam_chars.csv", "w");
    if (csv == NULL) {
        printf("(note: could not open artifacts/belowbeam_chars/"
               "belowbeam_chars.csv for writing; continuing, table only)\n");
    } else {
        fprintf(csv, "cause,beam_k,knob,delta_rel,rank,rank_gap,margin,found\n");
        wrote_csv = 1;
    }

    sweep_coldstart(csv, cold_rows, &n_cold);
    sweep_poisoning(csv, poison_rows, &n_poison);

    printf("\n=== Per-cause summaries ===\n");
    summarize("cold-start", cold_rows, n_cold);
    summarize("rank-poison", poison_rows, n_poison);

    if (csv != NULL) {
        fclose(csv);
    }
    if (wrote_csv) {
        printf("\nWrote artifacts/belowbeam_chars/belowbeam_chars.csv\n");
    }

    printf("\nHonest scope: this is the reliability-margin boundary + the "
           "rank-gap shape, both mechanism-intrinsic. The real-deployment "
           "blind-spot frequency is the deployer's convolution of their own "
           "reliability-margin distribution against this boundary -- it is NOT "
           "measured here.\n");
    return 0;
}
