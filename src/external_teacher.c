#include "../include/external_teacher.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double *inputs;
    double *targets;
    size_t n_rows;
    size_t in_dim;
    size_t out_dim;
    int onehot_in;
} ExtTableCtx;

static size_t port_total(Port p) {
    return p.field_width * p.field_count;
}

static int argmax_first(const double *v, size_t n) {
    size_t i, b = 0;
    for (i = 1; i < n; i++)
        if (v[i] > v[b]) b = i;
    return (int)b;
}

static int table_teacher_fn(const double *in, double *out, void *ctx) {
    ExtTableCtx *t = (ExtTableCtx *)ctx;
    size_t r, best = 0;
    double best_d = 1e300;
    if (!t || !in || !out) return -1;
    if (t->onehot_in) {
        int hot = argmax_first(in, t->in_dim);
        if (hot < 0 || (size_t)hot >= t->n_rows) return 1;
        memcpy(out, t->targets + (size_t)hot * t->out_dim,
               t->out_dim * sizeof(double));
        return 0;
    }
    for (r = 0; r < t->n_rows; r++) {
        size_t j;
        double d = 0.0;
        const double *row = t->inputs + r * t->in_dim;
        for (j = 0; j < t->in_dim; j++) {
            double e = in[j] - row[j];
            d += e * e;
        }
        if (d < best_d) {
            best_d = d;
            best = r;
        }
    }
    memcpy(out, t->targets + best * t->out_dim, t->out_dim * sizeof(double));
    return 0;
}

int external_teacher_init(ExternalTeacher *t) {
    if (!t) return -1;
    memset(t, 0, sizeof *t);
    t->abi_version = CNET_EXT_TEACHER_ABI;
    t->struct_size = (uint32_t)sizeof *t;
    return 0;
}

const char *cnet_modality_name(CnetModality m) {
    switch (m) {
    case CNET_MODALITY_TEXT: return "text";
    case CNET_MODALITY_VOICE: return "voice";
    case CNET_MODALITY_VISION: return "vision";
    default: return "unknown";
    }
}

int external_teacher_bind_callback(
    ExternalTeacher *t,
    CnetModality modality,
    const char *name,
    Port input_port,
    Port output_port,
    CnetOracleFn fn,
    void *ctx,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest)
{
    if (!t || !name || !name[0] || !fn || !identity) return -1;
    if (identity->artifact_digest == 0) return -2;
    external_teacher_unbind(t);
    external_teacher_init(t);
    t->modality = modality;
    snprintf(t->name, sizeof t->name, "%s", name);
    snprintf(t->kind, sizeof t->kind, "callback");
    t->input_port = input_port;
    t->output_port = output_port;
    t->fn = fn;
    t->ctx = ctx;
    t->identity = *identity;
    if (t->identity.abi_version == 0)
        t->identity.abi_version = CNET_ORACLE_ABI_VERSION;
    if (t->identity.struct_size == 0)
        t->identity.struct_size = (uint32_t)sizeof t->identity;
    t->behavior_digest = behavior_digest
        ? behavior_digest
        : cnet_oracle_identity_digest(&t->identity);
    if (t->behavior_digest == 0)
        t->behavior_digest = identity->artifact_digest;
    t->bound = 1;
    return 0;
}

int external_teacher_bind_table(
    ExternalTeacher *t,
    CnetModality modality,
    const char *name,
    Port input_port,
    Port output_port,
    const double *inputs,
    const double *targets,
    size_t n_rows,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest)
{
    ExtTableCtx *tc;
    size_t in_dim, out_dim;
    if (!t || !name || !inputs || !targets || n_rows == 0 || !identity)
        return -1;
    if (identity->artifact_digest == 0) return -2;
    in_dim = port_total(input_port);
    out_dim = port_total(output_port);
    if (in_dim == 0 || out_dim == 0) return -3;
    tc = (ExtTableCtx *)calloc(1, sizeof *tc);
    if (!tc) return -4;
    tc->inputs = (double *)malloc(n_rows * in_dim * sizeof(double));
    tc->targets = (double *)malloc(n_rows * out_dim * sizeof(double));
    if (!tc->inputs || !tc->targets) {
        free(tc->inputs);
        free(tc->targets);
        free(tc);
        return -4;
    }
    memcpy(tc->inputs, inputs, n_rows * in_dim * sizeof(double));
    memcpy(tc->targets, targets, n_rows * out_dim * sizeof(double));
    tc->n_rows = n_rows;
    tc->in_dim = in_dim;
    tc->out_dim = out_dim;
    tc->onehot_in = (input_port.family == PORT_ONEHOT);
    if (external_teacher_bind_callback(t, modality, name, input_port,
                                       output_port, table_teacher_fn, tc,
                                       identity, behavior_digest) != 0) {
        free(tc->inputs);
        free(tc->targets);
        free(tc);
        return -5;
    }
    snprintf(t->kind, sizeof t->kind, "table");
    return 0;
}

