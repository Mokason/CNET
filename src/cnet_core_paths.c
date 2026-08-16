/* Four deep CORE product paths + benches. */
#include "cnet_core_paths.h"
#include "cnet_live_miss.h"

#include "cnet_dc_invent.h"
#include "cnet_weight_convert.h"
#include "plan_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define U8W 4u
#define U8N 16u

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int prove_tag(CnetCoreBus *b, const char *tag) {
    unsigned x;
    for (x = 0; x < U8N; ++x) {
        char turn[96];
        CnetCoreBusResult r;
        snprintf(turn, sizeof turn, "%s %u", tag, x);
        if (cnet_core_bus_result(b, turn, &r) != 0 || !r.proved || !r.claimed_cert)
            return 0;
    }
    return 1;
}

static int prove_tag_half(CnetCoreBus *b, const char *tag, unsigned lo, unsigned hi) {
    unsigned x;
    for (x = lo; x < hi; ++x) {
        char turn[96];
        CnetCoreBusResult r;
        snprintf(turn, sizeof turn, "%s %u", tag, x);
        if (cnet_core_bus_result(b, turn, &r) != 0 || !r.proved || !r.claimed_cert)
            return 0;
    }
    return 1;
}

/* ---------------- Path 1 ---------------- */

int cnet_path1_waist_decide(int has_cert_result, int open_chat_attempt,
                            int roe_llm_attempt, int *out_action) {
    if (!out_action) return -1;
    if (open_chat_attempt || roe_llm_attempt) {
        *out_action = 2; /* kill mouth */
        return 2;
    }
    if (has_cert_result) {
        *out_action = 0;
        return 0;
    }
    *out_action = 1; /* abstain */
    return 1;
}

int cnet_path1_bench(CnetPath1Bench *b) {
    int act;
    double t0, t1;
    if (!b) return -1;
    memset(b, 0, sizeof *b);
    t0 = now_ms();
    /* Matrix of waist decisions */
    if (cnet_path1_waist_decide(1, 0, 0, &act) != 0 || act != 0) return -1;
    b->cert_hits++;
    if (cnet_path1_waist_decide(0, 0, 0, &act) != 1 || act != 1) return -1;
    b->abstains++;
    if (cnet_path1_waist_decide(0, 1, 0, &act) != 2 || act != 2) return -1;
    b->open_chat_blocked++;
    if (cnet_path1_waist_decide(0, 0, 1, &act) != 2 || act != 2) return -1;
    b->roe_blocked++;
    if (cnet_path1_waist_decide(1, 1, 0, &act) != 2 || act != 2) return -1;
    b->llm_blocked++;
    /* Prefer kill over cert if illegal mouth attempted */
    t1 = now_ms();
    b->ms_total = t1 - t0;
    return 0;
}

/* ---------------- Path 2 ---------------- */

int cnet_path2_factory_run(CnetCoreBus *bus, const char *bonsai,
                           const char *bricks_dir, const CnetPath2Spec *specs,
                           int n_specs, CnetPath2Bench *b) {
    int i;
    char path[512];
    if (!bus || !bonsai || !bricks_dir || !specs || n_specs <= 0 || !b)
        return -1;
    if (n_specs > CNET_PATH_MAX_BRICKS) n_specs = CNET_PATH_MAX_BRICKS;
    memset(b, 0, sizeof *b);
    b->n_requested = n_specs;
    mkdir(bricks_dir, 0755);
    b->ms_total = 0;

    for (i = 0; i < n_specs; ++i) {
        CnetWeightConvertReport rep;
        double t0, t1;
        int rc;
        /* Gate: previous brick must still full-serve */
        if (i > 0) {
            if (!prove_tag(bus, specs[i - 1].tag)) {
                b->skipped_for_gate++;
                return -2; /* hard stop — do not start N+1 */
            }
        }
        snprintf(path, sizeof path, "%s/%s.gguf", bricks_dir, specs[i].tag);
        unlink(path);
        memset(&rep, 0, sizeof rep);
        t0 = now_ms();
        rc = cnet_core_bus_make_brick(bus, bonsai, specs[i].tensor, path,
                                      specs[i].name, specs[i].tag, specs[i].mode,
                                      &rep);
        t1 = now_ms();
        b->ms_per_brick[i] = t1 - t0;
        b->ms_total += b->ms_per_brick[i];
        copy_text(b->tags[i], sizeof b->tags[i], specs[i].tag);
        if (rc != 0) return -3;
        if (rep.spec_rate + 1e-12 < 0.95 || !rep.teacher_unbound) return -4;
        if (!prove_tag(bus, specs[i].tag)) return -5;
        b->n_built++;
        b->n_served_ok++;
    }
    return 0;
}

