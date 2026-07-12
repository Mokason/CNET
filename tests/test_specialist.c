#include <stdio.h>
#include <string.h>

#include "../include/specialist.h"
#include "../include/model_runtime.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"

static int checks;
static int failures;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static void test_registry_init_clears_streamer(void) {
    PrimitiveRegistry reg;
    memset(&reg, 0xa5, sizeof reg);

    registry_init(&reg);

    check(reg.streamer == NULL,
          "registry_init clears the optional streaming callback");
    registry_free(&reg);
}

static int identity_forward(void *context,
                            const double *input, size_t input_count,
                            double *output, size_t output_count) {
    (void)context;
    if (!input || !output || input_count != 1 || output_count != 1) return -1;
    output[0] = input[0];
    return 0;
}

static void test_admit_rejects_unknown_kind(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork adapter;
    Contract contract;
    Specialist specialist;
    const Port bit = { PORT_BINARY_MSB, 1, 1, "bit" };
    const double inputs[] = { 0.0, 1.0 };
    const double targets[] = { 0.0, 1.0 };

    memset(&adapter, 0, sizeof adapter);
    memset(&contract, 0, sizeof contract);
    memset(&specialist, 0, sizeof specialist);
    registry_init(&reg);

    if (btn_init_adapter(&adapter, 1, 1, &bit, 1, &bit, 1,
                         identity_forward, NULL, NULL,
                         0x535045435f554e49ULL, 1) != 0 ||
        contract_init_borrowed(&contract, "invalid_kind", &adapter,
                               inputs, targets, 2) != 0) {
        check(0, "invalid-kind fixture initializes");
        registry_free(&reg);
        btn_free(&adapter);
        return;
    }

    specialist.kind = (SpecialistKind)99;
    specialist.btn = &adapter;
    specialist.name = "invalid_kind";
    check(specialist_admit(&reg, &specialist, &contract) != 0,
          "the admission door rejects an unknown SpecialistKind");
    check(reg.count == 0,
          "unknown-kind refusal leaves the registry unchanged");

    registry_free(&reg);
    contract_free(&contract);
    btn_free(&adapter);
}