void external_teacher_unbind(ExternalTeacher *t) {
    if (!t) return;
    if (t->bound && strcmp(t->kind, "table") == 0 && t->ctx) {
        ExtTableCtx *tc = (ExtTableCtx *)t->ctx;
        free(tc->inputs);
        free(tc->targets);
        free(tc);
        t->ctx = NULL;
    }
    memset(t, 0, sizeof *t);
}

int external_teacher_to_oracle(const ExternalTeacher *t, OracleEntry *out) {
    if (!t || !t->bound || !out) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", t->name);
    out->input_port = t->input_port;
    out->output_port = t->output_port;
    out->fn = t->fn;
    out->ctx = t->ctx;
    out->identity = t->identity;
    out->behavior_digest = t->behavior_digest;
    return 0;
}

int external_teacher_register(OracleRegistry *oracles, ExternalTeacher *t) {
    OracleEntry *oe;
    if (!oracles || !t || !t->bound) return -1;
    if (acquire_oracle_register(oracles, t->name, t->input_port, t->output_port,
                                t->fn, t->ctx) != 0)
        return -2;
    oe = &oracles->entries[oracles->count - 1];
    oe->identity = t->identity;
    oe->behavior_digest = t->behavior_digest;
    return 0;
}

int external_teacher_admit_oracle(
    ExternalTeacher *t,
    PrimitiveRegistry *reg,
    const Contract *contract,
    BinaryTransformNetwork *adapter_out,
    Specialist *spec_out)
{
    OracleEntry entry;
    BinaryTransformNetwork adapter;
    Specialist spec;
    if (!t || !t->bound || !reg || !contract) return -1;
    if (external_teacher_to_oracle(t, &entry) != 0) return -2;
    memset(&adapter, 0, sizeof adapter);
    memset(&spec, 0, sizeof spec);
    if (specialist_wrap_oracle(&spec, &adapter, &entry, t->behavior_digest, 1,
                               t->name) != 0)
        return -3;
    if (specialist_admit(reg, &spec, contract) != 0) {
        btn_free(&adapter);
        return -4;
    }
    if (adapter_out) *adapter_out = adapter;
    else btn_free(&adapter);
    if (spec_out) *spec_out = spec;
    return 0;
}

int external_teacher_mine_admit(
    ExternalTeacher *t,
    PrimitiveRegistry *reg,
    const double *probe_inputs,
    const double *canonical_targets,
    size_t n_rows,
    size_t init_hidden,
    size_t max_hidden,
    size_t max_epochs,
    unsigned int seed,
    const char *unit_name,
    BinaryTransformNetwork **student_out)
{
    size_t in_dim, out_dim, i;
    double *labeled = NULL;
    BinaryTransformNetwork *student = NULL;
    Contract c;
    Specialist s;

    if (!t || !t->bound || !reg || !probe_inputs || !canonical_targets ||
        n_rows == 0 || !unit_name || !unit_name[0] || !student_out)
        return -1;
    in_dim = port_total(t->input_port);
    out_dim = port_total(t->output_port);
    if (in_dim == 0 || out_dim == 0) return -2;

    labeled = (double *)malloc(n_rows * out_dim * sizeof(double));
    if (!labeled) return -3;
    for (i = 0; i < n_rows; i++) {
        int rc = t->fn(probe_inputs + i * in_dim, labeled + i * out_dim,
                       t->ctx);
        if (rc != 0) {
            memcpy(labeled + i * out_dim, canonical_targets + i * out_dim,
                   out_dim * sizeof(double));
        }
    }

    student = (BinaryTransformNetwork *)calloc(1, sizeof *student);
    if (!student) {
        free(labeled);
        return -3;
    }
    if (btn_init(student, in_dim, out_dim, init_hidden ? init_hidden : 16,
                 max_hidden ? max_hidden : 64, 0.5, seed ? seed : 42) != 0) {
        free(student);
        free(labeled);
        return -4;
    }
    if (btn_set_ports(student, t->input_port, t->output_port) != 0) {
        btn_free(student);
        free(student);
        free(labeled);
        return -4;
    }
    (void)btn_train_dynamic(student, probe_inputs, labeled, n_rows,
                            max_epochs ? max_epochs : 12000, 200, 1e-6, 1e-8);
    (void)btn_train(student, probe_inputs, canonical_targets, n_rows, 4000);

    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, unit_name, student, probe_inputs,
                               canonical_targets, n_rows) != 0) {
        btn_free(student);
        free(student);
        free(labeled);
        return 1;
    }
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, student, unit_name) != 0 ||
        specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        btn_free(student);
        free(student);
        free(labeled);
        return 1;
    }
    contract_free(&c);
    free(labeled);
    *student_out = student;
    return 0;
}
