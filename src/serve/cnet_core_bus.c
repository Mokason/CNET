/* CORE bus — lease / table / certify / result. No leftover mouth. */
#include "cnet_core_bus.h"
#include "cnet_core_serve.h"
#include "plan_table.h"

#include "cce/cce_gguf.h"
#include "contract/contract.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U8W 4u
#define U8N 16u

typedef struct {
    float *lut;
    int live;
} LutServeCtx;

static int lut_forward_ctx(void *ctx, const double *in, size_t input_count,
                           double *out, size_t output_count) {
    LutServeCtx *lc = (LutServeCtx *)ctx;
    unsigned x = 0, y, j;
    if (!lc || !lc->live || !lc->lut || !in || !out) return -1;
    if (input_count != U8W || output_count != U8W) return -1;
    for (j = 0; j < U8W; ++j) {
        if (in[j] > 0.5) x |= 1u << (unsigned)(U8W - 1u - j);
    }
    if (x >= U8N) return -1;
    y = (unsigned)(lc->lut[x] + 0.5f) & 15u;
    for (j = 0; j < U8W; ++j) {
        unsigned bit = (unsigned)(U8W - 1u - j);
        out[j] = (double)((y >> bit) & 1u);
    }
    return 0;
}

static int bus_forward(void *ctx, const double *in, size_t ic, double *out,
                       size_t oc) {
    CnetCoreBus *b = (CnetCoreBus *)ctx;
    LutServeCtx lc;
    if (!b) return -1;
    lc.lut = b->lut_table;
    lc.live = b->lut_live;
    return lut_forward_ctx(&lc, in, ic, out, oc);
}

