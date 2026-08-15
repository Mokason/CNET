/*
 * Contract: machine-checkable interface contracts for primitives. A contract
 * names a transform and carries a full port signature plus an exemplar table
 * over canonical values. btn_certify replays the exemplars through a frozen
 * net to grant or deny the contract. Certification is never persisted.
 */
#include "../../include/contract/contract.h"
#include "../../include/arena.h"
#include "../../include/plan_table.h"
#include "../../include/specialist.h"   /* Specialist / SpecialistKind — the door lives here */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- local helpers ------------------------------------------------------- */

static const char *family_token(PortFamily family) {
    switch (family) {
    case PORT_RAW:        return "raw";
    case PORT_ONEHOT:     return "onehot";
    case PORT_BINARY_MSB: return "binary_msb";
    case PORT_BINARY_LSB: return "binary_lsb";
    case PORT_EVIDENCE:   return "evidence";
    case PORT_CONCEPT:    return "concept";
    default:              return NULL;
    }
}

/* Returns the PortFamily for a token, or -1 cast to int on unknown. */
static int family_parse(const char *token) {
    if (strcmp(token, "raw") == 0)        return (int)PORT_RAW;
    if (strcmp(token, "onehot") == 0)     return (int)PORT_ONEHOT;
    if (strcmp(token, "binary_msb") == 0) return (int)PORT_BINARY_MSB;
    if (strcmp(token, "binary_lsb") == 0) return (int)PORT_BINARY_LSB;
    if (strcmp(token, "evidence") == 0)   return (int)PORT_EVIDENCE;
    if (strcmp(token, "concept") == 0)    return (int)PORT_CONCEPT;
    return -1;
}

/* A contract name is a non-empty atom over [A-Za-z0-9_], length <
   CONTRACT_NAME_MAX. Returns 1 if valid, else 0. */
static int name_valid(const char *name) {
    size_t len;
    const char *p;
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    len = strlen(name);
    if (len >= CONTRACT_NAME_MAX) {
        return 0;
    }
    for (p = name; *p != '\0'; ++p) {
        char ch = *p;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            return 0;
        }
    }
    return 1;
}

/* Sum of plan_port_total over an array of ports.  Returns (size_t)-1 on
   overflow so the caller can refuse a malformed file before any malloc. */
static size_t ports_total(const Port *ports, size_t n) {
    size_t total = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        size_t pw = ports[i].field_width;
        size_t pc = ports[i].field_count;
        /* Guard field_width * field_count overflow. */
        if (pc != 0 && pw > (size_t)-1 / pc) {
            return (size_t)-1;
        }
        size_t add = pw * pc;
        /* Guard summation overflow. */
        if (total > (size_t)-1 - add) {
            return (size_t)-1;
        }
        total += add;
    }
    return total;
}

/* Contract tables are canonical specifications, not arbitrary network inputs.
   Discrete families retain the historical exact {0,1} format. RAW carries any
   finite scalar vector; EVIDENCE carries finite normalized distributions. */
static int contract_slice_valid(Port port, const double *values) {
    size_t total;
    size_t i;
    double sum = 0.0;

    if (values == NULL || port.field_width == 0 || port.field_count == 0 ||
        port.field_width > (size_t)-1 / port.field_count) {
        return 0;
    }
    total = port.field_width * port.field_count;
    for (i = 0; i < total; ++i) {
        if (!isfinite(values[i])) {
            return 0;
        }
        if (port.family == PORT_EVIDENCE) {
            sum += values[i];
        } else if (port.family != PORT_RAW &&
                   values[i] != 0.0 && values[i] != 1.0) {
            return 0;
        }
    }
    if (port.family == PORT_EVIDENCE && fabs(sum - 1.0) > 1e-12) {
        return 0;
    }
    return port_validate(port, values);
}

/* ---- content identity (FNV-1a 64) ---------------------------------------- */

#define CONTRACT_FNV_OFFSET 1469598103934665603ULL
#define CONTRACT_FNV_PRIME  1099511628211ULL

static unsigned long long fnv_bytes(unsigned long long h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= CONTRACT_FNV_PRIME;
    }
    return h;
}

static unsigned long long fnv_size(unsigned long long h, size_t v) {
    unsigned long long x = (unsigned long long)v;
    return fnv_bytes(h, &x, sizeof x);
}

static unsigned long long fnv_ports(unsigned long long h, const Port *ports, size_t n) {
    size_t i;
    h = fnv_size(h, n);
    for (i = 0; i < n; ++i) {
        h = fnv_size(h, (size_t)ports[i].family);
        h = fnv_size(h, ports[i].field_width);
        h = fnv_size(h, ports[i].field_count);
        h = fnv_bytes(h, ports[i].tag, strlen(ports[i].tag));
        h = fnv_bytes(h, "|", 1);
    }
    return h;
}

/* Behavior-only digest: exactly the entries btn_save persists (live weight/
   bias slots via the stride-aware index helpers) plus the ternary inference
   settings. Excludes learning_rate, capacity, activations, and the runtime
   evidence counters — accruing evidence must not invalidate certification. */
unsigned long long contract_btn_digest(const BinaryTransformNetwork *btn) {
    unsigned long long h = CONTRACT_FNV_OFFSET;
    size_t i, j;

    if (btn == NULL) {
        return 0;
    }
    h = fnv_size(h, btn->input_count);
    h = fnv_size(h, btn->output_count);
    h = fnv_size(h, btn->hidden_count);
    h = fnv_size(h, (size_t)btn->ternary_inference);
    h = fnv_bytes(h, &btn->ternary_threshold, sizeof btn->ternary_threshold);
    h = fnv_ports(h, btn->input_ports, btn->input_port_count);
    h = fnv_ports(h, btn->output_ports, btn->output_port_count);

    /* Runtime adapters have no matrix payload. Their backend supplies a stable
       nonzero artifact/behavior digest; never hash process-local pointers. */
    if (btn_is_adapter(btn)) {
        h = fnv_bytes(h, &btn->adapter_digest, sizeof btn->adapter_digest);
        return h;
    }

    for (i = 0; i < btn->output_count; ++i) {
        h = fnv_bytes(h, &btn->output_bias[i], sizeof(double));
    }
    for (i = 0; i < btn->hidden_count; ++i) {
        h = fnv_bytes(h, &btn->hidden_bias[i], sizeof(double));
    }
    /* live weight slots only, mirroring btn_save's stride convention
       (input_hidden: hidden-major over input_count; hidden_output_weights:
       output-major over max_hidden_count) — capacity padding excluded */
    for (i = 0; i < btn->input_count; ++i) {
        for (j = 0; j < btn->hidden_count; ++j) {
            h = fnv_bytes(h, &btn->input_hidden[j * btn->input_count + i],
                          sizeof(double));
        }
    }
    for (i = 0; i < btn->hidden_count; ++i) {
        for (j = 0; j < btn->output_count; ++j) {
            h = fnv_bytes(h,
                          &btn->hidden_output_weights[j * btn->max_hidden_count + i],
                          sizeof(double));
        }
    }
    return h;
}

