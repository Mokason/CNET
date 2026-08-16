#include "../../include/router.h"
#include "../../include/contract/unit.h"
#include "../../include/contract/contract.h"
#include "../../include/router/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <errno.h>
#if defined(_WIN32)
#include <direct.h>   /* _mkdir (single-argument form) */
#endif

/* ========================================================================
 * Registry core + persistence + lifecycle + entry utilities
 * Extracted from monolithic src/router.c for SRP / maintainability.
 * ======================================================================== */

/* ---- low level port + path helpers (used by save and others) ----------- */

static int registry_port_count(Port port, size_t *count) {
    if (count == NULL || port.field_width == 0 || port.field_count == 0) {
        return -1;
    }
    if (port.field_width > (size_t)-1 / port.field_count) {
        return -1;
    }
    *count = port.field_width * port.field_count;
    return 0;
}

static int registry_ports_total(
    const Port *ports,
    size_t count,
    size_t *out_total
) {
    size_t total = 0;
    size_t i;

    if (ports == NULL || out_total == NULL || count == 0) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        size_t current = 0;
        if (registry_port_count(ports[i], &current) != 0) {
            return -1;
        }
        if (total > (size_t)-1 - current) {
            return -1;
        }
        total += current;
    }
    if (total == 0) {
        return -1;
    }
    *out_total = total;
    return 0;
}

static int registry_build_path(
    char **out,
    const char *dir,
    const char *name,
    const char *suffix
) {
    size_t dir_len;
    size_t name_len;
    size_t need;
    int use_sep;

    if (out == NULL || dir == NULL || name == NULL || suffix == NULL) {
        return -1;
    }

    dir_len = strlen(dir);
    name_len = strlen(name);
    if (name_len == 0) {
        return -1;
    }

    use_sep = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
    need = dir_len + (size_t)use_sep + name_len + 1 + strlen(suffix) + 1;
    *out = malloc(need);
    if (*out == NULL) {
        return -1;
    }

    if (dir_len == 0) {
        (void)snprintf(*out, need, "%s%s", name, suffix);
    } else if (use_sep) {
        (void)snprintf(*out, need, "%s/%s%s", dir, name, suffix);
    } else {
        (void)snprintf(*out, need, "%s%s%s", dir, name, suffix);
    }
    return 0;
}

/* 3C: write an entry's expansion recipe + cost truth as a small text sidecar. */
static int registry_write_expansion(const RegistryEntry *e, const char *path) {
    FILE *f;
    size_t k;
    f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "expansion v1\n");
    fprintf(f, "teacher_mac %lu\n", (unsigned long)e->teacher_mac);
    fprintf(f, "student_mac %lu\n", (unsigned long)e->student_mac);
    fprintf(f, "compute_beneficial %d\n", e->compute_beneficial);
    fprintf(f, "expand_in_low %d\n", e->expand_in_low);
    fprintf(f, "primitives %lu\n", (unsigned long)e->recipe->primitive_count);
    for (k = 0; k < e->recipe->primitive_count; ++k) {
        fprintf(f, "%s\n", e->recipe->primitives[k]);
    }
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;
}

static int registry_synthesize_contract(
    BinaryTransformNetwork *btn,
    const char *name,
    Contract *c_out
) {
    size_t in_total;
    size_t out_total;
    size_t port_total;
    size_t offset;
    size_t i;
    size_t bit;
    double *inputs = NULL;
    double *outputs = NULL;
    const double *predicted;

    if (btn == NULL || name == NULL || c_out == NULL) {
        return -1;
    }

    if (registry_ports_total(btn->input_ports, btn->input_port_count, &in_total) != 0 ||
        registry_ports_total(btn->output_ports, btn->output_port_count, &out_total) != 0) {
        return -1;
    }

    inputs = calloc(in_total, sizeof(*inputs));
    outputs = malloc(out_total * sizeof(*outputs));
    if (inputs == NULL || outputs == NULL) {
        free(inputs);
        free(outputs);
        return -1;
    }

    offset = 0;
    for (i = 0; i < btn->input_port_count; ++i) {
        size_t k;
        if (registry_port_count(btn->input_ports[i], &port_total) != 0) {
            free(inputs);
            free(outputs);
            return -1;
        }

        for (k = 0; k < btn->input_ports[i].field_count; ++k) {
            size_t base = offset + k * btn->input_ports[i].field_width;

            for (bit = 0; bit < btn->input_ports[i].field_width; ++bit) {
                inputs[base + bit] = 0.0;
            }

            switch (btn->input_ports[i].family) {
            case PORT_ONEHOT:
                inputs[base] = 1.0;
                break;
            case PORT_BINARY_MSB:
            case PORT_BINARY_LSB:
            case PORT_RAW:
                break;
            default:
                free(inputs);
                free(outputs);
                return -1;
            }
        }
        offset += port_total;
    }

    predicted = btn_forward(btn, inputs);
    if (predicted == NULL) {
        free(inputs);
        free(outputs);
        return -1;
    }
    memcpy(outputs, predicted, out_total * sizeof(*outputs));

    offset = 0;
    for (i = 0; i < btn->output_port_count; ++i) {
        if (registry_port_count(btn->output_ports[i], &port_total) != 0) {
            free(inputs);
            free(outputs);
            return -1;
        }
        if (port_canonicalize(btn->output_ports[i], outputs + offset, outputs + offset) != 0) {
            free(inputs);
            free(outputs);
            return -1;
        }
        offset += port_total;
    }

    if (contract_init_borrowed(c_out, name, btn, inputs, outputs, 1) != 0) {
        free(inputs);
        free(outputs);
        return -1;
    }
    c_out->owns_data = 1;
    c_out->inputs = inputs;
    c_out->outputs = outputs;
    return 0;
}

