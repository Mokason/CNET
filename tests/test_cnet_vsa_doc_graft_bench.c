#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_doc_graft.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint64_t bench_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Priority 4: Doc-Graft Hierarchical Document Ingest\n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;
    uint64_t rng = 9988776655ULL;

    CnetVsaDocGraft dg;
    cnet_vsa_doc_graft_init(&dg, D);

    /* -------------------------------------------------------------
     * Suite 1: Ingesting 12-Clause Master Service Agreement (MSA)
     * ------------------------------------------------------------- */
    printf("[1/3] Ingesting Hierarchical Document Sections & Clauses...\n");

    /* Create 4 distinct section basis vectors */
    float sec_v[4][CNET_VSA_DEFAULT_DIM];
    for (int s = 0; s < 4; ++s) cnet_vsa_random(sec_v[s], D, &rng);

    /* Generate clause vectors with realistic local semantic variation */
    float clause_v[12][CNET_VSA_DEFAULT_DIM];
    for (int i = 0; i < 12; ++i) {
        int sec_idx = i / 3;
        float noise[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise, D, &rng);
        for (int j = 0; j < D; ++j) {
            clause_v[i][j] = sec_v[sec_idx][j] + 0.20f * noise[j];
        }
        cnet_vsa_normalize(clause_v[i], D);
    }

    /* Ingest clauses */
    cnet_vsa_doc_graft_add_clause(&dg, "Warranty", "Section 1.1: System warranty covers hardware defects for 24 months.", 12, clause_v[0]);
    cnet_vsa_doc_graft_add_clause(&dg, "Warranty", "Section 1.2: Replacement parts are shipped via priority courier within 48h.", 24, clause_v[1]);
    cnet_vsa_doc_graft_add_clause(&dg, "Warranty", "Section 1.3: Software maintenance releases provided free during warranty term.", 36, clause_v[2]);

    cnet_vsa_doc_graft_add_clause(&dg, "Exclusions", "Section 2.1: Liquid immersion or deliberate tampering immediately voids warranty.", 48, clause_v[3]);
    cnet_vsa_doc_graft_add_clause(&dg, "Exclusions", "Section 2.2: Third-party component installation without prior approval is excluded.", 60, clause_v[4]);
    cnet_vsa_doc_graft_add_clause(&dg, "Exclusions", "Section 2.3: Acts of God, floods, and electrical power surges are not covered.", 72, clause_v[5]);

    cnet_vsa_doc_graft_add_clause(&dg, "Dispute", "Section 3.1: Parties agree to 30-day informal mediation prior to legal filing.", 84, clause_v[6]);
    cnet_vsa_doc_graft_add_clause(&dg, "Dispute", "Section 3.2: Formal binding arbitration shall take place in Geneva, Switzerland under ICC rules.", 96, clause_v[7]);
    cnet_vsa_doc_graft_add_clause(&dg, "Dispute", "Section 3.3: Prevailing party entitled to reasonable attorney fees and costs.", 108, clause_v[8]);

    cnet_vsa_doc_graft_add_clause(&dg, "Privacy", "Section 4.1: Customer telemetry data is stored encrypted at rest using AES-256.", 120, clause_v[9]);
    cnet_vsa_doc_graft_add_clause(&dg, "Privacy", "Section 4.2: Data processing complies strictly with EU GDPR and Swiss FADP mandates.", 132, clause_v[10]);
    cnet_vsa_doc_graft_add_clause(&dg, "Privacy", "Section 4.3: Right to erasure honored within 14 business days upon verified notice.", 144, clause_v[11]);

    printf("  Clauses ingested: %zu\n", dg.clause_count);
    printf("  Sections formed:  %zu\n", dg.section_count);
    check(dg.clause_count == 12, "All 12 document clauses successfully ingested into session graph");
    check(dg.section_count == 4, "All 4 document sections formed hierarchical centroids");

    /* -------------------------------------------------------------
     * Suite 2: Needle Query & Cross-Clause Discovery
     * ------------------------------------------------------------- */
    printf("\n[2/3] Performing Needle Query & Cross-Clause Conflict Discovery...\n");

    /* Needle query: probe for arbitration clause (clause 7) under noise */
    float probe_arbitration[CNET_VSA_DEFAULT_DIM];
    float query_noise[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(query_noise, D, &rng);
    for (int j = 0; j < D; ++j) {
        probe_arbitration[j] = clause_v[7][j] + 0.20f * query_noise[j];
    }
    cnet_vsa_normalize(probe_arbitration, D);

    const CnetVsaDocClause *needle_match = NULL;
    float needle_sim = 0.0f;
    cnet_vsa_doc_graft_query(&dg, probe_arbitration, &needle_match, &needle_sim);

    check(needle_match != NULL && needle_match->clause_id == 8,
          "Hierarchical query pinpoints exact Geneva arbitration needle clause");
    check(needle_match->line_number == 96, "Query correctly recovers source line number (Line 96)");
    check(strstr(needle_match->text, "Geneva, Switzerland") != NULL,
          "Retrieved clause contains exact legal text");

    /* Cross-clause conflict discovery */
    const CnetVsaDocClause *c_a = NULL;
    const CnetVsaDocClause *c_b = NULL;
    float joint_score = 0.0f;
    cnet_vsa_doc_graft_find_cross_reference(&dg, clause_v[0], clause_v[3], &c_a, &c_b, &joint_score);

    check(c_a != NULL && c_b != NULL, "Cross-reference resolves both related clauses");
    check(c_a->clause_id == 1 && c_b->clause_id == 4,
          "Identifies cross-clause link between 24-month warranty and liquid exclusion");
    printf("  Cross-link: [%s L%d] <--> [%s L%d] (Joint score: %.2f)\n",
           c_a->section_name, c_a->line_number, c_b->section_name, c_b->line_number, joint_score);

    /* -------------------------------------------------------------
     * Suite 3: Throughput & Retrieval Latency
     * ------------------------------------------------------------- */
    printf("\n[3/3] Measuring Doc-Graft Retrieval Throughput...\n");
    const int TIMING_RUNS = 10000;
    uint64_t t0 = bench_mono_ns();
    for (int i = 0; i < TIMING_RUNS; ++i) {
        const CnetVsaDocClause *dummy_c = NULL;
        float dummy_s = 0.0f;
        cnet_vsa_doc_graft_query(&dg, probe_arbitration, &dummy_c, &dummy_s);
    }
    uint64_t t1 = bench_mono_ns();
    double total_ms = (double)(t1 - t0) / 1000000.0;
    double us_per_query = (total_ms * 1000.0) / (double)TIMING_RUNS;
    printf("  %d document graph queries in %.2f ms (%.2f us/query)\n",
           TIMING_RUNS, total_ms, us_per_query);
    check(us_per_query < 20.0, "Doc-Graft query executes in sub-20 microseconds per query");

    cnet_vsa_doc_graft_free(&dg);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_DOC_GRAFT_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_DOC_GRAFT_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
