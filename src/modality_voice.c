#include "../include/modality_voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const VOICE_CMDS[CNET_VOICE_N_CMD] = {
    "yes", "no", "up", "down", "left", "right", "on", "off", "stop", "go"
};

/* Precomputed once — teacher no longer re-hashes features every class probe. */
static double g_voice_feat_tab[CNET_VOICE_N_CMD][CNET_VOICE_FEAT_DIM];
static int g_voice_feat_ready;

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

static void voice_feat_table_ensure(void) {
    int c;
    if (g_voice_feat_ready) return;
    for (c = 0; c < CNET_VOICE_N_CMD; c++)
        cnet_voice_hermetic_features(c, g_voice_feat_tab[c]);
    g_voice_feat_ready = 1;
}

int cnet_voice_hermetic_teacher(const double *in, double *out, void *ctx) {
    int c, best = 0;
    double best_d = 1e300;
    (void)ctx;
    if (!in || !out) return -1;
    voice_feat_table_ensure();
    for (c = 0; c < CNET_VOICE_N_CMD; c++) {
        int k;
        double d = 0.0;
        const double *feat = g_voice_feat_tab[c];
        for (k = 0; k < CNET_VOICE_FEAT_DIM; k++) {
            double e = in[k] - feat[k];
            d += e * e;
        }
        if (d < best_d) {
            best_d = d;
            best = c;
            if (d == 0.0) break; /* exact hermetic match */
        }
    }
    memset(out, 0, (size_t)CNET_VOICE_N_CMD * sizeof(double));
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

static uint64_t parse_u64_env(const char *s) {
    unsigned long long v = 0;
    if (!s || !s[0]) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        v = strtoull(s, NULL, 16);
    else
        v = strtoull(s, NULL, 10);
    return (uint64_t)v;
}

/* Best-effort: first path-like token ending in .py for digest. */
static uint64_t digest_from_cmdline(const char *cmdline) {
    char buf[1024];
    char *tok, *save = NULL;
    if (!cmdline) return 0;
    snprintf(buf, sizeof buf, "%s", cmdline);
    for (tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        size_t n = strlen(tok);
        if (n > 3 && strcmp(tok + n - 3, ".py") == 0) {
            uint64_t d = external_teacher_file_digest(tok);
            if (d) return d;
        }
    }
    /* FNV of cmdline itself when script path not found */
    {
        uint64_t h = 14695981039346656037ULL;
        const unsigned char *p = (const unsigned char *)cmdline;
        for (; *p; p++) {
            h ^= (uint64_t)*p;
            h *= 1099511628211ULL;
        }
        return h ? h : 1ULL;
    }
}

int cnet_voice_bind_subprocess(
    ExternalTeacher *t,
    const char *name,
    const char *cmdline,
    uint64_t identity_digest_salt)
{
    CnetOracleIdentity id;
    uint64_t dig;
    if (!t || !cmdline || !cmdline[0]) return -1;
    dig = identity_digest_salt ? identity_digest_salt
                               : digest_from_cmdline(cmdline);
    if (!dig) dig = 0x564f4943455f5254ULL; /* "VOICE_RT" */
    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = dig;
    id.contract_digest = 0x5350454543485f52ULL; /* SPEECH_R */
    id.config_digest = (uint64_t)CNET_VOICE_N_CMD << 32 |
                       (uint64_t)CNET_VOICE_FEAT_DIM;
    return external_teacher_bind_subprocess(
        t, CNET_MODALITY_VOICE,
        name && name[0] ? name : "voice_subprocess",
        cnet_voice_input_port(), cnet_voice_output_port(),
        cmdline, &id, 0);
}

int cnet_voice_bind_from_env(ExternalTeacher *t) {
    const char *cmd = getenv("CNET_VOICE_TEACHER_CMD");
    const char *name = getenv("CNET_VOICE_TEACHER_NAME");
    const char *digs = getenv("CNET_VOICE_TEACHER_DIGEST");
    uint64_t dig = 0;
    if (!t) return -1;
    if (!cmd || !cmd[0]) return -2;
    dig = parse_u64_env(digs);
    return cnet_voice_bind_subprocess(t, name, cmd, dig);
}

int cnet_voice_mine_admit_teacher(
    ExternalTeacher *t,
    PrimitiveRegistry *reg,
    const char *unit_name,
    BinaryTransformNetwork **student_out)
{
    double inputs[CNET_VOICE_N_CMD * CNET_VOICE_FEAT_DIM];
    double targets[CNET_VOICE_N_CMD * CNET_VOICE_N_CMD];
    int c, j;
    if (!t || !t->bound || !reg || !student_out) return -1;
    for (c = 0; c < CNET_VOICE_N_CMD; c++) {
        cnet_voice_hermetic_features(c, inputs + c * CNET_VOICE_FEAT_DIM);
        for (j = 0; j < CNET_VOICE_N_CMD; j++)
            targets[c * CNET_VOICE_N_CMD + j] = (j == c) ? 1.0 : 0.0;
    }
    return external_teacher_mine_admit(
        t, reg, inputs, targets, CNET_VOICE_N_CMD, 16, 64, 15000, 4242u,
        unit_name && unit_name[0] ? unit_name : "voice_cmd_v0", student_out);
}