/* ---- public registry persistence -------------------------------------- */

int registry_save(const PrimitiveRegistry *reg, const char *dir) {
    size_t i;

    if (reg == NULL || dir == NULL) {
        return -1;
    }

    for (i = 0; i < reg->count; ++i) {
        char *btn_path = NULL;
        char *contract_path = NULL;
        char *stats_path = NULL;
        Contract contract = {0};
        int status = -1;

        if (reg->entries[i].btn == NULL ||
            reg->entries[i].name == NULL) {
            return -1;
        }

        /* weights + contract persist as ONE sealed binary unit (<name>.cnu):
           binary f64 weights + bit-packed canonical exemplars + FNV seal,
           replacing the legacy <name>.btn + <name>.contract text pair. */
        if (registry_build_path(&btn_path, dir, reg->entries[i].name, ".cnu") != 0) {
            return -1;
        }
        if (registry_synthesize_contract(reg->entries[i].btn,
                                         reg->entries[i].name,
                                         &contract) != 0) {
            free(btn_path);
            return -1;
        }
        status = unit_save(reg->entries[i].btn, &contract, btn_path);
        contract_free(&contract);
        if (status != 0) {
            free(btn_path);
            return -1;
        }
        (void)contract_path;

        if (registry_build_path(&stats_path, dir, reg->entries[i].name,
                               ".stats") != 0) {
            free(btn_path);
            free(contract_path);
            return -1;
        }
        {
            FILE *sf = fopen(stats_path, "w");
            if (sf) {
                unsigned long s = 0, f = 0;
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
                s = (unsigned long)reg->entries[i].btn->output_successes;
                f = (unsigned long)reg->entries[i].btn->output_failures;
#else
                s = reg->entries[i].btn->output_successes;
                f = reg->entries[i].btn->output_failures;
#endif
                fprintf(sf, "CNET_STATS 1\n%lu %lu\n", s, f);
                fclose(sf);
            }
        }

        /* 3C: an expandable chunk also writes its recipe + cost sidecar. */
        if (reg->entries[i].recipe != NULL) {
            char *exp_path = NULL;
            if (registry_build_path(&exp_path, dir, reg->entries[i].name,
                                    ".expansion") != 0) {
                free(btn_path);
                free(contract_path);
                free(stats_path);
                return -1;
            }
            status = registry_write_expansion(&reg->entries[i], exp_path);
            free(exp_path);
            if (status != 0) {
                free(btn_path);
                free(contract_path);
                free(stats_path);
                return -1;
            }
        }

        free(btn_path);
        free(contract_path);
        free(stats_path);
    }

    /* 3F: persist global flags for CNET-D, 3C, text contract */
    {
        char *meta_path = NULL;
        FILE *f;
        if (registry_build_path(&meta_path, dir, "registry", ".meta") != 0) {
            return -1;
        }
        f = fopen(meta_path, "w");
        if (f) {
            fprintf(f, "cnet_d_influence %f\n", reg->cnet_d_influence);
            fprintf(f, "expand_in_low_enabled %d\n", reg->expand_in_low_enabled);
            fprintf(f, "text_contract_expansion_enabled %d\n", reg->text_contract_expansion_enabled);
            fclose(f);
        }
        free(meta_path);
    }
    return 0;
}

/* ---- usability + ranking (used by planners) --------------------------- */

static int entry_usable_base(const PrimitiveRegistry *reg, size_t i) {
    if (reg->lifecycle_enabled &&
        (reg->entries[i].state == PRIM_RESET || reg->entries[i].shadow_of != NULL)) {
        return 0;
    }
    return !reg->require_certified || reg->entries[i].certified;
}

