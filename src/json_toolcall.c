#include "../include/json_toolcall.h"
#include "../include/json_toolcall_alphabet.inc"
#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CNET_JTC_N_TOOL_GEN != CNET_JTC_N_TOOL
#error "json_toolcall_alphabet.inc N_TOOL mismatch — re-run tools/gen_json_toolcall_alphabet.py"
#endif
#if CNET_JTC_N_FEAT_GEN != CNET_JTC_N_FEAT
#error "json_toolcall_alphabet.inc N_FEAT mismatch — re-run tools/gen_json_toolcall_alphabet.py"
#endif

const char *const *cnet_jtc_tool_names(void) { return CNET_JTC_TOOLS_GEN; }
const char *const *cnet_jtc_feature_names(void) { return CNET_JTC_FEATS_GEN; }

const char *cnet_jtc_example_json(int tool_id) {
    if (tool_id < 0 || tool_id >= CNET_JTC_N_TOOL) return NULL;
    return CNET_JTC_EXAMPLES_GEN[tool_id];
}

static int ascii_ci_contains(const char *hay, const char *needle) {
    size_t nlen, hlen, i, j;
    if (!hay || !needle || !needle[0]) return 0;
    nlen = strlen(needle);
    hlen = strlen(hay);
    if (nlen > hlen) return 0;
    for (i = 0; i + nlen <= hlen; i++) {
        for (j = 0; j < nlen; j++) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (tolower(a) != tolower(b)) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

int cnet_jtc_encode(const char *json_text, double *feat_out) {
    int k;
    if (!feat_out) return -1;
    if (!json_text) json_text = "";
    for (k = 0; k < CNET_JTC_N_FEAT; k++)
        feat_out[k] = ascii_ci_contains(json_text, CNET_JTC_FEATS_GEN[k]) ? 1.0 : 0.0;
    return 0;
}

int cnet_jtc_decode_tool(const double *tool_onehot) {
    int i, best = 0;
    if (!tool_onehot) return -1;
    for (i = 1; i < CNET_JTC_N_TOOL; i++)
        if (tool_onehot[i] > tool_onehot[best]) best = i;
    return best;
}

static int jtc_feat_idx(const char *name) {
    int i;
    if (!name) return -1;
    for (i = 0; i < CNET_JTC_N_FEAT; i++)
        if (strcmp(CNET_JTC_FEATS_GEN[i], name) == 0) return i;
    return -1;
}

static int jtc_tool_idx(const char *name) {
    int i;
    if (!name) return -1;
    for (i = 0; i < CNET_JTC_N_TOOL; i++)
        if (strcmp(CNET_JTC_TOOLS_GEN[i], name) == 0) return i;
    return -1;
}

static void jtc_onehot(double *out, int tool_id) {
    int c;
    for (c = 0; c < CNET_JTC_N_TOOL; c++) out[c] = 0.0;
    if (tool_id >= 0 && tool_id < CNET_JTC_N_TOOL) out[tool_id] = 1.0;
}

int cnet_jtc_hermetic_teacher(const double *in, double *out, void *ctx) {
    int t, fi, final_id;
    int f_expr, f_key, f_value, f_query, f_path, f_cond, f_current, f_answer;
    (void)ctx;
    if (!in || !out) return -1;
    for (t = 0; t < CNET_JTC_N_TOOL; t++) out[t] = 0.0;

    /* Prefer explicit tool-name features (alphabet order; final last). */
    for (t = 0; t < CNET_JTC_N_TOOL; t++) {
        fi = jtc_feat_idx(CNET_JTC_TOOLS_GEN[t]);
        if (fi >= 0 && in[fi] > 0.5) {
            jtc_onehot(out, t);
            return 0;
        }
    }

    /* Arg-keyword heuristics when the host JSON omitted a tool name. */
    f_expr = jtc_feat_idx("expr");
    f_key = jtc_feat_idx("key");
    f_value = jtc_feat_idx("value");
    f_query = jtc_feat_idx("query");
    f_path = jtc_feat_idx("path");
    f_cond = jtc_feat_idx("cond");
    f_current = jtc_feat_idx("current");
    f_answer = jtc_feat_idx("answer");

    if (f_expr >= 0 && in[f_expr] > 0.5) {
        jtc_onehot(out, jtc_tool_idx("calculator"));
        return 0;
    }
    if (f_key >= 0 && f_value >= 0 && in[f_key] > 0.5 && in[f_value] > 0.5) {
        jtc_onehot(out, jtc_tool_idx("memory_store"));
        return 0;
    }
    if (f_path >= 0 && in[f_path] > 0.5) {
        jtc_onehot(out, jtc_tool_idx("file_read"));
        return 0;
    }
    if ((f_cond >= 0 && in[f_cond] > 0.5) ||
        (f_current >= 0 && in[f_current] > 0.5)) {
        jtc_onehot(out, jtc_tool_idx("cnet_recall"));
        return 0;
    }
    if (f_query >= 0 && in[f_query] > 0.5) {
        /* Ambiguous query: default to local memory_recall (not network). */
        jtc_onehot(out, jtc_tool_idx("memory_recall"));
        return 0;
    }
    if (f_answer >= 0 && in[f_answer] > 0.5) {
        jtc_onehot(out, jtc_tool_idx("final"));
        return 0;
    }

    final_id = jtc_tool_idx("final");
    jtc_onehot(out, final_id >= 0 ? final_id : CNET_JTC_N_TOOL - 1);
    return 0;
}

Port cnet_jtc_input_port(void) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_RAW;
    p.field_width = CNET_JTC_N_FEAT;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "jtc_feat");
    return p;
}

