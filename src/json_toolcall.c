#include "../include/json_toolcall.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const TOOLS[CNET_JTC_N_TOOL] = {
    "calculator", "memory_store", "memory_recall",
    "file_read", "cnet_recall", "final"
};

/* Closed feature alphabet — host + C must stay in sync with .NET JsonToolCall.cs */
static const char *const FEATS[CNET_JTC_N_FEAT] = {
    "calculator", "memory_store", "memory_recall", "file_read",
    "cnet_recall", "final", "expr", "key", "value", "query",
    "path", "cond", "current", "answer", "tool", "args"
};

/* One exemplar JSON per tool (closed set — not free-form open JSON). */
static const char *const EXAMPLES[CNET_JTC_N_TOOL] = {
    "{\"tool\":\"calculator\",\"args\":{\"expr\":\"23 * 19\"}}",
    "{\"tool\":\"memory_store\",\"args\":{\"key\":\"k\",\"value\":\"v\"}}",
    "{\"tool\":\"memory_recall\",\"args\":{\"query\":\"k\"}}",
    "{\"tool\":\"file_read\",\"args\":{\"path\":\"readme.txt\"}}",
    "{\"tool\":\"cnet_recall\",\"args\":{\"cond\":0,\"current\":1}}",
    "{\"final\":\"done\",\"answer\":\"ok\"}"
};

const char *const *cnet_jtc_tool_names(void) { return TOOLS; }
const char *const *cnet_jtc_feature_names(void) { return FEATS; }

const char *cnet_jtc_example_json(int tool_id) {
    if (tool_id < 0 || tool_id >= CNET_JTC_N_TOOL) return NULL;
    return EXAMPLES[tool_id];
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
        feat_out[k] = ascii_ci_contains(json_text, FEATS[k]) ? 1.0 : 0.0;
    return 0;
}

int cnet_jtc_decode_tool(const double *tool_onehot) {
    int i, best = 0;
    if (!tool_onehot) return -1;
    for (i = 1; i < CNET_JTC_N_TOOL; i++)
        if (tool_onehot[i] > tool_onehot[best]) best = i;
    return best;
}

int cnet_jtc_hermetic_teacher(const double *in, double *out, void *ctx) {
    /* Priority: explicit tool-name features, then final, then arg cues. */
    int c;
    (void)ctx;
    if (!in || !out) return -1;
    for (c = 0; c < CNET_JTC_N_TOOL; c++) out[c] = 0.0;

    if (in[0] > 0.5) { out[0] = 1.0; return 0; } /* calculator */
    if (in[1] > 0.5) { out[1] = 1.0; return 0; } /* memory_store */
    if (in[2] > 0.5) { out[2] = 1.0; return 0; } /* memory_recall */
    if (in[3] > 0.5) { out[3] = 1.0; return 0; } /* file_read */
    if (in[4] > 0.5) { out[4] = 1.0; return 0; } /* cnet_recall */
    if (in[5] > 0.5 || in[13] > 0.5) { out[5] = 1.0; return 0; } /* final/answer */

    /* Soft cues without tool field */
    if (in[6] > 0.5) { out[0] = 1.0; return 0; }  /* expr → calculator */
    if (in[7] > 0.5 && in[8] > 0.5) { out[1] = 1.0; return 0; } /* key+value */
    if (in[9] > 0.5) { out[2] = 1.0; return 0; }  /* query → recall */
    if (in[10] > 0.5) { out[3] = 1.0; return 0; } /* path → file */
    if (in[11] > 0.5 || in[12] > 0.5) { out[4] = 1.0; return 0; } /* cond/current */

    out[5] = 1.0; /* default final */
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
    double inputs[CNET_JTC_N_TOOL * CNET_JTC_N_FEAT];
    double targets[CNET_JTC_N_TOOL * CNET_JTC_N_TOOL];
    int c, j;

    if (!reg || !student_out) return -1;
    t = teacher_out ? teacher_out : &local;
    external_teacher_init(t);

    for (c = 0; c < CNET_JTC_N_TOOL; c++) {
        cnet_jtc_encode(EXAMPLES[c], inputs + c * CNET_JTC_N_FEAT);
        for (j = 0; j < CNET_JTC_N_TOOL; j++)
            targets[c * CNET_JTC_N_TOOL + j] = (j == c) ? 1.0 : 0.0;
    }

    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = identity_digest_salt
        ? identity_digest_salt
        : 0x4A54435F56300001ULL; /* JTC_V0 */
    id.contract_digest = 0x4A534F4E5F5430ULL; /* JSON_T0 */
    id.config_digest = (uint64_t)CNET_JTC_N_TOOL << 32 |
                       (uint64_t)CNET_JTC_N_FEAT;

    if (external_teacher_bind_callback(
            t, CNET_MODALITY_TEXT, "json_toolcall_hermetic_v0",
            cnet_jtc_input_port(), cnet_jtc_output_port(),
            cnet_jtc_hermetic_teacher, NULL, &id, 0) != 0)
        return -2;

    if (external_teacher_mine_admit(
            t, reg, inputs, targets, CNET_JTC_N_TOOL, 16, 64, 15000, 4242u,
            CNET_JTC_UNIT_NAME, student_out) != 0) {
        if (!teacher_out) external_teacher_unbind(t);
        return 1;
    }
    if (!teacher_out) external_teacher_unbind(t);
    return 0;
}