static int recipe_available(const PrimitiveRegistry *reg, const ExpansionRecipe *r,
                            size_t self) {
    size_t k, j;
    if (r == NULL || r->primitive_count == 0) return 0;
    for (k = 0; k < r->primitive_count; ++k) {
        int found = 0;
        for (j = 0; j < reg->count; ++j) {
            if (j == self) continue;
            if (reg->entries[j].name != NULL && reg->entries[j].btn != NULL &&
                strcmp(reg->entries[j].name, r->primitives[k]) == 0 &&
                entry_usable_base(reg, j)) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

int entry_usable(const PrimitiveRegistry *reg, size_t i) {
    const RegistryEntry *e = &reg->entries[i];
    if (!entry_usable_base(reg, i)) return 0;
    /* 3C: only chunks MARKED expand_in_low (i.e. NOT compute-beneficial) hide
       in LOW power; the split had dropped the e->expand_in_low condition,
       expanding beneficial chunks too (caught by test_expansion). */
    if (e->expand_in_low && e->recipe != NULL && reg->expand_in_low_enabled &&
        reg->power_mode == CNET_POWER_LOW) {
        return !recipe_available(reg, e->recipe, i);
    }
    return 1;
}

/* rank_by_reliability was a duplicate of the active implementation in
   dag_full.c; removed to eliminate unused-function warning. */

/* ---- public registry API --------------------------------------------- */

void registry_init(PrimitiveRegistry *reg) {
    if (reg == NULL) {
        return;
    }
    reg->entries = NULL;
    reg->count = 0;
    reg->capacity = 0;
    reg->require_certified = 0;
    reg->dag_beam_limit = 8;
    reg->attention_mode = CNET_ATTENTION_OFF;
    reg->attention_prune_k = 0;
    reg->rank_artifact = NULL;
    reg->disable_plan_memo = 0;
    reg->lifecycle_enabled = 0;
    reg->power_mode = CNET_POWER_DEFAULT;
    reg->expand_in_low_enabled = 0;
    reg->cnet_d_influence = 0.6;
    reg->text_contract_expansion_enabled = 0;
    reg->streamer = NULL;
    reg->name_hash = NULL;
    reg->name_hash_cap = 0;
    reg->lora_serving_enabled = 0;   /* opt-in; see registry_lora_enable_serving */
}

/* ---- name hash (Phase A: registry_find chokepoint) --------------------- */

static int registry_linear_forced(void) {
    const char *e = getenv("CNET_REGISTRY_LINEAR");
    return (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
}

static uint64_t registry_name_fnv(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    const unsigned char *p = (const unsigned char *)s;
    if (!p) return 1ULL;
    for (; *p; p++) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static int registry_hash_insert_at(PrimitiveRegistry *reg, size_t idx) {
    const char *name;
    uint64_t h;
    size_t mask, i, probes;
    if (!reg || !reg->name_hash || reg->name_hash_cap == 0 || idx >= reg->count)
        return -1;
    name = reg->entries[idx].name;
    if (!name || !name[0]) return 0; /* anonymous: not indexed */
    h = registry_name_fnv(name);
    mask = reg->name_hash_cap - 1;
    i = (size_t)h & mask;
    for (probes = 0; probes < reg->name_hash_cap; probes++) {
        if (reg->name_hash[i] == 0) {
            reg->name_hash[i] = idx + 1; /* 0 reserved empty */
            return 0;
        }
        i = (i + 1) & mask;
    }
    return -1; /* table full */
}

static int registry_hash_rebuild(PrimitiveRegistry *reg) {
    size_t want, i;
    size_t *tab;
    if (!reg) return -1;
    if (registry_linear_forced()) {
        free(reg->name_hash);
        reg->name_hash = NULL;
        reg->name_hash_cap = 0;
        return 0;
    }
    want = 16;
    while (want < reg->count * 2 + 8) want <<= 1;
    tab = (size_t *)calloc(want, sizeof *tab);
    if (!tab) return -1;
    free(reg->name_hash);
    reg->name_hash = tab;
    reg->name_hash_cap = want;
    for (i = 0; i < reg->count; i++) {
        if (registry_hash_insert_at(reg, i) != 0) {
            free(reg->name_hash);
            reg->name_hash = NULL;
            reg->name_hash_cap = 0;
            return -1;
        }
    }
    return 0;
}



void registry_init_production(PrimitiveRegistry *reg) {
    registry_init(reg);
    if (reg != NULL) {
        reg->require_certified = 1;
    }
}

void registry_set_dag_beam_limit(PrimitiveRegistry *reg, size_t dag_beam_limit) {
    if (reg == NULL) {
        return;
    }
    reg->dag_beam_limit = dag_beam_limit;
}

int registry_add(PrimitiveRegistry *reg, BinaryTransformNetwork *btn, const char *name) {
    size_t idx;
    if (reg == NULL || btn == NULL) {
        return -1;
    }

    if (reg->count == reg->capacity) {
        size_t new_capacity = reg->capacity == 0 ? 4 : reg->capacity * 2;
        RegistryEntry *grown =
            realloc(reg->entries, new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return -1;
        }
        reg->entries = grown;
        reg->capacity = new_capacity;
    }

    idx = reg->count;
    reg->entries[idx].btn = btn;
    reg->entries[idx].name = name;
    reg->entries[idx].kind = SPECIALIST_KIND_BTN;  /* native default; specialist_admit stamps the true kind */
    reg->entries[idx].certified = 0;
    reg->entries[idx].cert_btn_digest = 0;
    reg->entries[idx].state = PRIM_FUZZY;
    reg->entries[idx].queue = NULL;
    reg->entries[idx].shadow_of = NULL;
    reg->entries[idx].teacher_mac = 0;
    reg->entries[idx].student_mac = 0;
    reg->entries[idx].compute_beneficial = 0;
    reg->entries[idx].recipe = NULL;
    reg->entries[idx].expand_in_low = 0;
    reg->entries[idx].lora = NULL;   /* borrowed adapter, attached by registry_lora.* */
    reg->entries[idx].lora_certified = 0;   /* uncertified until registry_certify_lora */
    reg->count++;

    /* Index name; fail-open to linear find if hash OOM (table cleared).
       Rebuild path already re-inserts all entries including idx — no double insert. */
    if (name && name[0] && !registry_linear_forced()) {
        int need_rebuild = (!reg->name_hash || reg->name_hash_cap == 0 ||
                            reg->count * 10 > reg->name_hash_cap * 7);
        if (need_rebuild) {
            if (registry_hash_rebuild(reg) != 0) {
                free(reg->name_hash);
                reg->name_hash = NULL;
                reg->name_hash_cap = 0;
            }
        } else if (registry_hash_insert_at(reg, idx) != 0) {
            if (registry_hash_rebuild(reg) != 0) {
                free(reg->name_hash);
                reg->name_hash = NULL;
                reg->name_hash_cap = 0;
            }
        }
    }
    return 0;
}

static void retrain_queue_free(RetrainQueue *q) {
    if (q == NULL) return;
    free(q->labeled_inputs);
    free(q->labeled_targets);
    free(q->unlabeled_inputs);
    free(q->unlabeled_raw);
    free(q);
}

void registry_free(PrimitiveRegistry *reg) {
    if (reg == NULL) {
        return;
    }
    {
        size_t i;
        for (i = 0; i < reg->count; ++i) {
            retrain_queue_free(reg->entries[i].queue);
            reg->entries[i].queue = NULL;
            free(reg->entries[i].recipe);
            reg->entries[i].recipe = NULL;
        }
    }
    free(reg->entries);
    reg->entries = NULL;
    reg->count = 0;
    reg->capacity = 0;
    free(reg->name_hash);
    reg->name_hash = NULL;
    reg->name_hash_cap = 0;
}

int registry_remove_last(PrimitiveRegistry *reg) {
    if (reg == NULL || reg->count == 0) return -1;
    retrain_queue_free(reg->entries[reg->count - 1].queue);
    reg->entries[reg->count - 1].queue = NULL;
    free(reg->entries[reg->count - 1].recipe);
    reg->entries[reg->count - 1].recipe = NULL;
    reg->count--;
    /* Indices after removed slot don't shift (only last); rebuild keeps map honest. */
    if (reg->name_hash)
        (void)registry_hash_rebuild(reg);
    return 0;
}

/* Forward decl — used by set_state and peers before definition. */
static size_t registry_find(const PrimitiveRegistry *reg, const char *name);

int registry_set_state(PrimitiveRegistry *reg, const char *name,
                       PrimitiveState state) {
    size_t i;
    if (reg == NULL || name == NULL) return -1;
    i = registry_find(reg, name);
    if (i == reg->count) return -1;
    reg->entries[i].state = state;
    return 0;
}

void lifecycle_promote_provisional(PrimitiveRegistry *reg,
                                   double promote_threshold,
                                   size_t min_evidence) {
    size_t i;
    if (reg == NULL) return;
    for (i = 0; i < reg->count; ++i) {
        const BinaryTransformNetwork *p = reg->entries[i].btn;
        unsigned long evidence;
        if (p == NULL || reg->entries[i].state != PRIM_FUZZY) {
            continue;
        }
        evidence = (unsigned long)p->output_successes +
                   (unsigned long)p->output_failures;
        if (btn_reliability(p) >= promote_threshold &&
            evidence >= min_evidence) {
            reg->entries[i].state = PRIM_PROVISIONAL;
        }
    }
}

/* Name → index. Prefer the per-registry open-addressing hash; linear fallback. */
static size_t registry_find(const PrimitiveRegistry *reg, const char *name) {
    size_t i;
    if (!reg || !name) return reg ? reg->count : 0;

    if (reg->name_hash && reg->name_hash_cap && !registry_linear_forced()) {
        uint64_t h = registry_name_fnv(name);
        size_t mask = reg->name_hash_cap - 1;
        size_t slot = (size_t)h & mask;
        size_t probes;
        for (probes = 0; probes < reg->name_hash_cap; probes++) {
            size_t v = reg->name_hash[slot];
            size_t idx;
            if (v == 0) break; /* empty → miss */
            idx = v - 1;
            if (idx < reg->count && reg->entries[idx].name != NULL &&
                strcmp(reg->entries[idx].name, name) == 0) {
                return idx;
            }
            slot = (slot + 1) & mask;
        }
        /* Hash miss: fall through to linear (handles rare desync). */
    }

    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0) {
            return i;
        }
    }
    return reg->count;
}

int registry_set_expansion(PrimitiveRegistry *reg, const char *name,
                           const char *const *prim_names, size_t n,
                           size_t teacher_mac, size_t student_mac,
                           int compute_beneficial) {
    size_t idx, k;
    RegistryEntry *e;
    if (reg == NULL || name == NULL) {
        return -1;
    }
    idx = registry_find(reg, name);
    if (idx == reg->count) {
        return -1;
    }
    e = &reg->entries[idx];
    free(e->recipe);
    e->recipe = NULL;
    e->teacher_mac = teacher_mac;
    e->student_mac = student_mac;
    e->compute_beneficial = compute_beneficial ? 1 : 0;
    e->expand_in_low = 0;
    if (prim_names != NULL && n >= 1 && n <= EXPAND_MAX_PRIMS) {
        ExpansionRecipe *r = calloc(1, sizeof *r);
        if (r == NULL) {
            return -1;
        }
        for (k = 0; k < n; ++k) {
            (void)snprintf(r->primitives[k], sizeof r->primitives[k], "%s",
                           prim_names[k] != NULL ? prim_names[k] : "");
        }
        r->primitive_count = n;
        e->recipe = r;
        e->expand_in_low = compute_beneficial ? 0 : 1;
    }
    return 0;
}

int registry_load_expansion(PrimitiveRegistry *reg, const char *name,
                            const char *dir) {
    size_t idx, k;
    char *path = NULL;
    FILE *f;
    char header[32];
    unsigned long tmac = 0, smac = 0, count = 0;
    int cben = 0, einlow = 0;
    ExpansionRecipe *r;

    if (reg == NULL || name == NULL || dir == NULL) {
        return -1;
    }
    idx = registry_find(reg, name);
    if (idx == reg->count) {
        return -1;
    }
    if (registry_build_path(&path, dir, name, ".expansion") != 0) {
        return -1;
    }
    f = fopen(path, "r");
    free(path);
    if (f == NULL) {
        return 0; /* no sidecar is not an error */
    }
    if (!fgets(header, sizeof header, f) || strncmp(header, "expansion", 9) != 0) {
        fclose(f);
        return -1;
    }
    if (fscanf(f, "teacher_mac %lu\n", &tmac) != 1 ||
        fscanf(f, "student_mac %lu\n", &smac) != 1 ||
        fscanf(f, "compute_beneficial %d\n", &cben) != 1 ||
        fscanf(f, "expand_in_low %d\n", &einlow) != 1 ||
        fscanf(f, "primitives %lu\n", &count) != 1) {
        fclose(f);
        return -1;
    }
    if (count > EXPAND_MAX_PRIMS) {
        fclose(f);
        return -1;
    }
    r = calloc(1, sizeof *r);
    if (r == NULL) {
        fclose(f);
        return -1;
    }
    r->primitive_count = (size_t)count;
    for (k = 0; k < r->primitive_count; ++k) {
        if (!fgets(r->primitives[k], sizeof r->primitives[k], f)) {
            free(r);
            fclose(f);
            return -1;
        }
        /* strip trailing newline */
        size_t len = strlen(r->primitives[k]);
        if (len > 0 && (r->primitives[k][len-1] == '\n' || r->primitives[k][len-1] == '\r')) {
            r->primitives[k][len-1] = '\0';
        }
    }
    fclose(f);

    /* attach */
    free(reg->entries[idx].recipe);
    reg->entries[idx].recipe = r;
    reg->entries[idx].teacher_mac = (size_t)tmac;
    reg->entries[idx].student_mac = (size_t)smac;
    reg->entries[idx].compute_beneficial = cben ? 1 : 0;
    reg->entries[idx].expand_in_low = einlow ? 1 : 0;
    return 0;
}

/* Load persisted global policy knobs. Missing state is a no-op; an existing
   file is an all-or-nothing checkpoint and malformed/partial data fails. */
int registry_load_globals(PrimitiveRegistry *reg, const char *dir) {
    char *path = NULL;
    FILE *f;
    char line[128];
    double influence = 0.0;
    int expand_low = 0;
    int text_expand = 0;
    unsigned seen = 0;
    if (reg == NULL || dir == NULL) return -1;
    if (registry_build_path(&path, dir, "registry", ".meta") != 0) return -1;
    f = fopen(path, "r");
    free(path);
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        double d;
        int v;
        char extra;
        if (sscanf(line, "cnet_d_influence %lf %c", &d, &extra) == 1) {
            if ((seen & 1u) != 0 || !isfinite(d) || d < 0.0 || d > 1.0) goto malformed;
            influence = d;
            seen |= 1u;
        } else if (sscanf(line, "expand_in_low_enabled %d %c", &v, &extra) == 1) {
            if ((seen & 2u) != 0 || (v != 0 && v != 1)) goto malformed;
            expand_low = v;
            seen |= 2u;
        } else if (sscanf(line, "text_contract_expansion_enabled %d %c", &v, &extra) == 1) {
            if ((seen & 4u) != 0 || (v != 0 && v != 1)) goto malformed;
            text_expand = v;
            seen |= 4u;
        } else {
            goto malformed;
        }
    }
    if (ferror(f) || seen != 7u) goto malformed;
    if (fclose(f) != 0) return -1;
    reg->cnet_d_influence = influence;
    reg->expand_in_low_enabled = expand_low;
    reg->text_contract_expansion_enabled = text_expand;
    return 0;

malformed:
    fclose(f);
    return -1;
}

int registry_restore_runtime_state(PrimitiveRegistry *reg, const char *dir) {
    size_t i;
    if (reg == NULL || dir == NULL) return -1;
    if (registry_load_globals(reg, dir) != 0) return -1;
    for (i = 0; i < reg->count; ++i) {
        char *stats_path = NULL;
        if (reg->entries[i].name == NULL ||
            registry_load_expansion(reg, reg->entries[i].name, dir) != 0) {
            return -1;
        }
        /* Optional reliability sidecar: absent = keep Laplace prior (0.5). */
        if (reg->entries[i].btn != NULL &&
            registry_build_path(&stats_path, dir, reg->entries[i].name,
                                ".stats") == 0) {
            (void)btn_load_stats(reg->entries[i].btn, stats_path);
            free(stats_path);
        }
    }
    return 0;
}

int registry_persist_runtime_state(const PrimitiveRegistry *reg, const char *dir) {
    size_t i;
    char *meta_path = NULL;
    FILE *mf;
    if (reg == NULL || dir == NULL) return -1;
#if defined(_WIN32)
    _mkdir(dir);
#else
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        /* try parent-relative create once; still fail closed on write */
    }
#endif
    for (i = 0; i < reg->count; ++i) {
        char *stats_path = NULL;
        if (reg->entries[i].btn == NULL || reg->entries[i].name == NULL)
            continue;
        if (registry_build_path(&stats_path, dir, reg->entries[i].name,
                                ".stats") != 0)
            return -1;
        if (btn_save_stats(reg->entries[i].btn, stats_path) != 0) {
            free(stats_path);
            return -1;
        }
        free(stats_path);
        if (reg->entries[i].recipe != NULL) {
            char *exp_path = NULL;
            if (registry_build_path(&exp_path, dir, reg->entries[i].name,
                                    ".expansion") != 0)
                return -1;
            if (registry_write_expansion(&reg->entries[i], exp_path) != 0) {
                free(exp_path);
                return -1;
            }
            free(exp_path);
        }
    }
    if (registry_build_path(&meta_path, dir, "registry", ".meta") != 0)
        return -1;
    mf = fopen(meta_path, "w");
    if (!mf) {
        free(meta_path);
        return -1;
    }
    fprintf(mf, "cnet_d_influence %f\n", reg->cnet_d_influence);
    fprintf(mf, "expand_in_low_enabled %d\n", reg->expand_in_low_enabled);
    fprintf(mf, "text_contract_expansion_enabled %d\n",
            reg->text_contract_expansion_enabled);
    fclose(mf);
    free(meta_path);
    return 0;
}

/* ---- lifecycle fault + shadow machinery ------------------------------- */

static int retrain_queue_add_unlabeled(RetrainQueue *q,
                                       const double *input,
                                       const double *raw,
                                       size_t in_count,
                                       size_t out_count) {
    size_t idx;
    if (q->unlabeled_count == q->unlabeled_cap) {
        size_t ncap = q->unlabeled_cap == 0 ? 8 : q->unlabeled_cap * 2;
        double *ni = realloc(q->unlabeled_inputs, ncap * in_count * sizeof(double));
        double *nr = realloc(q->unlabeled_raw,    ncap * out_count * sizeof(double));
        if (!ni || !nr) { free(ni); free(nr); return -1; }
        q->unlabeled_inputs = ni;
        q->unlabeled_raw    = nr;
        q->unlabeled_cap = ncap;
    }
    idx = q->unlabeled_count;
    memcpy(q->unlabeled_inputs + idx * in_count, input, in_count * sizeof(double));
    memcpy(q->unlabeled_raw    + idx * out_count, raw,   out_count * sizeof(double));
    q->unlabeled_count++;
    return 0;
}

static int retrain_queue_add_labeled(RetrainQueue *q,
                                     const double *input,
                                     const double *target,
                                     size_t in_count, size_t out_count) {
    size_t idx;
    if (q->labeled_count == q->labeled_cap) {
        size_t ncap = q->labeled_cap == 0 ? 8 : q->labeled_cap * 2;
        double *ni = realloc(q->labeled_inputs, ncap * in_count * sizeof(double));
        double *nt = realloc(q->labeled_targets, ncap * out_count * sizeof(double));
        if (!ni || !nt) { free(ni); free(nt); return -1; }
        q->labeled_inputs = ni;
        q->labeled_targets = nt;
        q->labeled_cap = ncap;
    }
    idx = q->labeled_count;
    memcpy(q->labeled_inputs + idx*in_count, input, in_count*sizeof(double));
    memcpy(q->labeled_targets + idx*out_count, target, out_count*sizeof(double));
    q->labeled_count++;
    return 0;
}

static void retrain_queue_drop_unlabeled(RetrainQueue *q, size_t r) {
    size_t in = q->input_count;
    size_t out = q->output_count;
    if (r >= q->unlabeled_count) return;
    if (r + 1 < q->unlabeled_count) {
        memmove(q->unlabeled_inputs + r*in,
                q->unlabeled_inputs + (r+1)*in,
                (q->unlabeled_count - r - 1) * in * sizeof(double));
        memmove(q->unlabeled_raw + r*out,
                q->unlabeled_raw + (r+1)*out,
                (q->unlabeled_count - r - 1) * out * sizeof(double));
    }
    q->unlabeled_count--;
}

int registry_record_fault(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *raw_output) {
    size_t i;
    RegistryEntry *e;
    RetrainQueue *q;
    if (reg == NULL || name == NULL || input == NULL || raw_output == NULL) return -1;
    i = registry_find(reg, name);
    if (i == reg->count) return -1;
    e = &reg->entries[i];
    if (e->queue == NULL) {
        q = calloc(1, sizeof *q);
        if (!q) return -1;
        q->input_count = e->btn ? e->btn->input_count : 0;
        q->output_count = e->btn ? e->btn->output_count : 0;
        e->queue = q;
    } else {
        q = e->queue;
    }
    if (retrain_queue_add_unlabeled(q, input, raw_output, q->input_count, q->output_count) != 0)
        return -1;
    e->state = PRIM_RESET;
    return 0;
}

size_t registry_pending_labels(const PrimitiveRegistry *reg, const char *name) {
    size_t i;
    RetrainQueue *q;
    if (!reg || !name) return 0;
    i = registry_find(reg, name);
    if (i == reg->count) return 0;
    q = reg->entries[i].queue;
    return q ? q->unlabeled_count : 0;
}

/* Optional dual-write into the unified fault bus (cnet_fault.c). Weak so
 * binaries that don't link the fault TU stay unchanged. */
void cnet_fault_mirror_labeled(const char *unit, const double *input,
                               const double *target, int in_dim, int out_dim,
                               const char *source_name)
    __attribute__((weak));

int registry_supply_label(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *target) {
    size_t i, j;
    RegistryEntry *e;
    RetrainQueue *q;
    if (!reg || !name || !input || !target) return -1;
    i = registry_find(reg, name);
    if (i == reg->count) return -1;
    e = &reg->entries[i]; q = e->queue;
    if (!q || q->unlabeled_count == 0) return -1;
    for (j = 0; j < q->unlabeled_count; ++j) {
        if (memcmp(q->unlabeled_inputs + j * q->input_count, input,
                   q->input_count * sizeof(double)) == 0) {
            if (retrain_queue_add_labeled(q, input, target, q->input_count, q->output_count) != 0)
                return -1;
            retrain_queue_drop_unlabeled(q, j);
            /* Cross-process bus: labeled pair survives for registry_lora_tick. */
            if (cnet_fault_mirror_labeled) {
                cnet_fault_mirror_labeled(name, input, target,
                                          (int)q->input_count, (int)q->output_count,
                                          "jtc");
            }
            return 0;
        }
    }
    return -1;
}

int registry_add_labeled_pair(PrimitiveRegistry *reg, const char *name,
                              const double *input, const double *target) {
    size_t i;
    RegistryEntry *e;
    RetrainQueue *q;
    if (!reg || !name || !input || !target) return -1;
    i = registry_find(reg, name);
    if (i == reg->count) return -1;
    e = &reg->entries[i];
    if (!e->btn) return -1;
    if (e->queue == NULL) {
        q = calloc(1, sizeof *q);
        if (!q) return -1;
        q->input_count = e->btn->input_count;
        q->output_count = e->btn->output_count;
        e->queue = q;
    } else {
        q = e->queue;
    }
    if (retrain_queue_add_labeled(q, input, target, q->input_count,
                                  q->output_count) != 0)
        return -1;
    return 0;
}

size_t registry_label_via_teacher(PrimitiveRegistry *reg, const char *name) {
    size_t fault_idx, labeled = 0;
    RegistryEntry *e;
    RetrainQueue *q;
    if (!reg || !name) return 0;
    fault_idx = registry_find(reg, name);
    if (fault_idx == reg->count) return 0;
    e = &reg->entries[fault_idx]; q = e->queue;
    if (!q || q->unlabeled_count == 0 || !e->btn) return 0;
    {
        size_t u;
        for (u = 0; u < q->unlabeled_count; ) {
            int adopted = 0;
            size_t t;
            for (t = 0; t < reg->count; ++t) {
                if (t == fault_idx) continue;
                BinaryTransformNetwork *teacher = reg->entries[t].btn;
                if (!teacher || teacher->input_count != q->input_count ||
                    teacher->output_count != q->output_count) continue;
                if (!entry_usable(reg, t)) continue;
                /* run teacher */
                const double *raw = btn_forward(teacher, q->unlabeled_inputs + u * q->input_count);
                if (raw) {
                    /* check every output port segment is valid */
                    int clean = 1;
                    size_t off = 0;
                    size_t p;
                    for (p = 0; p < teacher->output_port_count; ++p) {
                        if (!port_validate(teacher->output_ports[p], raw + off)) { clean=0; break; }
                        off += teacher->output_ports[p].field_width * teacher->output_ports[p].field_count;
                    }
                    if (clean) {
                        double *canon = malloc(q->output_count * sizeof(double));
                        if (canon) {
                            off = 0;
                            for (p=0; p<teacher->output_port_count; ++p) {
                                port_canonicalize(teacher->output_ports[p], raw+off, canon+off);
                                off += teacher->output_ports[p].field_width * teacher->output_ports[p].field_count;
                            }
                            if (retrain_queue_add_labeled(q, q->unlabeled_inputs + u*q->input_count,
                                                          canon, q->input_count, q->output_count) == 0) {
                                retrain_queue_drop_unlabeled(q, u);
                                free(canon);
                                labeled++;
                                adopted = 1;
                                break;
                            }
                            free(canon);
                        }
                    }
                }
            }
            if (!adopted) ++u;
        }
    }
    return labeled;
}

int registry_set_shadow(PrimitiveRegistry *reg, const char *name,
                        const char *active_name) {
    size_t i;
    if (!reg || !name) return -1;
    i = registry_find(reg, name);
    if (i == reg->count) return -1;
    reg->entries[i].shadow_of = active_name; /* may be NULL to clear */
    return 0;
}

size_t registry_run_shadows(PrimitiveRegistry *reg, const char *active_name,
                            const double *input, size_t in_len) {
    size_t run = 0, i;
    if (!reg || !active_name || !input) return 0;
    for (i = 0; i < reg->count; ++i) {
        RegistryEntry *e = &reg->entries[i];
        if (e->shadow_of && strcmp(e->shadow_of, active_name) == 0 && e->btn) {
            if (e->btn->input_count != in_len) continue;
            const double *raw = btn_forward(e->btn, input);
            if (raw && e->btn->output_port_count > 0) {
                if (port_validate(e->btn->output_ports[0], raw)) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
                    atomic_fetch_add_explicit(&e->btn->output_successes, 1, memory_order_relaxed);
#else
                    e->btn->output_successes++;
#endif
                } else {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
                    atomic_fetch_add_explicit(&e->btn->output_failures, 1, memory_order_relaxed);
#else
                    e->btn->output_failures++;
#endif
                }
            }
            run++;
        }
    }
    return run;
}

/* ---- cost ------------------------------------------------------------- */

size_t btn_cost(const BinaryTransformNetwork *btn) {
    if (btn == NULL) return 0;
    if (btn_is_adapter(btn)) return btn->adapter_cost;
    return btn->input_count * btn->hidden_count + btn->hidden_count * btn->output_count;
}

/* btn_mac_estimate was a duplicate of the active implementation in
   dag_full.c; removed to eliminate unused-function warning. */

/* expose a few study-only items that other modules reference via the internal header */
size_t g_circuit_prune_allowed_prim[128];
int    g_circuit_prune_allowed_oj[128];
size_t g_circuit_prune_num_allowed = 0;
int    g_circuit_prune_active = 0;

size_t *g_nodes_expanded_counter = NULL;

int router_same_port_type(Port a, Port b) {
    /* implementation moved to attention/dag as needed; simple version here for compatibility */
    return (a.family == b.family) && (a.field_width == b.field_width);
}




