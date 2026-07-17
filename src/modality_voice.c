#include "../include/modality_voice.h"

#include <stdio.h>
#include <string.h>

static const char *const VOICE_CMDS[CNET_VOICE_N_CMD] = {
    "yes", "no", "up", "down", "left", "right", "on", "off", "stop", "go"
};

const char *const *cnet_voice_command_names(void) { return VOICE_CMDS; }

void cnet_voice_hermetic_features(int class_id, double *out_feat) {
    unsigned h = 2166136261u;
    int k;
    const char *name;
    if (!out_feat) return;
    if (class_id < 0 || class_id >= CNET_VOICE_N_CMD) class_id = 0;
    name = VOICE_CMDS[class_id];
    for (; *name; name++) {
        h ^= (unsigned char)*name;
        h *= 16777619u;
    }
    h ^= (unsigned)class_id * 0x9e3779b9u;
    for (k = 0; k < CNET_VOICE_FEAT_DIM; k++) {
        int bit = (h >> (k % 32)) & 1;
        /* soft binary features — stand-in for a frozen audio encoder */
        out_feat[k] = bit ? 0.85 : 0.15;
        h = h * 1664525u + 1013904223u;
    }
}

int cnet_voice_hermetic_teacher(const double *in, double *out, void *ctx) {
    int c, best = 0;
    double best_d = 1e300;
    double feat[CNET_VOICE_FEAT_DIM];
    (void)ctx;
    if (!in || !out) return -1;
    for (c = 0; c < CNET_VOICE_N_CMD; c++) {
        int k;
        double d = 0.0;
        cnet_voice_hermetic_features(c, feat);
        for (k = 0; k < CNET_VOICE_FEAT_DIM; k++) {
            double e = in[k] - feat[k];
            d += e * e;
        }
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    for (c = 0; c < CNET_VOICE_N_CMD; c++) out[c] = 0.0;
    out[best] = 1.0;
    return 0;
}

Port cnet_voice_input_port(void) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_RAW;
    p.field_width = CNET_VOICE_FEAT_DIM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "speech_feat");
    return p;
}

Port cnet_voice_output_port(void) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = CNET_VOICE_N_CMD;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "speech_cmd");
    return p;
}

int cnet_voice_v0_mine_admit(
    PrimitiveRegistry *reg,
    uint64_t identity_digest_salt,
    BinaryTransformNetwork **student_out,
    ExternalTeacher *teacher_out)
{
    ExternalTeacher local, *t;
    CnetOracleIdentity id;
    double inputs[CNET_VOICE_N_CMD * CNET_VOICE_FEAT_DIM];
    double targets[CNET_VOICE_N_CMD * CNET_VOICE_N_CMD];
    int c, j;

    if (!reg || !student_out) return -1;
    t = teacher_out ? teacher_out : &local;
    external_teacher_init(t);

    for (c = 0; c < CNET_VOICE_N_CMD; c++) {
        cnet_voice_hermetic_features(c, inputs + c * CNET_VOICE_FEAT_DIM);
        for (j = 0; j < CNET_VOICE_N_CMD; j++)
            targets[c * CNET_VOICE_N_CMD + j] = (j == c) ? 1.0 : 0.0;
    }

    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = identity_digest_salt
        ? identity_digest_salt
        : 0x564f4943455f5630ULL; /* "VOICE_V0" */
    id.contract_digest = 0x5350454543485f43ULL; /* "SPEECH_C" */
    id.config_digest = (uint64_t)CNET_VOICE_N_CMD << 32 |
                       (uint64_t)CNET_VOICE_FEAT_DIM;

    if (external_teacher_bind_callback(
            t, CNET_MODALITY_VOICE, "voice_hermetic_v0",
            cnet_voice_input_port(), cnet_voice_output_port(),
            cnet_voice_hermetic_teacher, NULL, &id, 0) != 0)
        return -2;

    if (external_teacher_mine_admit(
            t, reg, inputs, targets, CNET_VOICE_N_CMD, 16, 64, 15000, 4242u,
            "voice_cmd_v0", student_out) != 0) {
        if (!teacher_out) external_teacher_unbind(t);
        return 1;
    }
    if (!teacher_out) external_teacher_unbind(t);
    return 0;
}