/* Spec digest over what a contract file round-trips: name + port signatures
   + exemplar tables. parent is unpersisted lineage metadata and excluded. */
unsigned long long contract_content_digest(const Contract *c) {
    unsigned long long h = CONTRACT_FNV_OFFSET;
    size_t in_total, out_total;

    if (c == NULL || c->inputs == NULL || c->outputs == NULL) {
        return 0;
    }
    in_total  = ports_total(c->input_ports,  c->input_port_count);
    out_total = ports_total(c->output_ports, c->output_port_count);
    if (in_total == (size_t)-1 || out_total == (size_t)-1) {
        return 0;
    }
    h = fnv_bytes(h, c->name, strlen(c->name));
    h = fnv_ports(h, c->input_ports, c->input_port_count);
    h = fnv_ports(h, c->output_ports, c->output_port_count);
    h = fnv_size(h, c->exemplar_count);
    h = fnv_bytes(h, c->inputs, c->exemplar_count * in_total * sizeof(double));
    h = fnv_bytes(h, c->outputs, c->exemplar_count * out_total * sizeof(double));
    return h;
}

/* ---- certification cache --------------------------------------------------
   Small in-process memo keyed by (btn digest, contract digest). Both digests
   cover every input the certification verdict depends on, and btn_certify is
   deterministic, so a hit IS the verdict a replay would produce. Round-robin
   replacement; never persisted. */

#define CERT_CACHE_SLOTS 64

typedef struct {
    unsigned long long btn_dig;
    unsigned long long con_dig;
    int rc;
    CertifyReport rep;
    int valid;
} CertCacheEntry;

static CertCacheEntry g_cert_cache[CERT_CACHE_SLOTS];
static size_t g_cert_cache_next;
static size_t g_cert_cache_hits;
static size_t g_cert_cache_misses;

void contract_cache_stats(size_t *hits, size_t *misses) {
    if (hits != NULL)   { *hits = g_cert_cache_hits; }
    if (misses != NULL) { *misses = g_cert_cache_misses; }
}

void contract_cache_reset(void) {
    memset(g_cert_cache, 0, sizeof g_cert_cache);
    g_cert_cache_next = 0;
    g_cert_cache_hits = 0;
    g_cert_cache_misses = 0;
}

static const CertCacheEntry *cert_cache_find(unsigned long long bd,
                                             unsigned long long cd) {
    size_t i;
    for (i = 0; i < CERT_CACHE_SLOTS; ++i) {
        if (g_cert_cache[i].valid &&
            g_cert_cache[i].btn_dig == bd && g_cert_cache[i].con_dig == cd) {
            return &g_cert_cache[i];
        }
    }
    return NULL;
}

static void cert_cache_insert(unsigned long long bd, unsigned long long cd,
                              int rc, const CertifyReport *rep) {
    CertCacheEntry *e = &g_cert_cache[g_cert_cache_next];
    g_cert_cache_next = (g_cert_cache_next + 1) % CERT_CACHE_SLOTS;
    e->btn_dig = bd;
    e->con_dig = cd;
    e->rc = rc;
    e->rep = *rep;
    e->valid = 1;
}

/* ---- contract_init_borrowed ---------------------------------------------- */

int contract_init_borrowed(Contract *c, const char *name,
                           const BinaryTransformNetwork *btn,
                           const double *inputs, const double *targets,
                           size_t exemplar_count) {
    Contract local;
    size_t in_total, out_total;
    size_t i, j;

    if (c == NULL || btn == NULL || inputs == NULL || targets == NULL ||
        !name_valid(name) || exemplar_count == 0 ||
        btn->input_port_count == 0 ||
        btn->input_port_count > BTN_MAX_INPUT_PORTS ||
        btn->output_port_count == 0 ||
        btn->output_port_count > BTN_MAX_OUTPUT_PORTS) {
        return -1;
    }
    for (i = 0; i < btn->input_port_count; ++i) {
        Port probe = btn->input_ports[i];
        if (family_token(probe.family) == NULL ||
            probe.field_width == 0 || probe.field_count == 0 ||
            memchr(probe.tag, '\0', sizeof probe.tag) == NULL ||
            (probe.tag[0] != '\0' && port_set_tag(&probe, probe.tag) != 0)) {
            return -1;
        }
    }
    for (i = 0; i < btn->output_port_count; ++i) {
        Port probe = btn->output_ports[i];
        if (family_token(probe.family) == NULL ||
            probe.field_width == 0 || probe.field_count == 0 ||
            memchr(probe.tag, '\0', sizeof probe.tag) == NULL ||
            (probe.tag[0] != '\0' && port_set_tag(&probe, probe.tag) != 0)) {
            return -1;
        }
    }

    in_total = ports_total(btn->input_ports, btn->input_port_count);
    out_total = ports_total(btn->output_ports, btn->output_port_count);
    if (in_total == 0 || in_total == (size_t)-1 || in_total != btn->input_count ||
        out_total == 0 || out_total == (size_t)-1 || out_total != btn->output_count ||
        exemplar_count > (size_t)-1 / sizeof(double) / in_total ||
        exemplar_count > (size_t)-1 / sizeof(double) / out_total) {
        return -1;
    }
    for (i = 0; i < exemplar_count; ++i) {
        size_t offset = 0;
        for (j = 0; j < btn->input_port_count; ++j) {
            if (!contract_slice_valid(btn->input_ports[j],
                                      inputs + i * in_total + offset)) {
                return -1;
            }
            offset += plan_port_total(btn->input_ports[j]);
        }
        offset = 0;
        for (j = 0; j < btn->output_port_count; ++j) {
            if (!contract_slice_valid(btn->output_ports[j],
                                      targets + i * out_total + offset)) {
                return -1;
            }
            offset += plan_port_total(btn->output_ports[j]);
        }
    }

    memset(&local, 0, sizeof local);
    strcpy(local.name, name);
    memcpy(local.input_ports, btn->input_ports,
           btn->input_port_count * sizeof(Port));
    local.input_port_count = btn->input_port_count;
    memcpy(local.output_ports, btn->output_ports,
           btn->output_port_count * sizeof(Port));
    local.output_port_count = btn->output_port_count;
    local.inputs = (double *)inputs;
    local.outputs = (double *)targets;
    local.exemplar_count = exemplar_count;
    local.owns_data = 0;
    *c = local;
    return 0;
}