static int brick_forward(void *ctx, const double *in, size_t ic, double *out,
                         size_t oc) {
    CnetCoreBrick *br = (CnetCoreBrick *)ctx;
    LutServeCtx lc;
    if (!br) return -1;
    lc.lut = br->lut_table;
    lc.live = br->live && br->certified;
    return lut_forward_ctx(&lc, in, ic, out, oc);
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

void cnet_core_bus_init(CnetCoreBus *b) {
    if (!b) return;
    memset(b, 0, sizeof *b);
    registry_init(&b->reg);
    memset(&b->oracles, 0, sizeof b->oracles);
    acquire_oracle_policy_defaults(&b->policy);
    b->policy.require_attested_to_teach = 1;
    b->policy.require_lease_to_teach = 1;
    acquire_oracle_policy_set(&b->oracles, &b->policy);
    copy_text(b->domain, sizeof b->domain, CNET_WEIGHT_DOMAIN_U8_INC16);
    copy_text(b->name, sizeof b->name, "core_bus_student");
    copy_text(b->domain_tag, sizeof b->domain_tag, "q1_xor16");
}

void cnet_core_bus_free(CnetCoreBus *b) {
    int i;
    if (!b) return;
    if (b->wo.bound) (void)cnet_weight_unbind(&b->wo);
    cnet_weight_unmap(&b->file);
    if (b->student_live) btn_free(&b->student);
    for (i = 0; i < b->n_bricks; ++i) {
        if (b->bricks[i].live) {
            btn_free(&b->bricks[i].student);
            registry_free(&b->bricks[i].reg);
        }
    }
    registry_free(&b->reg);
    memset(b, 0, sizeof *b);
}

int cnet_core_bus_unbind(CnetCoreBus *b) {
    if (!b) return -1;
    if (b->wo.bound) {
        if (cnet_weight_unbind(&b->wo) != 0) return -1;
    }
    if (b->state == CNET_CORE_BUS_LEASED) b->state = CNET_CORE_BUS_IDLE;
    return 0;
}

int cnet_core_bus_lease(CnetCoreBus *b, const char *gguf_path, const char *name) {
    CnetOracleIdentity id;
    if (!b || !gguf_path || !gguf_path[0]) return -1;
    if (b->state != CNET_CORE_BUS_IDLE && b->state != CNET_CORE_BUS_LEASED)
        return -1;
    if (b->wo.bound) (void)cnet_weight_unbind(&b->wo);
    cnet_weight_unmap(&b->file);
    memset(&id, 0, sizeof id);
    if (cnet_weight_attest(gguf_path, &id) != 0) return -1;
    if (cnet_weight_mmap(&b->file, gguf_path) != 0) return -1;
    if (name && name[0]) copy_text(b->name, sizeof b->name, name);
    if (cnet_weight_bind(&b->wo, &b->oracles, &b->file, b->name) != 0) {
        cnet_weight_unmap(&b->file);
        return -1;
    }
    b->state = CNET_CORE_BUS_LEASED;
    b->certified = 0;
    return 0;
}

int cnet_core_bus_table(CnetCoreBus *b, CnetWeightConvertReport *rep) {
    CnetWeightConvertReport local;
    CnetWeightConvertReport *r = rep ? rep : &local;
    PlanTable table;
    Port pin, pout;
    unsigned x;
    if (!b) return -1;
    if (b->state != CNET_CORE_BUS_LEASED || !b->wo.bound) return -1;
    if (!b->file.lut || b->file.lut_n < U8N) return -1;
    memset(r, 0, sizeof *r);
    snprintf(r->domain, sizeof r->domain, "%s", b->domain);
    r->combos = U8N;
    r->fixture = b->file.fixture;
    r->reader_cce_gguf = b->file.reader_cce_gguf;
    r->certified = 0;
    memcpy(b->lut_table, b->file.lut, sizeof b->lut_table);
    b->lut_live = 1;
    pin = cnet_weight_u8_inc16_in_port();
    pout = cnet_weight_u8_inc16_out_port();
    memset(&table, 0, sizeof table);
    if (plan_table_build(&pin, 1, U8W, cnet_weight_labeler, &b->wo, NULL, 0, U8N,
                         &table) != 0)
        return -1;
    r->kept = table.kept;
    r->aborts = table.aborts;
    if (table.kept != U8N) {
        plan_table_free(&table);
        return -1;
    }
    if (b->student_live) btn_free(&b->student);
    memset(&b->student, 0, sizeof b->student);
    if (btn_init_adapter(&b->student, (int)U8W, (int)U8W, &pin, 1, &pout, 1,
                         bus_forward, NULL, b, 0x5131584f52313600ULL, 1) != 0) {
        plan_table_free(&table);
        return -1;
    }
    r->verified = 0;
    r->missed = 0;
    for (x = 0; x < U8N; ++x) {
        const double *want = table.targets + (size_t)x * table.out_total;
        const double *in = table.inputs + (size_t)x * table.in_total;
        const double *got = btn_forward(&b->student, in);
        size_t j;
        int ok = got != NULL;
        if (ok) {
            for (j = 0; j < U8W; ++j) {
                if ((got[j] > 0.5) != (want[j] > 0.5)) {
                    ok = 0;
                    break;
                }
            }
        }
        if (ok) r->verified++;
        else r->missed++;
    }
    r->spec_rate = (double)r->verified / (double)table.kept;
    plan_table_free(&table);
    if (r->spec_rate + 1e-12 < CNET_WEIGHT_SPEC_BAR) {
        btn_free(&b->student);
        b->lut_live = 0;
        return -1;
    }
    if (cnet_weight_unbind(&b->wo) == 0) r->teacher_unbound = 1;
    r->residual_speak = b->wo.residual_speak;
    r->gguf_reads = b->file.gguf_reads;
    b->student_live = 1;
    b->state = CNET_CORE_BUS_TABLED;
    b->certified = 0;
    return 0;
}

int cnet_core_bus_creative_emit_table(CnetCoreBus *b,
                                      CnetWeightConvertReport *rep) {
    return cnet_core_bus_table(b, rep);
}

int cnet_core_bus_certify(CnetCoreBus *b) {
    Contract c;
    double inputs[U8N * U8W];
    double targets[U8N * U8W];
    unsigned x;
    if (!b || !b->student_live) return -1;
    if (b->state != CNET_CORE_BUS_TABLED) return -1;
    if (b->wo.bound || b->wo.lease != 0) return -1;
    for (x = 0; x < U8N; ++x) {
        size_t j;
        double in[U8W], out[U8W];
        for (j = 0; j < U8W; ++j) {
            unsigned bit = (unsigned)(U8W - 1u - (unsigned)j);
            in[j] = (double)((x >> bit) & 1u);
        }
        if (cnet_weight_serve(&b->wo, &b->student, in, out) != 0) return -1;
        memcpy(inputs + x * U8W, in, sizeof in);
        memcpy(targets + x * U8W, out, sizeof out);
    }
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, b->name, &b->student, inputs, targets,
                               (int)U8N) != 0)
        return -1;
    registry_free(&b->reg);
    registry_init(&b->reg);
    memset(&b->specialist, 0, sizeof b->specialist);
    b->specialist.kind = SPECIALIST_KIND_ORACLE;
    b->specialist.btn = &b->student;
    b->specialist.name = b->name;
    if (specialist_admit(&b->reg, &b->specialist, &c) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    b->certified = 1;
    b->state = CNET_CORE_BUS_CERTIFIED;
    return 0;
}