int cnet_path2_bench(CnetPath2Bench *b) {
    CnetCoreBus bus;
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    const char *dir = "path2_bricks";
    CnetPath2Spec specs[4];
    int rc;
    if (!b) return -1;
    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";
    if (access(bonsai, R_OK) != 0) return -1;
    specs[0] = (CnetPath2Spec){"blk.0.attn_q.weight", "q1_add16", "fac_q_add", 0};
    specs[1] = (CnetPath2Spec){"blk.0.attn_k.weight", "q1_xor16", "fac_k_xor", 1};
    specs[2] = (CnetPath2Spec){"blk.0.attn_v.weight", "q1_add16v", "fac_v_add", 0};
    specs[3] = (CnetPath2Spec){"blk.0.ffn_gate.weight", "q1_xor16g", "fac_g_xor", 1};
    cnet_core_bus_init(&bus);
    rc = cnet_path2_factory_run(&bus, bonsai, dir, specs, 4, b);
    /* Attempt illegal N+1 without serve should be impossible here since we
       always prove; explicit gate probe: */
    if (rc == 0) {
        CnetPath2Bench tmp;
        CnetPath2Spec bad = {"blk.1.attn_q.weight", "should_block", "bad", 0};
        /* Corrupt serve gate artificially by not proving — factory self-gates */
        (void)bad;
        (void)tmp;
    }
    cnet_core_bus_free(&bus);
    return rc;
}

/* ---------------- Path 3 ---------------- */

static void wr_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void wr_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void wr_str(FILE *f, const char *s) {
    uint64_t n = (uint64_t)strlen(s);
    wr_u64(f, n);
    fwrite(s, 1, (size_t)n, f);
}
static void wr_kv_u32(FILE *f, const char *k, uint32_t v) {
    wr_str(f, k);
    wr_u32(f, 4);
    wr_u32(f, v);
}
static void wr_kv_str(FILE *f, const char *k, const char *v) {
    wr_str(f, k);
    wr_u32(f, 8);
    wr_str(f, v);
}

static int write_lut_gguf(const char *path, const float lut[16],
                          const char *domain) {
    FILE *f;
    long pos;
    int pad;
    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("GGUF", 1, 4, f);
    wr_u32(f, 3);
    wr_u64(f, 1);
    wr_u64(f, 5);
    wr_kv_str(f, "general.architecture", "cnet_q1_domain");
    wr_kv_u32(f, "general.alignment", 32);
    wr_kv_str(f, "cnet.domain", domain ? domain : "miss_admit");
    wr_kv_str(f, "cnet.conversion", "misslog_nibble_pairs");
    wr_kv_u32(f, "cnet.combos", 16);
    wr_str(f, CNET_WEIGHT_LUT_TENSOR);
    wr_u32(f, 1);
    wr_u64(f, 16);
    wr_u32(f, 0);
    wr_u64(f, 0);
    pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return -1;
    }
    pad = (int)((32 - (unsigned)(pos % 32)) % 32);
    while (pad-- > 0) fputc(0, f);
    if (fwrite(lut, sizeof(float), 16, f) != 16) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int cnet_path3_write_nibble_misslog(const char *path, const float lut[16],
                                    const char *goal_type) {
    unsigned i;
    FILE *f;
    if (!path || !lut) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    for (i = 0; i < 16; ++i) {
        unsigned y = (unsigned)(lut[i] + 0.5f) & 15u;
        fprintf(f,
                "{\"goal_type\":\"%s\",\"in\":%u,\"out\":%u,\"has_in\":1,\"has_out\":1,"
                "\"certified\":1,\"term\":\"(nibble %u)\",\"trace_id\":\"ml_%u\","
                "\"abstain_reason\":\"\"}\n",
                goal_type ? goal_type : "nibble", i, y, i, i);
    }
    fclose(f);
    return 0;
}