/* ---- contract_save ------------------------------------------------------- */

int contract_save(const Contract *c, const char *path) {
    FILE *f;
    size_t i, j;
    size_t in_total, out_total;

    if (c == NULL || path == NULL) {
        return -1;
    }
    if (!name_valid(c->name)) {
        return -1;
    }
    if (c->exemplar_count == 0) {
        return -1;
    }

    f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }

    if (fprintf(f, "CNET_CONTRACT 2\n") < 0) { goto fail; }
    if (fprintf(f, "%s\n", c->name) < 0) { goto fail; }

    if (fprintf(f, "INPUTS %lu\n", (unsigned long)c->input_port_count) < 0) {
        goto fail;
    }
    for (i = 0; i < c->input_port_count; ++i) {
        const Port *p = &c->input_ports[i];
        const char *tok = family_token(p->family);
        if (tok == NULL) { goto fail; }
        if (fprintf(f, "PORT_IN %s %lu %lu %s\n",
                    tok,
                    (unsigned long)p->field_width,
                    (unsigned long)p->field_count,
                    p->tag[0] != '\0' ? p->tag : "-") < 0) {
            goto fail;
        }
    }

    if (fprintf(f, "OUTPUTS %lu\n", (unsigned long)c->output_port_count) < 0) {
        goto fail;
    }
    for (i = 0; i < c->output_port_count; ++i) {
        const Port *p = &c->output_ports[i];
        const char *tok = family_token(p->family);
        if (tok == NULL) { goto fail; }
        if (fprintf(f, "PORT_OUT %s %lu %lu %s\n",
                    tok,
                    (unsigned long)p->field_width,
                    (unsigned long)p->field_count,
                    p->tag[0] != '\0' ? p->tag : "-") < 0) {
            goto fail;
        }
    }

    if (fprintf(f, "EXEMPLARS %lu\n",
                (unsigned long)c->exemplar_count) < 0) {
        goto fail;
    }

    in_total = ports_total(c->input_ports, c->input_port_count);
    out_total = ports_total(c->output_ports, c->output_port_count);

    for (i = 0; i < c->exemplar_count; ++i) {
        const double *in_row  = c->inputs  + i * in_total;
        const double *out_row = c->outputs + i * out_total;
        for (j = 0; j < in_total; ++j) {
            if (j > 0) {
                if (fputc(' ', f) == EOF) { goto fail; }
            }
            if (fprintf(f, "%.17g", in_row[j]) < 0) { goto fail; }
        }
        for (j = 0; j < out_total; ++j) {
            if (fputc(' ', f) == EOF) { goto fail; }
            if (fprintf(f, "%.17g", out_row[j]) < 0) { goto fail; }
        }
        if (fputc('\n', f) == EOF) { goto fail; }
    }

    /* v2 tamper seal: digest over the semantic content the file round-trips.
       contract_load recomputes it from the loaded struct and refuses a
       mismatch, so silent edits to exemplars or ports are detected. */
    if (fprintf(f, "SEAL %016llx\n", contract_content_digest(c)) < 0) {
        goto fail;
    }

    /* fclose is the last I/O; close succeeds or we treat the file as corrupt.
       The file is already closed here so return directly -- do not goto fail,
       which would fclose a second time. */
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;

fail:
    fclose(f);
    return -1;
}

/* ---- contract_load ------------------------------------------------------- */