Port cnet_jtc_output_port(void) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = CNET_JTC_N_TOOL;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "json_tool");
    return p;
}

int cnet_jtc_v0_mine_admit(
    PrimitiveRegistry *reg,
    uint64_t identity_digest_salt,
    BinaryTransformNetwork **student_out,
    ExternalTeacher *teacher_out)
{
    ExternalTeacher local, *t;
    CnetOracleIdentity id;
    enum { JTC_TRAIN_EXTRA = 7, JTC_TRAIN_N = CNET_JTC_N_TOOL + JTC_TRAIN_EXTRA };
    static const char *const heuristic_training[JTC_TRAIN_EXTRA] = {
        "{\"args\":{\"expr\":\"6*7\"}}",
        "{\"args\":{\"key\":\"alpha\",\"value\":\"beta\"}}",
        "{\"args\":{\"path\":\"notes.txt\"}}",
        "{\"args\":{\"cond\":0,\"current\":2}}",
        "{\"args\":{\"query\":\"local recollection\"}}",
        "{\"answer\":\"complete response\"}",
        "{\"tool\":\"memory_recall\",\"args\":{\"key\":\"saved preference\"}}"
    };
    double inputs[JTC_TRAIN_N * CNET_JTC_N_FEAT];
    double targets[JTC_TRAIN_N * CNET_JTC_N_TOOL];
    int c, j;

    if (!reg || !student_out) return -1;
    t = teacher_out ? teacher_out : &local;
    external_teacher_init(t);

    for (c = 0; c < CNET_JTC_N_TOOL; c++) {
        cnet_jtc_encode(CNET_JTC_EXAMPLES_GEN[c], inputs + c * CNET_JTC_N_FEAT);
        for (j = 0; j < CNET_JTC_N_TOOL; j++)
            targets[c * CNET_JTC_N_TOOL + j] = (j == c) ? 1.0 : 0.0;
    }
    for (c = 0; c < JTC_TRAIN_EXTRA; ++c) {
        double *sample = inputs + (CNET_JTC_N_TOOL + c) * CNET_JTC_N_FEAT;
        double *target = targets + (CNET_JTC_N_TOOL + c) * CNET_JTC_N_TOOL;
        cnet_jtc_encode(heuristic_training[c], sample);
        if (cnet_jtc_hermetic_teacher(sample, target, NULL) != 0) return -2;
    }

    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = identity_digest_salt
        ? identity_digest_salt
        : 0x4A54435F56300001ULL;
    id.contract_digest = 0x4A534F4E5F5430ULL;
    id.config_digest = (uint64_t)JTC_TRAIN_N << 48 |
                       (uint64_t)CNET_JTC_N_TOOL << 32 |
                       (uint64_t)CNET_JTC_N_FEAT;

    if (external_teacher_bind_callback(
            t, CNET_MODALITY_TEXT, "json_toolcall_hermetic_v0",
            cnet_jtc_input_port(), cnet_jtc_output_port(),
            cnet_jtc_hermetic_teacher, NULL, &id, 0) != 0)
        return -2;

    if (external_teacher_mine_admit(
            t, reg, inputs, targets, JTC_TRAIN_N, 24, 64, 30000, 4242u,
            CNET_JTC_UNIT_NAME, student_out) != 0) {
        if (!teacher_out) external_teacher_unbind(t);
        return 1;
    }
    if (!teacher_out) external_teacher_unbind(t);
    return 0;
}