int cnet_core_bus_park_brick(CnetCoreBus *b, const char *domain_tag) {
    CnetCoreBrick *br;
    if (!b || b->state != CNET_CORE_BUS_CERTIFIED || !b->student_live ||
        !b->certified)
        return -1;
    if (b->wo.bound || b->wo.lease != 0) return -1;
    if (b->n_bricks >= CNET_CORE_BUS_MAX_BRICKS) return -1;
    br = &b->bricks[b->n_bricks];
    memset(br, 0, sizeof *br);
    copy_text(br->name, sizeof br->name, b->name);
    copy_text(br->domain_tag, sizeof br->domain_tag,
              domain_tag && domain_tag[0] ? domain_tag : b->domain_tag);
    memcpy(br->lut_table, b->lut_table, sizeof br->lut_table);
    /* Move student + its private registry into brick. */
    br->student = b->student;
    br->student.adapter_context = br;
    br->student.adapter_forward = brick_forward;
    br->reg = b->reg;
    /* Retarget single entry btn pointer into brick storage. */
    if (br->reg.count > 0 && br->reg.entries) {
        size_t i;
        for (i = 0; i < br->reg.count; ++i) {
            if (br->reg.entries[i].btn == &b->student)
                br->reg.entries[i].btn = &br->student;
        }
    }
    br->specialist = b->specialist;
    br->specialist.btn = &br->student;
    br->specialist.name = br->name;
    br->live = 1;
    br->certified = 1;
    {
        const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
        if (dir && dir[0])
            (void)cnet_serve_save_lut(dir, br->domain_tag, br->name, br->lut_table);
    }
    memset(&b->student, 0, sizeof b->student);
    memset(&b->reg, 0, sizeof b->reg);
    registry_init(&b->reg);
    memset(&b->specialist, 0, sizeof b->specialist);
    b->student_live = 0;
    b->lut_live = 0;
    b->certified = 0;
    b->state = CNET_CORE_BUS_IDLE;
    b->n_bricks++;
    cnet_weight_unmap(&b->file);
    return 0;
}

static int parse_turn(const char *turn, char *tag_out, size_t tag_cap,
                      unsigned *out_x) {
    const char *p = turn;
    unsigned v = 0;
    int saw = 0;
    char tag[32];
    size_t ti = 0;
    if (!turn || !out_x) return -1;
    while (*p == ' ' || *p == '\t') p++;
    /* optional domain tag token */
    if (isalpha((unsigned char)*p) || *p == '_') {
        while (*p && *p != ' ' && *p != '\t' && ti + 1 < sizeof tag) {
            tag[ti++] = *p++;
        }
        tag[ti] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (tag_out && tag_cap) copy_text(tag_out, tag_cap, tag);
    } else if (tag_out && tag_cap) {
        tag_out[0] = '\0';
    }
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return -1;
    while (isdigit((unsigned char)*p)) {
        saw = 1;
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 15u) return -1;
        p++;
    }
    if (!saw) return -1;
    *out_x = v;
    return 0;
}

static int prove_lut(float *lut, unsigned x, unsigned *y_out, double *got) {
    double in[U8W];
    size_t j;
    unsigned y = 0;
    LutServeCtx lc;
    lc.lut = lut;
    lc.live = 1;
    for (j = 0; j < U8W; ++j) {
        unsigned bit = (unsigned)(U8W - 1u - (unsigned)j);
        in[j] = (double)((x >> bit) & 1u);
    }
    if (lut_forward_ctx(&lc, in, U8W, got, U8W) != 0) return -1;
    for (j = 0; j < U8W; ++j) {
        if (got[j] > 0.5) y |= 1u << (unsigned)(U8W - 1u - (unsigned)j);
    }
    *y_out = y;
    return 0;
}