int contract_load(Contract *c, const char *path) {
    FILE *f;
    Contract local;
    Arena arena_inputs = {0};
    Arena arena_outputs = {0};
    char magic[32];
    int version;
    unsigned long n_in, n_out, n_ex;
    unsigned long field_width, field_count;
    char fam_tok[32];
    char tag_tok[PORT_TAG_MAX + 4]; /* room for the "-" sentinel */
    size_t i, j;
    size_t in_total, out_total;
    char dir_key[16];

    if (c == NULL || path == NULL) {
        return -1;
    }

    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    memset(&local, 0, sizeof local);

    /* magic + version (v1 legacy unsealed; v2 requires a matching SEAL) */
    if (fscanf(f, "%31s %d", magic, &version) != 2) { goto fail; }
    if (strcmp(magic, "CNET_CONTRACT") != 0 ||
        (version != 1 && version != 2)) { goto fail; }

    /* name */
    if (fscanf(f, "%63s", local.name) != 1) { goto fail; }
    if (!name_valid(local.name)) { goto fail; }

    /* input ports */
    if (fscanf(f, "%15s %lu", dir_key, &n_in) != 2) { goto fail; }
    if (strcmp(dir_key, "INPUTS") != 0) { goto fail; }
    if (n_in == 0 || n_in > BTN_MAX_INPUT_PORTS) { goto fail; }
    local.input_port_count = (size_t)n_in;

    for (i = 0; i < local.input_port_count; ++i) {
        char kw[16];
        int fam;
        if (fscanf(f, "%15s %31s %lu %lu",
                   kw, fam_tok, &field_width, &field_count) != 4) {
            goto fail;
        }
        if (strcmp(kw, "PORT_IN") != 0) { goto fail; }
        /* read tag as a separate token */
        if (fscanf(f, "%35s", tag_tok) != 1) { goto fail; }
        fam = family_parse(fam_tok);
        if (fam < 0) { goto fail; }
        if (field_width == 0 || field_count == 0) { goto fail; }
        local.input_ports[i].family = (PortFamily)fam;
        local.input_ports[i].field_width = (size_t)field_width;
        local.input_ports[i].field_count = (size_t)field_count;
        local.input_ports[i].tag[0] = '\0';
        if (strcmp(tag_tok, "-") != 0) {
            if (port_set_tag(&local.input_ports[i], tag_tok) != 0) {
                goto fail;
            }
        }
    }

    /* output ports */
    if (fscanf(f, "%15s %lu", dir_key, &n_out) != 2) { goto fail; }
    if (strcmp(dir_key, "OUTPUTS") != 0) { goto fail; }
    if (n_out == 0 || n_out > BTN_MAX_OUTPUT_PORTS) { goto fail; }
    local.output_port_count = (size_t)n_out;

    for (i = 0; i < local.output_port_count; ++i) {
        char kw[16];
        int fam;
        if (fscanf(f, "%15s %31s %lu %lu",
                   kw, fam_tok, &field_width, &field_count) != 4) {
            goto fail;
        }
        if (strcmp(kw, "PORT_OUT") != 0) { goto fail; }
        if (fscanf(f, "%35s", tag_tok) != 1) { goto fail; }
        fam = family_parse(fam_tok);
        if (fam < 0) { goto fail; }
        if (field_width == 0 || field_count == 0) { goto fail; }
        local.output_ports[i].family = (PortFamily)fam;
        local.output_ports[i].field_width = (size_t)field_width;
        local.output_ports[i].field_count = (size_t)field_count;
        local.output_ports[i].tag[0] = '\0';
        if (strcmp(tag_tok, "-") != 0) {
            if (port_set_tag(&local.output_ports[i], tag_tok) != 0) {
                goto fail;
            }
        }
    }

    /* exemplars header */
    if (fscanf(f, "%15s %lu", dir_key, &n_ex) != 2) { goto fail; }
    if (strcmp(dir_key, "EXEMPLARS") != 0) { goto fail; }
    if (n_ex == 0) { goto fail; }
    local.exemplar_count = (size_t)n_ex;

    /* A malformed file must be refused, not trusted with arithmetic: guard all
       multiplications so a hostile header cannot produce a wrapped allocation
       that the row-reading loop then overflows. */
    in_total  = ports_total(local.input_ports,  local.input_port_count);
    out_total = ports_total(local.output_ports, local.output_port_count);
    if (in_total == (size_t)-1 || out_total == (size_t)-1) { goto fail; }
    if (in_total == 0 || out_total == 0) { goto fail; }
    if (local.exemplar_count > (size_t)-1 / sizeof(double) / in_total) {
        goto fail;
    }
    if (local.exemplar_count > (size_t)-1 / sizeof(double) / out_total) {
        goto fail;
    }

    local.inputs  = arena_alloc(&arena_inputs,
                               local.exemplar_count * in_total * sizeof(double));
    local.outputs = arena_alloc(&arena_outputs,
                               local.exemplar_count * out_total * sizeof(double));
    if (local.inputs == NULL || local.outputs == NULL) { goto fail; }

    /* read exemplar rows and validate */
    for (i = 0; i < local.exemplar_count; ++i) {
        double *in_row  = local.inputs  + i * in_total;
        double *out_row = local.outputs + i * out_total;

        for (j = 0; j < in_total; ++j) {
            double v;
            if (fscanf(f, "%lf", &v) != 1) { goto fail; }
            if (!isfinite(v)) { goto fail; }
            in_row[j] = v;
        }
        for (j = 0; j < out_total; ++j) {
            double v;
            if (fscanf(f, "%lf", &v) != 1) { goto fail; }
            if (!isfinite(v)) { goto fail; }
            out_row[j] = v;
        }

        /* validate every input port slice */
        {
            size_t offset = 0;
            for (j = 0; j < local.input_port_count; ++j) {
                size_t tot = plan_port_total(local.input_ports[j]);
                if (!contract_slice_valid(local.input_ports[j],
                                          in_row + offset)) {
                    goto fail;
                }
                offset += tot;
            }
        }
        /* validate every output port slice */
        {
            size_t offset = 0;
            for (j = 0; j < local.output_port_count; ++j) {
                size_t tot = plan_port_total(local.output_ports[j]);
                if (!contract_slice_valid(local.output_ports[j],
                                          out_row + offset)) {
                    goto fail;
                }
                offset += tot;
            }
        }
    }

    /* v2: the trailing SEAL must match the digest recomputed from what we
       just loaded — a tampered exemplar or port line is refused even when
       each value is individually canonical. */
    if (version == 2) {
        char seal_kw[16];
        unsigned long long want = 0, have;
        if (fscanf(f, "%15s %llx", seal_kw, &want) != 2) { goto fail; }
        if (strcmp(seal_kw, "SEAL") != 0) { goto fail; }
        have = contract_content_digest(&local);
        if (have == 0 || have != want) { goto fail; }
        local.seal_verified = 1;
    }

    fclose(f);
    local.owns_data = 1;
    *c = local;
    return 0;

fail:
    arena_reset(&arena_inputs);
    arena_reset(&arena_outputs);
    fclose(f);
    return -1;
}

/* ---- contract_free ------------------------------------------------------- */

void contract_free(Contract *c) {
    if (c == NULL) {
        return;
    }
    if (c->owns_data) {
        free(c->inputs);
        free(c->outputs);
    }
    memset(c, 0, sizeof *c);
}

/* ---- contract_init_frozen / contract_set_parent ------------------------- */

