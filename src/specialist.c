/* The unified Specialist type: one wrap surface, one admission door, and
 * pure axis views over the state the registry already owns. No new state is
 * stored here — trust/role are read from RegistryEntry, residency is mapped
 * from the native tier enums — so these views cannot drift from the truth
 * the planners act on. */

#include <string.h>

#include "../include/specialist.h"
#include "../include/specialist_adapters.h"
#include "../include/cce/cce_contract_adapter.h"
#include "../include/cce/cce_forest.h"
#include "../include/model_runtime.h"

int specialist_wrap_btn(Specialist *s,
                        BinaryTransformNetwork *btn,
                        const char *name) {
    if (!s || !btn || !name || !name[0]) return -1;
    if (btn_is_adapter(btn)) return -1;
    s->kind = SPECIALIST_KIND_BTN;
    s->btn = btn;
    s->name = name;
    s->digest = 0;
    return 0;
}

int specialist_wrap_cce_model(Specialist *s,
                              BinaryTransformNetwork *adapter,
                              struct cce_model *model,
                              int owns_model,
                              const Port *input_ports,
                              size_t input_port_count,
                              const Port *output_ports,
                              size_t output_port_count,
                              unsigned long long behavior_digest,
                              size_t cost_hint,
                              const char *name) {
    if (!s || !adapter || !name || !name[0]) return -1;
    if (cce_model_init_contract_adapter(adapter, model, owns_model,
                                        input_ports, input_port_count,
                                        output_ports, output_port_count,
                                        behavior_digest, cost_hint) != 0)
        return -1;
    s->kind = SPECIALIST_KIND_CCE;
    s->btn = adapter;
    s->name = name;
    s->digest = 0;
    return 0;
}

int specialist_wrap_oracle(Specialist *s,
                           BinaryTransformNetwork *adapter,
                           OracleEntry *entry,
                           unsigned long long behavior_digest,
                           size_t cost_hint,
                           const char *name) {
    if (!s || !adapter || !entry || !name || !name[0]) return -1;
    if (behavior_digest == 0) behavior_digest = entry->behavior_digest;
    if (cnet_oracle_init_contract_adapter(adapter, entry,
                                          behavior_digest, cost_hint) != 0)
        return -1;
    s->kind = SPECIALIST_KIND_ORACLE;
    s->btn = adapter;
    s->name = name;
    s->digest = 0;
    return 0;
}

int specialist_admit(PrimitiveRegistry *reg,
                     Specialist *s,
                     const Contract *c) {
    if (!reg || !s || !s->btn || !s->name || !c) return -1;
    if (s->kind < SPECIALIST_KIND_BTN || s->kind > SPECIALIST_KIND_ORACLE)
        return -1;
    if (registry_add_certified(reg, s->btn, s->name, c) != 0) return -1;
    s->digest = contract_btn_digest(s->btn);
    return 0;
}

static const RegistryEntry *find_entry(const PrimitiveRegistry *reg,
                                       const char *name) {
    size_t i;
    if (!reg || !name) return NULL;
    for (i = 0; i < reg->count; i++)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    return NULL;
}

int specialist_axes(const PrimitiveRegistry *reg,
                    const char *name,
                    SpecialistTrust *trust,
                    SpecialistRole *role) {
    const RegistryEntry *e = find_entry(reg, name);
    if (!e) return -1;
    if (trust) {
        if (e->state == PRIM_RESET)
            *trust = SPECIALIST_TRUST_DEMOTED;   /* RESET overrides FROZEN */
        else if (e->certified && e->state == PRIM_FROZEN)
            *trust = SPECIALIST_TRUST_CERTIFIED;
        else if (e->state == PRIM_PROVISIONAL)
            *trust = SPECIALIST_TRUST_EVIDENCED;
        else
            *trust = SPECIALIST_TRUST_UNCERTIFIED;
    }
    if (role) {
        if (e->shadow_of != NULL)
            *role = SPECIALIST_ROLE_SHADOW;
        else if (e->recipe != NULL)
            *role = SPECIALIST_ROLE_RECIPE;
        else
            *role = SPECIALIST_ROLE_ACTIVE;
    }
    return 0;
}

SpecialistResidency specialist_residency_from_cce_tier(int cce_tier) {
    switch (cce_tier) {
    case 0: return SPECIALIST_RES_HOT;   /* CCE_TIER_HOT */
    case 1: return SPECIALIST_RES_WARM;  /* CCE_TIER_WARM (mmap view) */
    default: return SPECIALIST_RES_COLD; /* CCE_TIER_COLD / unknown */
    }
}

SpecialistResidency specialist_residency_from_model_state(int model_state) {
    switch (model_state) {
    case 2: return SPECIALIST_RES_HOT;   /* CNET_MODEL_STATE_RESIDENT */
    case 1: return SPECIALIST_RES_WARM;  /* CNET_MODEL_STATE_LOADING */
    default: return SPECIALIST_RES_COLD; /* COLD / FAILED / unknown */
    }
}

SpecialistResidency specialist_residency_of_model(
    const struct CnetModelManager *manager, const char *model_id) {
    CnetModelStats stats;
    if (!manager || !model_id ||
        cnet_model_stats(manager, model_id, &stats) != 0)
        return SPECIALIST_RES_COLD;
    return specialist_residency_from_model_state((int)stats.state);
}

SpecialistResidency specialist_residency_of_branch(
    const struct cce_forest *forest, const char *branch_name) {
    int i;
    if (!forest || !branch_name) return SPECIALIST_RES_COLD;
    for (i = 0; i < forest->num_branches; ++i)
        if (strcmp(forest->branches[i].name, branch_name) == 0)
            return specialist_residency_from_cce_tier(
                (int)forest->branches[i].tier);
    return SPECIALIST_RES_COLD;
}

SpecialistResidency specialist_residency_of_entry(const RegistryEntry *entry) {
    return (entry && entry->btn) ? SPECIALIST_RES_HOT : SPECIALIST_RES_COLD;
}

const char *specialist_kind_name(SpecialistKind k) {
    switch (k) {
    case SPECIALIST_KIND_BTN: return "btn";
    case SPECIALIST_KIND_CCE: return "cce";
    case SPECIALIST_KIND_ORACLE: return "oracle";
    default: return "unknown";
    }
}

const char *specialist_trust_name(SpecialistTrust t) {
    switch (t) {
    case SPECIALIST_TRUST_UNCERTIFIED: return "uncertified";
    case SPECIALIST_TRUST_EVIDENCED: return "evidenced";
    case SPECIALIST_TRUST_CERTIFIED: return "certified";
    case SPECIALIST_TRUST_DEMOTED: return "demoted";
    default: return "unknown";
    }
}

const char *specialist_residency_name(SpecialistResidency r) {
    switch (r) {
    case SPECIALIST_RES_HOT: return "hot";
    case SPECIALIST_RES_WARM: return "warm";
    case SPECIALIST_RES_COLD: return "cold";
    default: return "unknown";
    }
}

const char *specialist_role_name(SpecialistRole r) {
    switch (r) {
    case SPECIALIST_ROLE_ACTIVE: return "active";
    case SPECIALIST_ROLE_SHADOW: return "shadow";
    case SPECIALIST_ROLE_RECIPE: return "recipe";
    case SPECIALIST_ROLE_ADVISORY: return "advisory";
    default: return "unknown";
    }
}