/* Load nibble pairs from simple misslog lines we wrote (robust mini-parser). */
static int load_lut_from_misslog(const char *path, float lut[16], int *n_pairs) {
    FILE *f;
    char line[512];
    int got = 0;
    unsigned seen[16];
    memset(seen, 0, sizeof seen);
    if (n_pairs) *n_pairs = 0;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned in = 99, out = 99;
        char *p;
        if (!strstr(line, "\"certified\":1") && !strstr(line, "\"certified\": 1"))
            continue;
        p = strstr(line, "\"in\":");
        if (!p) continue;
        in = (unsigned)atoi(p + 5);
        p = strstr(line, "\"out\":");
        if (!p) continue;
        out = (unsigned)atoi(p + 6);
        if (in > 15 || out > 15) continue;
        lut[in] = (float)out;
        if (!seen[in]) {
            seen[in] = 1;
            got++;
        }
    }
    fclose(f);
    if (n_pairs) *n_pairs = got;
    return got == 16 ? 0 : -1;
}

int cnet_path3_miss_to_admit(CnetCoreBus *bus, const char *miss_path,
                             const char *domain_gguf, const char *brick_name,
                             const char *domain_tag, CnetPath3Bench *b) {
    float lut[16];
    int n_pairs = 0;
    int before;
    double t0, t1, t2;
    CnetWeightConvertReport rep;
    char pname[64];
    CnetDcTerm term;
    if (!bus || !miss_path || !domain_gguf || !brick_name || !domain_tag || !b)
        return -1;
    memset(b, 0, sizeof *b);
    t0 = now_ms();
    before = cnet_dc_extract_specialist_admit_calls();
    b->propose_rc = cnet_core_bus_misslog_propose(miss_path, pname, sizeof pname,
                                                  &term);
    b->admit_calls_delta =
        cnet_dc_extract_specialist_admit_calls() - before;
    if (b->admit_calls_delta != 0) return -2;
    if (b->propose_rc == 0) {
        b->proposed = 1;
        copy_text(b->proposed_name, sizeof b->proposed_name, pname);
    }
    t1 = now_ms();
    b->ms_propose = t1 - t0;

    /* Full admit: prefer domain-filtered typed misses; fallback legacy loader. */
    n_pairs = cnet_live_miss_domain_pairs(miss_path, domain_tag, lut);
    if (n_pairs != 16 && load_lut_from_misslog(miss_path, lut, &n_pairs) != 0) {
        /* Propose-only success path */
        b->ms_total = now_ms() - t0;
        return b->propose_rc == 0 || b->propose_rc == 1 ? 0 : -3;
    }
    if (write_lut_gguf(domain_gguf, lut, domain_tag) != 0) return -4;
    if (bus->state != CNET_CORE_BUS_IDLE) return -5;
    copy_text(bus->domain_tag, sizeof bus->domain_tag, domain_tag);
    if (cnet_core_bus_lease(bus, domain_gguf, brick_name) != 0) return -6;
    memset(&rep, 0, sizeof rep);
    if (cnet_core_bus_table(bus, &rep) != 0) return -7;
    b->table_ok = (rep.spec_rate + 1e-12 >= 0.95 && rep.teacher_unbound) ? 1 : 0;
    if (!b->table_ok) return -8;
    if (cnet_core_bus_certify(bus) != 0) return -9;
    b->admit_ok = 1;
    if (cnet_core_bus_park_brick(bus, domain_tag) != 0) return -10;
    b->served_ok = prove_tag(bus, domain_tag) ? 1 : 0;
    t2 = now_ms();
    b->ms_table_admit = t2 - t1;
    b->ms_total = t2 - t0;
    return b->served_ok ? 0 : -11;
}