int contract_init_frozen(Contract *c, const FrozenContractData *fd) {
    Contract local;
    size_t in_total, out_total;
    size_t i, j;

    if (c == NULL || fd == NULL || fd->name == NULL ||
        memchr(fd->name, '\0', CONTRACT_NAME_MAX) == NULL ||
        !name_valid(fd->name)) {
        return -1;
    }
    if (fd->parent != NULL && fd->parent[0] != '\0' &&
        (memchr(fd->parent, '\0', CONTRACT_NAME_MAX) == NULL ||
         !name_valid(fd->parent))) {
        return -1;
    }
    if (fd->input_port_count == 0 ||
        fd->input_port_count > BTN_MAX_INPUT_PORTS ||
        fd->output_port_count == 0 ||
        fd->output_port_count > BTN_MAX_OUTPUT_PORTS ||
        fd->input_ports == NULL || fd->output_ports == NULL ||
        fd->exemplar_count == 0 || fd->inputs == NULL || fd->outputs == NULL) {
        return -1;
    }

    for (i = 0; i < fd->input_port_count; ++i) {
        Port probe = fd->input_ports[i];
        if (family_token(probe.family) == NULL ||
            probe.field_width == 0 || probe.field_count == 0 ||
            memchr(probe.tag, '\0', sizeof probe.tag) == NULL ||
            (probe.tag[0] != '\0' && port_set_tag(&probe, probe.tag) != 0)) {
            return -1;
        }
    }
    for (i = 0; i < fd->output_port_count; ++i) {
        Port probe = fd->output_ports[i];
        if (family_token(probe.family) == NULL ||
            probe.field_width == 0 || probe.field_count == 0 ||
            memchr(probe.tag, '\0', sizeof probe.tag) == NULL ||
            (probe.tag[0] != '\0' && port_set_tag(&probe, probe.tag) != 0)) {
            return -1;
        }
    }

    in_total = ports_total(fd->input_ports, fd->input_port_count);
    out_total = ports_total(fd->output_ports, fd->output_port_count);
    if (in_total == 0 || in_total == (size_t)-1 ||
        out_total == 0 || out_total == (size_t)-1 ||
        fd->exemplar_count > (size_t)-1 / sizeof(double) / in_total ||
        fd->exemplar_count > (size_t)-1 / sizeof(double) / out_total) {
        return -1;
    }
    for (i = 0; i < fd->exemplar_count; ++i) {
        size_t offset = 0;
        for (j = 0; j < fd->input_port_count; ++j) {
            if (!contract_slice_valid(fd->input_ports[j],
                                      fd->inputs + i * in_total + offset)) {
                return -1;
            }
            offset += plan_port_total(fd->input_ports[j]);
        }
        offset = 0;
        for (j = 0; j < fd->output_port_count; ++j) {
            if (!contract_slice_valid(fd->output_ports[j],
                                      fd->outputs + i * out_total + offset)) {
                return -1;
            }
            offset += plan_port_total(fd->output_ports[j]);
        }
    }

    memset(&local, 0, sizeof local);
    strcpy(local.name, fd->name);
    if (fd->parent != NULL) {
        strcpy(local.parent, fd->parent);
    }
    memcpy(local.input_ports, fd->input_ports,
           fd->input_port_count * sizeof *fd->input_ports);
    local.input_port_count = fd->input_port_count;
    memcpy(local.output_ports, fd->output_ports,
           fd->output_port_count * sizeof *fd->output_ports);
    local.output_port_count = fd->output_port_count;
    local.inputs = (double *)fd->inputs;
    local.outputs = (double *)fd->outputs;
    local.exemplar_count = fd->exemplar_count;
    local.owns_data = 0;
    *c = local;
    return 0;
}

int contract_set_parent(Contract *c, const char *parent_name) {
    if (c == NULL) return -1;
    if (parent_name == NULL) {
        c->parent[0] = '\0';
    } else {
        strncpy(c->parent, parent_name, CONTRACT_NAME_MAX - 1);
        c->parent[CONTRACT_NAME_MAX - 1] = '\0';
    }
    return 0;
}

/* ---- btn_certify --------------------------------------------------------- */

static int ports_equal(const Port *a, size_t na, const Port *b, size_t nb) {
    size_t i;

    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; ++i) {
        if (a[i].family != b[i].family ||
            a[i].field_width != b[i].field_width ||
            a[i].field_count != b[i].field_count ||
            strcmp(a[i].tag, b[i].tag) != 0) {
            return 0;
        }
    }
    return 1;
}

int btn_certify(BinaryTransformNetwork *btn, const Contract *c,
                CertifyReport *report) {
    size_t in_total, out_total;
    double *clean = NULL;
    Arena arena;
    size_t s;
    int rc;
    double worst_margin = 1.0;
    CertifyReport local;
    unsigned long long btn_dig, con_dig;
    const CertCacheEntry *hit;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (btn == NULL || c == NULL || c->exemplar_count == 0 ||
        c->inputs == NULL || c->outputs == NULL ||
        c->input_port_count == 0 ||
        c->input_port_count > BTN_MAX_INPUT_PORTS ||
        c->output_port_count == 0 ||
        c->output_port_count > BTN_MAX_OUTPUT_PORTS) {
        return -1;
    }

    /* Gate 1 must precede content hashing: a forged signature can inflate a
       fixed-array port width, making the implied exemplar table larger than
       its borrowed allocation. Reject it before any digest reads table bytes. */
    if (!ports_equal(btn->input_ports, btn->input_port_count,
                     c->input_ports, c->input_port_count) ||
        !ports_equal(btn->output_ports, btn->output_port_count,
                     c->output_ports, c->output_port_count)) {
        return -1;
    }

    /* Cache: both digests cover everything the verdict depends on, so a hit
       returns exactly what a replay would. Digesting the btn costs about one
       exemplar-forward; a replay costs exemplar_count of them. */
    btn_dig = contract_btn_digest(btn);
    con_dig = contract_content_digest(c);
    if (btn_dig != 0 && con_dig != 0) {
        hit = cert_cache_find(btn_dig, con_dig);
        if (hit != NULL) {
            ++g_cert_cache_hits;
            if (report != NULL) {
                *report = hit->rep;
            }
            return hit->rc;
        }
        ++g_cert_cache_misses;
    }
    memset(&local, 0, sizeof local);

    in_total  = ports_total(c->input_ports,  c->input_port_count);
    out_total = ports_total(c->output_ports, c->output_port_count);
    if (in_total == (size_t)-1 || out_total == (size_t)-1 || out_total == 0) {
        return -1;
    }

    arena_init(&arena);
    clean = arena_alloc(&arena, out_total * sizeof *clean);
    if (clean == NULL) {
        return -1;
    }
    local.exemplars = c->exemplar_count;

    /* Gate 2: every exemplar must replay exactly. The replay always runs the
       full table so the cached result carries a complete report. */
    for (s = 0; s < c->exemplar_count; ++s) {
        const double *raw = btn_forward(btn, c->inputs + s * in_total);
        const double *want = c->outputs + s * out_total;
        int ok = raw != NULL;
        size_t off = 0;
        size_t p, i;

        for (p = 0; ok && p < c->output_port_count; ++p) {
            size_t total = plan_port_total(c->output_ports[p]);
            double mm;

            if (port_margin(c->output_ports[p], raw + off, &mm) == 0 &&
                mm < worst_margin) {
                worst_margin = mm;
            }
            if (!port_validate(c->output_ports[p], raw + off) ||
                port_canonicalize(c->output_ports[p], raw + off,
                                  clean + off) != 0) {
                ok = 0;
            }
            off += total;
        }
        for (i = 0; ok && i < out_total; ++i) {
            if (clean[i] != want[i]) {
                ok = 0;
            }
        }
        if (ok) {
            ++local.passed;
        } else {
            ++local.failed;
        }
    }

    local.min_margin = worst_margin;
    rc = (local.failed == 0) ? 0 : -1;
    if (btn_dig != 0 && con_dig != 0) {
        cert_cache_insert(btn_dig, con_dig, rc, &local);
    }
    if (report != NULL) {
        *report = local;
    }
    arena_reset(&arena);
    return rc;
}