int cnet_core_bus_result(CnetCoreBus *b, const char *turn,
                         CnetCoreBusResult *out) {
    unsigned x = 0, y = 0;
    double got[U8W];
    char tag[32];
    int i;
    if (!b || !out) return -1;
    memset(out, 0, sizeof *out);
    b->open_chat_blocked++;
    b->residual_answer_blocked++;

    if (parse_turn(turn, tag, sizeof tag, &x) != 0) {
        out->abstained = 1;
        copy_text(out->refusal, sizeof out->refusal, "no_table_match");
        copy_text(out->spoken, sizeof out->spoken, "no_table_match");
        b->results_abstained++;
        return 1;
    }

    /* Prefer parked bricks matching domain tag (or any if tag empty). */
    for (i = 0; i < b->n_bricks; ++i) {
        CnetCoreBrick *br = &b->bricks[i];
        if (!br->live || !br->certified) continue;
        if (tag[0] && strcmp(tag, br->domain_tag) != 0) continue;
        if (prove_lut(br->lut_table, x, &y, got) != 0) continue;
        out->proved = 1;
        out->claimed_cert = 1;
        out->in_nibble = x;
        out->out_nibble = y;
        snprintf(out->spoken, sizeof out->spoken, "%u", y);
        copy_text(out->brick, sizeof out->brick, br->name);
        b->results_proved++;
        return 0;
    }

    /* Active CERTIFIED slot (not yet parked). */
    if (b->state == CNET_CORE_BUS_CERTIFIED && b->certified && b->student_live &&
        b->lut_live) {
        if (!tag[0] || strcmp(tag, b->domain_tag) == 0) {
            if (prove_lut(b->lut_table, x, &y, got) == 0) {
                out->proved = 1;
                out->claimed_cert = 1;
                out->in_nibble = x;
                out->out_nibble = y;
                snprintf(out->spoken, sizeof out->spoken, "%u", y);
                copy_text(out->brick, sizeof out->brick, b->name);
                b->results_proved++;
                return 0;
            }
        }
    }

    out->abstained = 1;
    out->claimed_cert = 0;
    copy_text(out->refusal, sizeof out->refusal, "outside_table_abstain");
    copy_text(out->spoken, sizeof out->spoken, "outside_table_abstain");
    b->results_abstained++;
    return 1;
}

/* ---- Bonsai Q1_0 → host-derived domain LUT GGUF ---- */

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

int cnet_core_bus_write_q1_domain_from_host_ex(const char *bonsai_gguf,
                                               const char *tensor_name,
                                               const char *out_gguf, int mode) {
    cce_gguf *g = NULL;
    const void *raw = NULL;
    size_t nbytes = 0;
    float lut[U8N];
    unsigned i;
    int idx;
    FILE *f;
    long pos;
    int pad;
    const char *tname = tensor_name && tensor_name[0] ? tensor_name
                                                      : "blk.0.attn_q.weight";

    if (!bonsai_gguf || !out_gguf) return -1;
    if (cce_gguf_load(bonsai_gguf, &g) != CCE_OK || !g) return -1;
    idx = cce_gguf_find_tensor(g, tname);
    if (idx < 0) {
        cce_gguf_free(g);
        return -1;
    }
    if (cce_gguf_tensor_bytes(g, idx, &raw, &nbytes) == CCE_OK && raw &&
        nbytes >= 18) {
        const uint8_t *blk = (const uint8_t *)raw;
        for (i = 0; i < U8N; ++i) {
            unsigned bit = (blk[2 + (i / 8u)] >> (i % 8u)) & 1u;
            if (mode == 1)
                lut[i] = (float)((i ^ bit) & 15u);
            else
                lut[i] = (float)((i + bit) & 15u);
        }
    } else {
        float head[128];
        if (cce_gguf_load_f32(g, idx, head, 128) != CCE_OK) {
            cce_gguf_free(g);
            return -1;
        }
        for (i = 0; i < U8N; ++i) {
            unsigned bit = head[i] >= 0.0f ? 1u : 0u;
            if (mode == 1)
                lut[i] = (float)((i ^ bit) & 15u);
            else
                lut[i] = (float)((i + bit) & 15u);
        }
    }
    cce_gguf_free(g);

    f = fopen(out_gguf, "wb");
    if (!f) return -1;
    fwrite("GGUF", 1, 4, f);
    wr_u32(f, 3);
    wr_u64(f, 1);
    wr_u64(f, 5);
    wr_kv_str(f, "general.architecture", "cnet_q1_domain");
    wr_kv_u32(f, "general.alignment", 32);
    wr_kv_str(f, "cnet.domain", mode == 1 ? "q1_xor16" : "q1_add16");
    wr_kv_str(f, "cnet.conversion", "bonsai_q1_host");
    wr_kv_u32(f, "cnet.combos", (uint32_t)U8N);
    wr_str(f, CNET_WEIGHT_LUT_TENSOR);
    wr_u32(f, 1);
    wr_u64(f, (uint64_t)U8N);
    wr_u32(f, 0);
    wr_u64(f, 0);
    pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return -1;
    }
    pad = (int)((32 - (unsigned)(pos % 32)) % 32);
    while (pad-- > 0) fputc(0, f);
    if (fwrite(lut, sizeof(float), U8N, f) != U8N) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