static void test_axes_follow_lifecycle_state(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork adapter;
    Contract contract;
    Specialist specialist;
    SpecialistTrust trust = SPECIALIST_TRUST_UNCERTIFIED;
    SpecialistRole role = SPECIALIST_ROLE_ADVISORY;
    const Port bit = { PORT_BINARY_MSB, 1, 1, "axis_bit" };
    const double inputs[] = { 0.0, 1.0 };
    const double targets[] = { 0.0, 1.0 };

    memset(&adapter, 0, sizeof adapter);
    memset(&contract, 0, sizeof contract);
    memset(&specialist, 0, sizeof specialist);
    registry_init(&reg);

    if (btn_init_adapter(&adapter, 1, 1, &bit, 1, &bit, 1,
                         identity_forward, NULL, NULL,
                         0x535045435f415849ULL, 1) != 0 ||
        contract_init_borrowed(&contract, "axis_identity", &adapter,
                               inputs, targets, 2) != 0) {
        check(0, "axes fixture initializes");
        registry_free(&reg);
        btn_free(&adapter);
        return;
    }

    specialist.kind = SPECIALIST_KIND_ORACLE;
    specialist.btn = &adapter;
    specialist.name = "axis_identity";
    check(specialist_admit(&reg, &specialist, &contract) == 0,
          "valid SpecialistKind admits");
    check(specialist_axes(&reg, "axis_identity", &trust, &role) == 0 &&
          trust == SPECIALIST_TRUST_CERTIFIED &&
          role == SPECIALIST_ROLE_ACTIVE,
          "freshly admitted entry is certified and active");

    check(registry_set_state(&reg, "axis_identity", PRIM_PROVISIONAL) == 0 &&
          specialist_axes(&reg, "axis_identity", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_EVIDENCED,
          "PROVISIONAL state is evidenced even when certification history remains");
    check(registry_set_state(&reg, "axis_identity", PRIM_RESET) == 0 &&
          specialist_axes(&reg, "axis_identity", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_DEMOTED,
          "RESET state overrides certification history");
    check(specialist_axes(&reg, "missing", &trust, &role) != 0,
          "axes rejects an unknown registry name");

    registry_free(&reg);
    contract_free(&contract);
    btn_free(&adapter);
}

static void test_specialist_edge_refusals_and_atoms(void) {
    Specialist specialist;
    BinaryTransformNetwork adapter;
    const Port bit = { PORT_BINARY_MSB, 1, 1, "edge_bit" };

    memset(&specialist, 0, sizeof specialist);
    memset(&adapter, 0, sizeof adapter);
    check(specialist_wrap_btn(NULL, &adapter, "x") != 0 &&
          specialist_wrap_btn(&specialist, NULL, "x") != 0 &&
          specialist_wrap_btn(&specialist, &adapter, NULL) != 0,
          "BTN wrapper rejects NULL arguments");

    check(btn_init_adapter(&adapter, 1, 1, &bit, 1, &bit, 1,
                           identity_forward, NULL, NULL,
                           0x535045435f454447ULL, 1) == 0,
          "edge adapter initializes");
    check(specialist_wrap_btn(&specialist, &adapter, "adapter") != 0,
          "native BTN wrapper refuses a runtime adapter");
    check(specialist_residency_from_cce_tier(-1) == SPECIALIST_RES_COLD &&
          specialist_residency_from_model_state(-1) == SPECIALIST_RES_COLD,
          "unknown residency values fail conservatively to cold");
    check(strcmp(specialist_kind_name((SpecialistKind)99), "unknown") == 0 &&
          strcmp(specialist_trust_name((SpecialistTrust)99), "unknown") == 0 &&
          strcmp(specialist_residency_name((SpecialistResidency)99), "unknown") == 0 &&
          strcmp(specialist_role_name((SpecialistRole)99), "unknown") == 0,
          "out-of-range axis atoms are explicit unknowns");
    btn_free(&adapter);
}

static int fixture_backend_load(void *ctx,
                                const CnetModelDescriptor *descriptor,
                                uint64_t resource_mask, void **handle_out,
                                uint64_t *resident_bytes_out) {
    (void)ctx; (void)descriptor; (void)resource_mask;
    *handle_out = (void *)0x1;
    *resident_bytes_out = 1024;
    return 0;
}

static void fixture_backend_unload(void *ctx, void *handle) {
    (void)ctx; (void)handle;
}

static void test_residency_truth(void) {
    /* forest truth: the branch's LIVE tier, not a caller-supplied enum */
    {
        cce_forest *forest = NULL;
        cce_cascade cascade;
        cce_block blk;
        memset(&cascade, 0, sizeof cascade);
        memset(&blk, 0, sizeof blk);
        remove("tmp_specialist_res.cce");
        check(cce_cascade_init(&cascade, 2) == CCE_OK &&
              cce_block_init_linear(&blk, 2, 2, 0.01f) == CCE_OK &&
              cce_cascade_append(&cascade, &blk) == CCE_OK &&
              cce_forest_open(&forest, "tmp_specialist_res.cce", 2) == CCE_OK &&
              cce_forest_add_branch(forest, &cascade, "res-truth") == CCE_OK,
              "residency fixture forest builds");
        check(specialist_residency_of_branch(forest, "res-truth") ==
                  SPECIALIST_RES_HOT,
              "a freshly added branch reads hot from the live forest");
        forest->branches[0].tier = CCE_TIER_WARM;
        check(specialist_residency_of_branch(forest, "res-truth") ==
                  SPECIALIST_RES_WARM,
              "the view follows the branch tier as it changes");
        check(specialist_residency_of_branch(forest, "absent") ==
                  SPECIALIST_RES_COLD,
              "an unknown branch is conservatively cold");
        cce_forest_close(forest);
        remove("tmp_specialist_res.cce");
    }

    /* model-catalog truth: the manager's LIVE state across a lease cycle */
    {
        CnetModelManager *mgr = NULL;
        CnetModelManagerOptions opt;
        CnetModelBackendSpec backend;
        CnetModelDescriptor d;
        CnetModelBudget budget;
        CnetModelLease lease;
        memset(&opt, 0, sizeof opt);
        memset(&backend, 0, sizeof backend);
        memset(&d, 0, sizeof d);
        memset(&budget, 0, sizeof budget);
        opt.abi_version = 1;
        opt.struct_size = (uint32_t)sizeof opt;
        opt.max_models = 4;
        opt.max_backends = 4;
        budget.resource_mask = CNET_MODEL_RESOURCE_CPU;
        budget.budget_bytes = 1 << 20;
        opt.budgets = &budget;
        opt.budget_count = 1;
        check(cnet_model_manager_open(&mgr, &opt) == 0,
              "residency fixture manager opens");
        backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
        backend.struct_size = (uint32_t)sizeof backend;
        snprintf(backend.name, sizeof backend.name, "fixture");
        backend.load = fixture_backend_load;
        backend.unload = fixture_backend_unload;
        check(cnet_model_backend_register(mgr, &backend) == 0,
              "fixture backend registers");
        d.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
        d.struct_size = (uint32_t)sizeof d;
        snprintf(d.model_id, sizeof d.model_id, "res_truth_model");
        snprintf(d.backend_name, sizeof d.backend_name, "fixture");
        d.model_class = CNET_MODEL_CLASS_DENSE_TRANSFORMER;
        d.resident_bytes_per_resource = 1024;
        d.allowed_resource_mask = CNET_MODEL_RESOURCE_CPU;
        d.preferred_resource_mask = CNET_MODEL_RESOURCE_CPU;
        d.required_resource_count = 1;
        check(cnet_model_catalog_add(mgr, &d) == 0, "descriptor enters catalog");
        check(specialist_residency_of_model(mgr, "res_truth_model") ==
                  SPECIALIST_RES_COLD,
              "an unloaded model reads cold from the live catalog");
        check(cnet_model_acquire(mgr, "res_truth_model",
                                 CNET_MODEL_RESOURCE_CPU, &lease) == 0 &&
              specialist_residency_of_model(mgr, "res_truth_model") ==
                  SPECIALIST_RES_HOT,
              "a leased model reads hot from the live catalog");
        check(cnet_model_release(mgr, &lease) == 0 &&
              cnet_model_evict(mgr, "res_truth_model") == 0 &&
              specialist_residency_of_model(mgr, "res_truth_model") ==
                  SPECIALIST_RES_COLD,
              "an evicted model reads cold again");
        check(specialist_residency_of_model(mgr, "never_registered") ==
                  SPECIALIST_RES_COLD,
              "an unknown model is conservatively cold");
        cnet_model_manager_close(mgr);
    }

    /* entry truth: mechanism residency is independent of planner trust */
    {
        RegistryEntry e;
        BinaryTransformNetwork b;
        memset(&e, 0, sizeof e);
        memset(&b, 0, sizeof b);
        check(specialist_residency_of_entry(&e) == SPECIALIST_RES_COLD &&
              specialist_residency_of_entry(NULL) == SPECIALIST_RES_COLD,
              "an entry without a node is cold");
        e.btn = &b;
        e.state = PRIM_RESET;
        check(specialist_residency_of_entry(&e) == SPECIALIST_RES_HOT,
              "a resident node stays hot while RESET trust blocks planning");
    }
}

int main(void) {
    test_registry_init_clears_streamer();
    test_admit_rejects_unknown_kind();
    test_axes_follow_lifecycle_state();
    test_specialist_edge_refusals_and_atoms();

    test_residency_truth();
    if (failures != 0) {
        fprintf(stderr, "SPECIALIST_UNIT_FAIL checks=%d failures=%d\n",
                checks, failures);
        return 1;
    }
    printf("SPECIALIST_UNIT_PASS checks=%d\n", checks);
    return 0;
}