int btn_certify_robust(BinaryTransformNetwork *btn, const Contract *c,
                       double margin_floor, CertifyReport *report) {
    CertifyReport local;
    int rc = btn_certify(btn, c, &local);
    if (report != NULL) {
        *report = local;
    }
    if (rc != 0) {
        return -1;
    }
    return local.min_margin >= margin_floor ? 0 : -1;
}

int contract_better_if(const Contract *c, const BinaryTransformNetwork *active,
                       const BinaryTransformNetwork *candidate) {
    CertifyReport active_report;
    CertifyReport candidate_report;
    int active_ok;
    int candidate_ok;

    if (c == NULL || candidate == NULL) {
        return -1;
    }
    if (c->exemplar_count == 0) {
        return -1;
    }
    if (active == NULL) {
        return (btn_certify((BinaryTransformNetwork *)candidate, c, NULL) == 0) ? 1 : -1;
    }

    candidate_ok = (btn_certify((BinaryTransformNetwork *)candidate, c, &candidate_report) == 0);
    if (!candidate_ok) {
        active_ok = (btn_certify((BinaryTransformNetwork *)active, c, &active_report) == 0);
        return active_ok ? 0 : -1;
    }

    active_ok = (btn_certify((BinaryTransformNetwork *)active, c, &active_report) == 0);
    if (!active_ok) {
        return 1;
    }

    if (candidate_report.failed < active_report.failed) {
        return 1;
    }
    if (candidate_report.failed > active_report.failed) {
        return 0;
    }

    /* Both candidates already replayed every exemplar. Prefer the one with
       greater worst-case headroom from the canonicalization boundary. This
       reuses the certification report, so comparison adds no second forward
       sweep. Reliability remains the tie-break for equal robustness. */
    if (candidate_report.min_margin > active_report.min_margin) {
        return 1;
    }
    if (candidate_report.min_margin < active_report.min_margin) {
        return 0;
    }
    return (btn_reliability(candidate) > btn_reliability(active)) ? 1 : 0;
}

int contract_swap_if_better(const Contract *c, BinaryTransformNetwork **active,
                            BinaryTransformNetwork *candidate) {
    int better;

    if (c == NULL || active == NULL || candidate == NULL) {
        return -1;
    }
    better = contract_better_if(c, *active, candidate);
    if (better == 1) {
        *active = candidate;
        return 1;
    }
    return better;
}

/* ---- registry_add_certified ---------------------------------------------- */

#if defined(__GNUC__)
int cnet_swap_registry_hook(PrimitiveRegistry *reg,
                            BinaryTransformNetwork *new_btn,
                            const char *name,
                            const Contract *new_c) __attribute__((weak));
#endif

CNET_INTERNAL int registry_add_certified(PrimitiveRegistry *reg,
                           BinaryTransformNetwork *btn,
                           const char *name, const Contract *c) {
    size_t i;

    if (reg == NULL || btn == NULL || name == NULL || c == NULL) {
        return -1;
    }
    if (btn_certify(btn, c, NULL) != 0) {
        return -1;
    }
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0) {
            /* Same-name slot: swap law, not contract_better_if.
               When LIBRARY / cnet.so is linked, cnet_swap_registry_hook
               is the strong symbol and calls cnet_swap_admit.
               Contract-only tests without the hook keep better-or-reject. */
#if defined(__GNUC__)
            if (cnet_swap_registry_hook != NULL)
                return cnet_swap_registry_hook(reg, btn, name, c);
#endif
            {
                int better = contract_better_if(c, reg->entries[i].btn, btn);

                if (better == 1) {
                    reg->entries[i].btn = btn;
                    reg->entries[i].certified = 1;
                    reg->entries[i].cert_btn_digest = contract_btn_digest(btn);
                    reg->entries[i].state = PRIM_FROZEN;
                    return 0;
                }
                return -1;
            }
        }
    }
    if (registry_add(reg, btn, name) != 0) {
        return -1;
    }
    reg->entries[reg->count - 1].certified = 1;
    reg->entries[reg->count - 1].cert_btn_digest = contract_btn_digest(btn);
    reg->entries[reg->count - 1].state = PRIM_FROZEN;
    return 0;
}

/* ---- the specialist admission door ----------------------------------------
   registry_add_certified above is the low-level certify-and-register; it is an
   implementation detail of THIS admission layer (contract.c/specialist.c). The
   two functions below are the ONLY sanctioned production entry point: they wrap
   a backend as a Specialist and admit it through the door, stamping the durable
   live SpecialistKind on the authoritative registry entry. They are defined
   here (rather than in specialist.c) so every production admission path links
   the door via the light contract.o without pulling in the CCE/model backends
   that specialist.c's runtime wraps require. */

int specialist_wrap_btn(Specialist *s, BinaryTransformNetwork *btn,
                        const char *name) {
    if (s == NULL || btn == NULL || name == NULL || name[0] == '\0') return -1;
    if (btn_is_adapter(btn)) return -1;   /* runtime adapters wrap at their own site */
    s->kind = SPECIALIST_KIND_BTN;
    s->btn = btn;
    s->name = name;
    s->digest = 0;
    return 0;
}

int specialist_admit(PrimitiveRegistry *reg, Specialist *s, const Contract *c) {
    size_t i;
    if (reg == NULL || s == NULL || s->btn == NULL || s->name == NULL ||
        c == NULL) {
        return -1;
    }
    if (s->kind < SPECIALIST_KIND_BTN || s->kind > SPECIALIST_KIND_ORACLE) {
        return -1;
    }
    if (registry_add_certified(reg, s->btn, s->name, c) != 0) return -1;
    /* Stamp the durable kind on the entry the door just certified (append or
       better-replacement, same first-name rule as registry_set_state). Kind is
       live identity, set once at admission; trust replays separately and is
       never persisted. */
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, s->name) == 0) {
            reg->entries[i].kind = s->kind;
            break;
        }
    }
    s->digest = contract_btn_digest(s->btn);
    return 0;
}

/* ---- registry_audit_certified --------------------------------------------
   A certificate binds a verdict to the WEIGHTS that earned it. Any certified
   entry whose current btn digest no longer matches the digest recorded at
   certification time has been mutated since (external write, aliasing bug,
   corruption) and is demoted to RESET: a stale certificate is never trusted,
   and the heal path is the only way back to FROZEN. */