int cnet_core_bus_write_q1_domain_from_host(const char *bonsai_gguf,
                                            const char *tensor_name,
                                            const char *out_gguf) {
    return cnet_core_bus_write_q1_domain_from_host_ex(bonsai_gguf, tensor_name,
                                                      out_gguf, 0);
}

int cnet_core_bus_make_brick(CnetCoreBus *b, const char *bonsai_gguf,
                             const char *tensor_name, const char *domain_gguf,
                             const char *brick_name, const char *domain_tag,
                             int mode, CnetWeightConvertReport *rep) {
    CnetWeightConvertReport local;
    CnetWeightConvertReport *r = rep ? rep : &local;
    if (!b || !bonsai_gguf || !domain_gguf || !brick_name || !domain_tag)
        return -1;
    if (b->state != CNET_CORE_BUS_IDLE) return -11;
    if (cnet_core_bus_write_q1_domain_from_host_ex(bonsai_gguf, tensor_name,
                                                   domain_gguf, mode) != 0)
        return -12;
    copy_text(b->domain_tag, sizeof b->domain_tag, domain_tag);
    if (cnet_core_bus_lease(b, domain_gguf, brick_name) != 0) return -13;
    if (cnet_core_bus_table(b, r) != 0) return -14;
    if (cnet_core_bus_certify(b) != 0) return -15;
    if (cnet_core_bus_park_brick(b, domain_tag) != 0) return -16;
    return 0;
}

int cnet_core_bus_misslog_propose(const char *miss_path, char *name_out,
                                  size_t name_cap, CnetDcTerm *brick_out) {
    CnetDcGrammar g;
    CnetDcTerm brick;
    char name[64];
    int rc;
    int before;
    if (!miss_path) return -1;
    memset(&g, 0, sizeof g);
    cnet_dc_grammar_init(&g);
    memset(&brick, 0, sizeof brick);
    before = cnet_dc_extract_specialist_admit_calls();
    rc = cnet_dc_misslog_extract(&g, miss_path, name, sizeof name, &brick);
    if (cnet_dc_extract_specialist_admit_calls() != before) return -1;
    if (name_out && name_cap) copy_text(name_out, name_cap, name);
    if (brick_out) *brick_out = brick;
    return rc; /* 0 invent, 1 none, <0 error */
}

static CnetCoreBus g_bus;
static int g_bus_ready;

CnetCoreBus *cnet_core_bus_global(void) {
    return g_bus_ready ? &g_bus : NULL;
}

int cnet_core_bus_global_load_env(void) {
    const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    char path_a[512], path_b[512];
    CnetWeightConvertReport ra, rb;
    if (g_bus_ready) return 0;
    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";
    if (!dir || !dir[0]) return 1; /* not configured */
    snprintf(path_a, sizeof path_a, "%s/q1_add16_attn_q.gguf", dir);
    snprintf(path_b, sizeof path_b, "%s/q1_xor16_attn_k.gguf", dir);
    cnet_core_bus_init(&g_bus);
    memset(&ra, 0, sizeof ra);
    memset(&rb, 0, sizeof rb);
    if (cnet_core_bus_make_brick(&g_bus, bonsai, "blk.0.attn_q.weight", path_a,
                                 "brick_attn_q_add", "q1_add16", 0, &ra) != 0)
        return -1;
    if (cnet_core_bus_make_brick(&g_bus, bonsai, "blk.0.attn_k.weight", path_b,
                                 "brick_attn_k_xor", "q1_xor16", 1, &rb) != 0)
        return -1;
    g_bus_ready = 1;
    return 0;
}