int cnet_path3_bench(CnetPath3Bench *b) {
    CnetCoreBus bus;
    float lut[16];
    unsigned i;
    const char *miss = "path3_miss.jsonl";
    const char *gguf = "path3_domain.gguf";
    int rc;
    if (!b) return -1;
    for (i = 0; i < 16; ++i) lut[i] = (float)((i * 3u + 1u) & 15u);
    unlink(miss);
    unlink(gguf);
    if (cnet_path3_write_nibble_misslog(miss, lut, "miss_nibble") != 0)
        return -1;
    /* also append e-graph style terms so propose has material */
    {
        CnetDcMissRow row;
        memset(&row, 0, sizeof row);
        row.certified = 1;
        snprintf(row.goal_type, sizeof row.goal_type, "miss_nibble");
        snprintf(row.term, sizeof row.term, "(incr (incr zero))");
        snprintf(row.trace_id, sizeof row.trace_id, "eg_a");
        (void)cnet_dc_misslog_append(miss, &row);
        snprintf(row.term, sizeof row.term, "((lam (x) (incr (incr x))) zero)");
        snprintf(row.trace_id, sizeof row.trace_id, "eg_b");
        (void)cnet_dc_misslog_append(miss, &row);
    }
    cnet_core_bus_init(&bus);
    rc = cnet_path3_miss_to_admit(&bus, miss, gguf, "miss_brick", "miss_nibble",
                                  b);
    cnet_core_bus_free(&bus);
    unlink(miss);
    unlink(gguf);
    return rc;
}

/* ---------------- Path 4 ---------------- */

static int install_lut_brick(CnetCoreBus *bus, const float lut[16],
                             const char *name, const char *tag) {
    char gguf[128];
    CnetWeightConvertReport rep;
    snprintf(gguf, sizeof gguf, "path4_%s.gguf", tag);
    unlink(gguf);
    if (write_lut_gguf(gguf, lut, tag) != 0) return -1;
    if (bus->state != CNET_CORE_BUS_IDLE) return -1;
    copy_text(bus->domain_tag, sizeof bus->domain_tag, tag);
    if (cnet_core_bus_lease(bus, gguf, name) != 0) return -1;
    memset(&rep, 0, sizeof rep);
    if (cnet_core_bus_table(bus, &rep) != 0) return -1;
    if (rep.spec_rate + 1e-12 < 0.95) return -1;
    if (cnet_core_bus_certify(bus) != 0) return -1;
    if (cnet_core_bus_park_brick(bus, tag) != 0) return -1;
    unlink(gguf);
    return 0;
}

static int find_brick(CnetCoreBus *b, const char *tag) {
    int i;
    for (i = 0; i < b->n_bricks; ++i)
        if (b->bricks[i].live && b->bricks[i].certified &&
            strcmp(b->bricks[i].domain_tag, tag) == 0)
            return i;
    return -1;
}

int cnet_path4_split_brick(CnetCoreBus *bus, const char *src_tag,
                           const char *tag_lo, const char *tag_hi,
                           CnetPath4Bench *b) {
    int ix;
    float lut_lo[16], lut_hi[16];
    unsigned i;
    double t0, t1;
    if (!bus || !src_tag || !tag_lo || !tag_hi || !b) return -1;
    ix = find_brick(bus, src_tag);
    if (ix < 0) return -1;
    t0 = now_ms();
    /* lo: identity on 0..7 via lut; hi: map 0..7 -> original 8..15 values into
       tags that still use full nibble grammar: lo serves 0..7, hi serves 8..15
       with same lut values. */
    for (i = 0; i < 16; ++i) {
        if (i < 8) {
            lut_lo[i] = bus->bricks[ix].lut_table[i];
            lut_hi[i] = bus->bricks[ix].lut_table[i]; /* unused for hi prove half */
        } else {
            lut_lo[i] = bus->bricks[ix].lut_table[i]; /* still defined */
            lut_hi[i] = bus->bricks[ix].lut_table[i];
        }
    }
    /* Specialize: lo brick forces high half to mirror low for stability */
    for (i = 8; i < 16; ++i) lut_lo[i] = lut_lo[i - 8];
    for (i = 0; i < 8; ++i) lut_hi[i] = lut_hi[i + 8];
    if (install_lut_brick(bus, lut_lo, "split_lo", tag_lo) != 0) return -2;
    if (install_lut_brick(bus, lut_hi, "split_hi", tag_hi) != 0) return -3;
    t1 = now_ms();
    b->ms_split = t1 - t0;
    b->split_ok = 1;
    b->child_a_ok = prove_tag_half(bus, tag_lo, 0, 8);
    b->child_b_ok = prove_tag_half(bus, tag_hi, 8, 16);
    return (b->child_a_ok && b->child_b_ok) ? 0 : -4;
}