size_t registry_audit_certified(PrimitiveRegistry *reg) {
    size_t i;
    size_t demoted = 0;

    if (reg == NULL) {
        return 0;
    }
    for (i = 0; i < reg->count; ++i) {
        if (!reg->entries[i].certified || reg->entries[i].btn == NULL) {
            continue;
        }
        if (contract_btn_digest(reg->entries[i].btn) !=
            reg->entries[i].cert_btn_digest) {
            reg->entries[i].certified = 0;
            reg->entries[i].cert_btn_digest = 0;
            reg->entries[i].state = PRIM_RESET;
            ++demoted;
        }
    }
    return demoted;
}

/* ---- registry_heal ------------------------------------------------------- */

int registry_heal(PrimitiveRegistry *reg, const char *name,
                  const Contract *contract, size_t max_epochs) {
    size_t idx, ic, oc, n_con, n_lab, total;
    RetrainQueue *q;
    BinaryTransformNetwork *btn;
    double *inputs = NULL;
    double *targets = NULL;
    int certified_ok;

    if (reg == NULL || name == NULL || contract == NULL) return -1;
    for (idx = 0; idx < reg->count; ++idx) {
        if (reg->entries[idx].name != NULL &&
            strcmp(reg->entries[idx].name, name) == 0) break;
    }
    if (idx == reg->count) return -1;
    if (reg->entries[idx].state != PRIM_RESET) return 0;   /* nothing to heal */

    q = reg->entries[idx].queue;
    if (q == NULL || q->labeled_count == 0) return 0;       /* no verified target: stay RESET */

    btn = reg->entries[idx].btn;
    if (btn == NULL) return -1;
    ic = btn->input_count;
    oc = btn->output_count;
    n_con = contract->exemplar_count;
    n_lab = q->labeled_count;

    /* Dimension gate: verify the contract's port totals match the btn's
       input/output counts BEFORE any allocation or memcpy.  The contract's
       exemplar tables (contract->inputs / contract->outputs) are laid out
       with in_total = ports_total(contract->input_ports) and out_total =
       ports_total(contract->output_ports).  Using btn->input_count as the
       row stride for contract->inputs is only safe when in_total == ic.
       A mismatch means the memcpy would read past the end of the contract
       buffer (over-read) or read too few doubles (under-read / data
       corruption).  Refuse the mismatched contract without mutating state. */
    {
        size_t c_in_total  = ports_total(contract->input_ports,
                                         contract->input_port_count);
        size_t c_out_total = ports_total(contract->output_ports,
                                          contract->output_port_count);
        if (c_in_total == (size_t)-1 || c_out_total == (size_t)-1 ||
            c_in_total != ic || c_out_total != oc) {
            return -1;  /* signature mismatch: refuse, leave state/queue intact */
        }
    }

    total = n_con + n_lab;

    inputs = malloc(total * ic * sizeof(double));
    targets = malloc(total * oc * sizeof(double));
    if (inputs == NULL || targets == NULL) { free(inputs); free(targets); return -1; }

    /* contract exemplars first, then the labeled queue */
    memcpy(inputs, contract->inputs, n_con * ic * sizeof(double));
    memcpy(targets, contract->outputs, n_con * oc * sizeof(double));
    memcpy(inputs + n_con * ic, q->labeled_inputs, n_lab * ic * sizeof(double));
    memcpy(targets + n_con * oc, q->labeled_targets, n_lab * oc * sizeof(double));

    (void)btn_train(btn, inputs, targets, total, max_epochs);
    free(inputs);
    free(targets);

    certified_ok = (btn_certify(btn, contract, NULL) == 0);
    if (!certified_ok) {
        return 0;   /* stays RESET -- never restore FROZEN without a passing certify */
    }

    /* Heal succeeded: restore FROZEN, clear labeled queue, reset stale evidence. */
    reg->entries[idx].state = PRIM_FROZEN;
    reg->entries[idx].certified = 1;
    reg->entries[idx].cert_btn_digest = contract_btn_digest(btn);
    q->labeled_count = 0;
    btn->output_successes = 0;
    btn->output_failures = 0;
    /* still-unlabeled rows are retained in the queue for future labeling. */
    return 1;
}

/* ---- shadow_promote_if_ready --------------------------------------------- */

int shadow_promote_if_ready(PrimitiveRegistry *reg, const char *shadow_name,
                            const Contract *contract, size_t min_evidence) {
    size_t s, a;
    BinaryTransformNetwork *sbtn, *abtn;
    unsigned long evidence;
    if (reg == NULL || shadow_name == NULL || contract == NULL) return -1;
    for (s = 0; s < reg->count; ++s) {
        if (reg->entries[s].name != NULL &&
            strcmp(reg->entries[s].name, shadow_name) == 0) break;
    }
    if (s == reg->count || reg->entries[s].shadow_of == NULL) return -1;
    for (a = 0; a < reg->count; ++a) {
        if (reg->entries[a].name != NULL &&
            strcmp(reg->entries[a].name, reg->entries[s].shadow_of) == 0) break;
    }
    if (a == reg->count) return -1;
    sbtn = reg->entries[s].btn;
    abtn = reg->entries[a].btn;
    if (sbtn == NULL || abtn == NULL) return -1;

    evidence = (unsigned long)sbtn->output_successes +
               (unsigned long)sbtn->output_failures;
    if (evidence < min_evidence) return 0;
    if (btn_reliability(sbtn) < btn_reliability(abtn)) return 0;
    if (btn_certify(sbtn, contract, NULL) != 0) return 0;

    reg->entries[s].shadow_of = NULL;
    reg->entries[s].state = PRIM_FROZEN;
    reg->entries[s].certified = 1;
    reg->entries[s].cert_btn_digest = contract_btn_digest(sbtn);
    reg->entries[a].state = PRIM_RESET;   /* demote the old active */
    return 1;
}

/* ---- contract_from_table (shared core) ---------------------------------- */

static int contract_from_table(Contract *out, const char *name,
                               const Port *in_ports, size_t n_in,
                               const Port *out_ports, size_t n_out,
                               PlanTeacherFn teacher, void *ctx,
                               BinaryTransformNetwork *const *members,
                               size_t n_members, size_t max_samples) {
    PlanTable table;
    size_t out_total = ports_total(out_ports, n_out);
    size_t i;

    if (out == NULL || !name_valid(name) || out_total == (size_t)-1) {
        return -1;
    }
    if (plan_table_build(in_ports, n_in, out_total, teacher, ctx,
                         members, n_members, max_samples, &table) != 0) {
        return -1;
    }
    if (table.kept == 0) {
        plan_table_free(&table);
        return -1;
    }

    memset(out, 0, sizeof *out);
    strcpy(out->name, name);
    for (i = 0; i < n_in; ++i) {
        out->input_ports[i] = in_ports[i];
    }
    out->input_port_count = n_in;
    for (i = 0; i < n_out; ++i) {
        out->output_ports[i] = out_ports[i];
    }
    out->output_port_count = n_out;
    out->inputs = table.inputs;     /* ownership transfers */
    out->outputs = table.targets;
    out->exemplar_count = table.kept;
    out->owns_data = 1;
    return 0;
}

