#include "../include/external_teacher.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef struct {
    double *inputs;
    double *targets;
    size_t n_rows;
    size_t in_dim;
    size_t out_dim;
    int onehot_in;
} ExtTableCtx;

typedef struct {
    char *cmdline;
    size_t in_dim;
    size_t out_dim;
    FILE *to_child;   /* parent writes stdin of child */
    FILE *from_child; /* parent reads stdout of child */
    pid_t pid;
    int started;
} ExtSubprocessCtx;

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

static void subprocess_stop(ExtSubprocessCtx *s) {
    if (!s) return;
    if (s->to_child) {
        fclose(s->to_child);
        s->to_child = NULL;
    }
    if (s->from_child) {
        fclose(s->from_child);
        s->from_child = NULL;
    }
    if (s->started && s->pid > 0) {
        int st = 0;
        (void)waitpid(s->pid, &st, 0);
        s->pid = 0;
        s->started = 0;
    }
}

static int subprocess_start(ExtSubprocessCtx *s) {
    int in_pipe[2], out_pipe[2];
    pid_t pid;
    if (!s || !s->cmdline || !s->cmdline[0]) return -1;
    if (s->started) return 0;
    if (pipe(in_pipe) != 0) return -2;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -2;
    }
    pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -3;
    }
    if (pid == 0) {
        /* child: stdin = in_pipe[0], stdout = out_pipe[1] */
        close(in_pipe[1]);
        close(out_pipe[0]);
        if (dup2(in_pipe[0], STDIN_FILENO) < 0) _exit(127);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        close(in_pipe[0]);
        close(out_pipe[1]);
        execl("/bin/sh", "sh", "-c", s->cmdline, (char *)NULL);
        _exit(127);
    }
    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    s->to_child = fdopen(in_pipe[1], "w");
    s->from_child = fdopen(out_pipe[0], "r");
    if (!s->to_child || !s->from_child) {
        subprocess_stop(s);
        return -4;
    }
    setvbuf(s->to_child, NULL, _IOLBF, 0);
    setvbuf(s->from_child, NULL, _IOLBF, 0);
    s->pid = pid;
    s->started = 1;
    return 0;
}

static int subprocess_teacher_fn(const double *in, double *out, void *ctx) {
    ExtSubprocessCtx *s = (ExtSubprocessCtx *)ctx;
    char line[65536];
    size_t j;
    unsigned long declared = 0;
    char *tok;
    char *save = NULL;

    if (!s || !in || !out) return -1;
    if (subprocess_start(s) != 0) return -2;

    if (fprintf(s->to_child, "IN %zu", s->in_dim) < 0) return -3;
    for (j = 0; j < s->in_dim; j++) {
        if (fprintf(s->to_child, " %.17g", in[j]) < 0) return -3;
    }
    if (fprintf(s->to_child, "\n") < 0 || fflush(s->to_child) != 0)
        return -3;

    if (!fgets(line, (int)sizeof line, s->from_child)) return -4;
    tok = strtok_r(line, " \t\r\n", &save);
    if (!tok || strcmp(tok, "OUT") != 0) return -5;
    tok = strtok_r(NULL, " \t\r\n", &save);
    if (!tok) return -5;
    declared = strtoul(tok, NULL, 10);
    if (declared != (unsigned long)s->out_dim) return -6;
    for (j = 0; j < s->out_dim; j++) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        if (!tok) return -7;
        out[j] = strtod(tok, NULL);
    }
    return 0;
}

uint64_t external_teacher_file_digest(const char *path) {
    FILE *f;
    uint64_t h = 14695981039346656037ULL;
    unsigned char buf[4096];
    size_t n;
    if (!path || !path[0]) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            h ^= (uint64_t)buf[i];
            h *= 1099511628211ULL;
        }
    }
    fclose(f);
    return h ? h : 1ULL; /* nonzero when file readable */
}

int external_teacher_bind_subprocess(
    ExternalTeacher *t,
    CnetModality modality,
    const char *name,
    Port input_port,
    Port output_port,
    const char *cmdline,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest)
{
    ExtSubprocessCtx *sc;
    size_t in_dim, out_dim;
    if (!t || !name || !name[0] || !cmdline || !cmdline[0] || !identity)
        return -1;
    if (identity->artifact_digest == 0) return -2;
    in_dim = port_total(input_port);
    out_dim = port_total(output_port);
    if (in_dim == 0 || out_dim == 0) return -3;
    sc = (ExtSubprocessCtx *)calloc(1, sizeof *sc);
    if (!sc) return -4;
    sc->cmdline = strdup(cmdline);
    if (!sc->cmdline) {
        free(sc);
        return -4;
    }
    sc->in_dim = in_dim;
    sc->out_dim = out_dim;
    if (external_teacher_bind_callback(t, modality, name, input_port,
                                       output_port, subprocess_teacher_fn, sc,
                                       identity, behavior_digest) != 0) {
        free(sc->cmdline);
        free(sc);
        return -5;
    }
    snprintf(t->kind, sizeof t->kind, "subprocess");
    return 0;
}

void external_teacher_unbind(ExternalTeacher *t) {
    if (!t) return;
    if (t->bound && t->ctx) {
        if (strcmp(t->kind, "table") == 0) {
            ExtTableCtx *tc = (ExtTableCtx *)t->ctx;
            free(tc->inputs);
            free(tc->targets);
            free(tc);
            t->ctx = NULL;
        } else if (strcmp(t->kind, "subprocess") == 0) {
            ExtSubprocessCtx *sc = (ExtSubprocessCtx *)t->ctx;
            subprocess_stop(sc);
            free(sc->cmdline);
            free(sc);
            t->ctx = NULL;
        }
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