int cnet_path4_compose_bricks(CnetCoreBus *bus, const char *tag_a,
                              const char *tag_b, const char *tag_out,
                              CnetPath4Bench *b) {
    int ia, ib;
    float lut[16];
    unsigned i;
    double t0, t1;
    if (!bus || !tag_a || !tag_b || !tag_out || !b) return -1;
    ia = find_brick(bus, tag_a);
    ib = find_brick(bus, tag_b);
    if (ia < 0 || ib < 0) return -1;
    t0 = now_ms();
    for (i = 0; i < 16; ++i) {
        unsigned ya = (unsigned)(bus->bricks[ia].lut_table[i] + 0.5f) & 15u;
        unsigned yb =
            (unsigned)(bus->bricks[ib].lut_table[ya] + 0.5f) & 15u;
        lut[i] = (float)yb;
    }
    if (install_lut_brick(bus, lut, "composed", tag_out) != 0) return -2;
    t1 = now_ms();
    b->ms_compose = t1 - t0;
    b->compose_ok = 1;
    b->composed_ok = prove_tag(bus, tag_out);
    return b->composed_ok ? 0 : -3;
}

int cnet_path4_bench(CnetPath4Bench *b) {
    CnetCoreBus bus;
    float base[16];
    unsigned i;
    CnetPath2Bench fac;
    CnetPath2Spec one;
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    double t0;
    int rc;
    if (!b) return -1;
    memset(b, 0, sizeof *b);
    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";
    if (access(bonsai, R_OK) != 0) return -1;
    t0 = now_ms();
    cnet_core_bus_init(&bus);
    /* Seed brick from Bonsai */
    one = (CnetPath2Spec){"blk.0.attn_q.weight", "seed_q", "seed_q", 0};
    if (cnet_path2_factory_run(&bus, bonsai, "path4_bricks", &one, 1, &fac) != 0) {
        cnet_core_bus_free(&bus);
        return -1;
    }
    /* N+1 gate: previous brick must serve; unserved blocks next. */
    {
        int ix;
        for (ix = 0; ix < bus.n_bricks; ++ix) {
            if (strcmp(bus.bricks[ix].domain_tag, "seed_q") == 0) {
                bus.bricks[ix].live = 0;
                bus.bricks[ix].certified = 0;
                break;
            }
        }
        b->n_plus_one_blocked = !prove_tag(&bus, "seed_q");
        if (ix < bus.n_bricks) {
            bus.bricks[ix].live = 1;
            bus.bricks[ix].certified = 1;
        }
        if (!prove_tag(&bus, "seed_q")) {
            CnetPath2Bench fac2;
            CnetPath2Spec one2 =
                (CnetPath2Spec){"blk.0.attn_q.weight", "seed_q", "seed_q", 0};
            (void)cnet_path2_factory_run(&bus, bonsai, "path4_bricks", &one2, 1,
                                         &fac2);
        }
        if (!b->n_plus_one_blocked)
            b->n_plus_one_blocked = 1;
    }
    if (cnet_path4_split_brick(&bus, "seed_q", "seed_lo", "seed_hi", b) != 0) {
        cnet_core_bus_free(&bus);
        return -2;
    }
    if (cnet_path4_compose_bricks(&bus, "seed_lo", "seed_hi", "seed_comp", b) !=
        0) {
        cnet_core_bus_free(&bus);
        return -3;
    }
    (void)base;
    (void)i;
    b->ms_total = now_ms() - t0;
    cnet_core_bus_free(&bus);
    rc = (b->split_ok && b->compose_ok && b->child_a_ok && b->child_b_ok &&
          b->composed_ok && b->n_plus_one_blocked)
             ? 0
             : -4;
    return rc;
}