int cnet_jtc_ports_match(Port in, Port goal) {
    if (in.family != PORT_RAW || in.field_width != (size_t)CNET_JTC_N_FEAT ||
        in.field_count != 1)
        return 0;
    if (goal.family != PORT_ONEHOT ||
        goal.field_width != (size_t)CNET_JTC_N_TOOL || goal.field_count != 1)
        return 0;
    /* Tags optional but when set must match spine. */
    if (in.tag[0] && strcmp(in.tag, "jtc_feat") != 0) return 0;
    if (goal.tag[0] && strcmp(goal.tag, "json_tool") != 0) return 0;
    return 1;
}

int cnet_jtc_ensure_sealed(CnetBase *base, PrimitiveRegistry *reg) {
    BinaryTransformNetwork *student = NULL;
    PrimitiveRegistry local_reg, *r;
    Contract c;
    double inputs[CNET_JTC_N_TOOL * CNET_JTC_N_FEAT];
    double targets[CNET_JTC_N_TOOL * CNET_JTC_N_TOOL];
    int ti, j, reused = 0, rc;
    int own_reg = 0;

    if (!base) return -1;
    if (cnb_has_unit(base, CNET_JTC_UNIT_NAME))
        return 1;

    r = reg;
    if (!r) {
        registry_init(&local_reg);
        r = &local_reg;
        own_reg = 1;
    }

    if (cnet_jtc_v0_mine_admit(r, 0x4A54435F54454143ULL, &student, NULL) != 0 ||
        !student) {
        if (own_reg) registry_free(&local_reg);
        return -2;
    }

    for (ti = 0; ti < CNET_JTC_N_TOOL; ti++) {
        cnet_jtc_encode(cnet_jtc_example_json(ti),
                        inputs + ti * CNET_JTC_N_FEAT);
        for (j = 0; j < CNET_JTC_N_TOOL; j++)
            targets[ti * CNET_JTC_N_TOOL + j] = (j == ti) ? 1.0 : 0.0;
    }
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, CNET_JTC_UNIT_NAME, student, inputs, targets,
                               CNET_JTC_N_TOOL) != 0 ||
        btn_certify(student, &c, NULL) != 0) {
        contract_free(&c);
        if (own_reg) registry_free(&local_reg);
        return -3;
    }
    rc = cnb_add_unit(base, student, &c, &reused);
    contract_free(&c);
    if (rc != 0) {
        if (own_reg) {
            registry_free(&local_reg);
        }
        return -4;
    }
    /* Base owns sealed blob. Free heap student only when we own the reg
       (mine_admit otherwise leaves student borrowed by caller's reg). */
    if (own_reg) {
        btn_free(student);
        free(student);
        registry_free(&local_reg);
    } else {
        /* Keep student alive for reg entry; base has independent copy. */
        (void)student;
    }
    (void)reused;
    return 0;
}