/* ---- contract_from_route ------------------------------------------------- */

int contract_from_route(const RoutePlan *plan, const char *name,
                        size_t max_samples, Contract *out) {
    RoutePlan teacher_plan;
    Port in_port;
    Port out_port;
    size_t i;

    if (plan == NULL || out == NULL || plan->length < 1) {
        return -1;
    }
    for (i = 0; i < plan->length; ++i) {
        if (plan->steps[i] == NULL) {
            return -1;
        }
    }

    in_port  = plan->steps[0]->input_ports[0];
    out_port = plan->steps[plan->length - 1]->output_ports[0];

    teacher_plan = *plan;
    teacher_plan.strict = 1;

    return contract_from_table(out, name,
                               &in_port, 1,
                               &out_port, 1,
                               plan_route_teacher, &teacher_plan,
                               (BinaryTransformNetwork *const *)plan->steps,
                               plan->length,
                               max_samples);
}

/* ---- contract_from_dag --------------------------------------------------- */

int contract_from_dag(const DagPlan *plan, const DagSource *sources,
                      size_t n_sources, const char *name,
                      size_t max_samples, Contract *out) {
    PlanDagTeacherCtx ctx;
    BinaryTransformNetwork **members = NULL;
    Arena arena;
    Port in_ports[DAG_MAX_SLOTS];
    size_t offsets[DAG_MAX_SLOTS];
    Port out_port;
    size_t n_members;
    size_t offset = 0;
    size_t i;
    int rc;

    if (plan == NULL || out == NULL || sources == NULL ||
        n_sources == 0 || n_sources > DAG_MAX_SLOTS ||
        plan->root == NULL || plan->root->kind != DAG_PRIMITIVE ||
        plan->root->btn == NULL) {
        return -1;
    }

    /* Every declared source must be consumed exactly once. */
    {
        size_t counts[DAG_MAX_SLOTS] = {0};

        if (plan_dag_count_sources(plan->root, counts, n_sources) != 0) {
            return -1;
        }
        for (i = 0; i < n_sources; ++i) {
            if (counts[i] != 1) {
                return -1;
            }
        }
    }

    n_members = plan_dag_collect_members(plan->root, NULL, 0, 0);
    arena_init(&arena);
    members = arena_alloc(&arena, n_members * sizeof *members);
    if (members == NULL) {
        arena_reset(&arena);
        return -1;
    }
    plan_dag_collect_members(plan->root, members, n_members, 0);

    for (i = 0; i < n_sources; ++i) {
        in_ports[i] = sources[i].type;
        offsets[i] = offset;
        offset += plan_port_total(sources[i].type);
    }
    out_port = plan->root->btn->output_ports[plan->root->output_index];

    memset(&ctx.teacher, 0, sizeof ctx.teacher); /* owned stays NULL: borrowed */
    ctx.teacher.root = plan->root;
    ctx.teacher.strict = 1;
    ctx.declared = sources;
    ctx.n_sources = n_sources;
    ctx.offsets = offsets;

    rc = contract_from_table(out, name,
                             in_ports, n_sources,
                             &out_port, 1,
                             plan_dag_teacher, &ctx,
                             (BinaryTransformNetwork *const *)members,
                             n_members,
                             max_samples);

    arena_reset(&arena);
    return rc;
}

/* ---- contract_from_circuit ------------------------------------------------ */

int contract_from_circuit(const CircuitPlan *plan, const DagSource *sources,
                          size_t n_sources, const char *name,
                          size_t max_samples, Contract *out) {
    PlanCircuitTeacherCtx ctx;
    BinaryTransformNetwork **members = NULL;
    Arena arena;
    Port in_ports[DAG_MAX_SLOTS];
    size_t offsets[DAG_MAX_SLOTS];
    Port out_ports[CIRCUIT_MAX_ROOTS];
    size_t n_members;
    size_t offset = 0;
    size_t i;
    int rc;

    if (plan == NULL || out == NULL || sources == NULL ||
        n_sources == 0 || n_sources > DAG_MAX_SLOTS ||
        plan->root_count == 0 || plan->root_count > CIRCUIT_MAX_ROOTS) {
        return -1;
    }
    for (i = 0; i < plan->root_count; ++i) {
        const DagNode *root = plan->roots[i];

        if (root == NULL || root->kind != DAG_PRIMITIVE || root->btn == NULL ||
            (size_t)plan->root_ports[i] >= root->btn->output_port_count) {
            return -1;
        }
        out_ports[i] = root->btn->output_ports[plan->root_ports[i]];
    }

    /* Every declared source must be referenced exactly once
       (sharing-aware: a shared node's sources count once). */
    {
        size_t counts[DAG_MAX_SLOTS] = {0};

        if (plan_circuit_count_sources(plan, counts, n_sources) != 0) {
            return -1;
        }
        for (i = 0; i < n_sources; ++i) {
            if (counts[i] != 1) {
                return -1;
            }
        }
    }

    n_members = plan_circuit_collect_members(plan, NULL, 0);
    if (n_members == 0) {
        return -1;
    }
    arena_init(&arena);
    members = arena_alloc(&arena, n_members * sizeof *members);
    if (members == NULL) {
        arena_reset(&arena);
        return -1;
    }
    plan_circuit_collect_members(plan, members, n_members);

    for (i = 0; i < n_sources; ++i) {
        in_ports[i] = sources[i].type;
        offsets[i] = offset;
        offset += plan_port_total(sources[i].type);
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.teacher = *plan;     /* borrowed roots; never freed through ctx */
    ctx.teacher.strict = 1;
    ctx.declared = sources;
    ctx.n_sources = n_sources;
    ctx.offsets = offsets;

    rc = contract_from_table(out, name,
                             in_ports, n_sources,
                             out_ports, plan->root_count,
                             plan_circuit_teacher, &ctx,
                             (BinaryTransformNetwork *const *)members,
                             n_members,
                             max_samples);

    arena_reset(&arena);
    return rc;
}



