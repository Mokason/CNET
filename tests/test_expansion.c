/*
 * test_expansion.c -- Dual-Track Expansion Gate (3C, variant A1).
 * A compute-heavy chunk carries the lean sub-plan it replaced (names-only
 * recipe). Under CNET_POWER_LOW + opt-in, the planner hides the chunk so it
 * rebuilds the obligation from the lean primitives; the recipe persists.
 * Planning-only synthetic BTNs (btn_init + ports; the planner reads contracts,
 * no training / no weight files). spec:
 * docs/superpowers/specs/2026-06-19-dual-track-expansion-3c-design.md
 */
#include "../include/nn.h"
#include "../include/router.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int make_btn(BinaryTransformNetwork *b, size_t in, size_t out,
                    Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, input_port, output_port);
}

/* Find a registry entry by name; returns NULL if absent. (RegistryEntry is
   public; the test inspects recipe/cost fields directly.) */
static const RegistryEntry *find_entry(const PrimitiveRegistry *reg, const char *name) {
    size_t i;
    for (i = 0; i < reg->count; ++i)
        if (reg->entries[i].name != NULL && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    return NULL;
}

/* Fixture port shapes: A = ONEHOT16, M = BINARY4, B = BINARY5.
   lo: A->M, hi: M->B, chunk: A->B (one hop, compute-heavy, recipe=[lo,hi]). */
#define A_PORT P(PORT_ONEHOT, 16, 1)
#define M_PORT P(PORT_BINARY_MSB, 4, 1)
#define B_PORT P(PORT_BINARY_MSB, 5, 1)

/* ---- the planner gates: DEFAULT keeps the chunk; LOW+opt-in expands ------- */
static void test_expand_planner_gates(void) {
    BinaryTransformNetwork lo = {0}, hi = {0}, chunk = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    const char *recipe_names[2] = { "lo", "hi" };
    const RegistryEntry *e;

    printf("expansion planner gates:\n");
    if (make_btn(&lo, 16, 4, A_PORT, M_PORT) != 0 ||
        make_btn(&hi, 4, 5, M_PORT, B_PORT) != 0 ||
        make_btn(&chunk, 16, 5, A_PORT, B_PORT) != 0) {
        CHECK(0, "gate setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &lo, "lo");
    registry_add(&reg, &hi, "hi");
    registry_add(&reg, &chunk, "chunk");

    /* attach the recipe + 3A cost truth to the chunk; compute_beneficial=0
       (compute-heavy) -> expand_in_low auto-set. */
    CHECK(registry_set_expansion(&reg, "chunk", recipe_names, 2,
                                 /*teacher*/100, /*student*/900,
                                 /*compute_beneficial*/0) == 0,
          "registry_set_expansion attaches recipe to the chunk");
    e = find_entry(&reg, "chunk");
    CHECK(e != NULL && e->recipe != NULL && e->recipe->primitive_count == 2,
          "chunk entry carries a 2-primitive recipe");
    CHECK(e != NULL && e->expand_in_low == 1,
          "compute-heavy chunk -> expand_in_low set");
    CHECK(e != NULL && e->teacher_mac == 100 && e->student_mac == 900 &&
          e->compute_beneficial == 0,
          "3A cost truth persisted on the entry");

    /* DEFAULT: 1-hop chunk wins (PI 0.5 > 0.25). */
    reg.power_mode = CNET_POWER_DEFAULT;
    reg.expand_in_low_enabled = 0;
    CHECK(route_plan(&reg, A_PORT, B_PORT, &plan) == 0 &&
          plan.length == 1 && strcmp(plan.names[0], "chunk") == 0,
          "DEFAULT keeps the chunk");

    /* LOW but opt-in OFF: still the chunk (the flag is required). */
    reg.power_mode = CNET_POWER_LOW;
    reg.expand_in_low_enabled = 0;
    CHECK(route_plan(&reg, A_PORT, B_PORT, &plan) == 0 &&
          plan.length == 1 && strcmp(plan.names[0], "chunk") == 0,
          "LOW without opt-in keeps the chunk");

    /* opt-in ON but DEFAULT power: still the chunk (power_mode is required). */
    reg.power_mode = CNET_POWER_DEFAULT;
    reg.expand_in_low_enabled = 1;
    CHECK(route_plan(&reg, A_PORT, B_PORT, &plan) == 0 &&
          plan.length == 1 && strcmp(plan.names[0], "chunk") == 0,
          "opt-in without LOW keeps the chunk");

    /* LOW + opt-in: the chunk is hidden -> rebuild from the recipe primitives. */
    reg.power_mode = CNET_POWER_LOW;
    reg.expand_in_low_enabled = 1;
    CHECK(route_plan(&reg, A_PORT, B_PORT, &plan) == 0 &&
          plan.length == 2 &&
          strcmp(plan.names[0], "lo") == 0 &&
          strcmp(plan.names[1], "hi") == 0,
          "LOW + opt-in expands to the recipe primitives");

    registry_free(&reg);
    btn_free(&lo); btn_free(&hi); btn_free(&chunk);
}

/* ---- availability + policy gates ----------------------------------------- */
static void test_expand_availability_and_policy(void) {
    BinaryTransformNetwork hi = {0}, chunk = {0}, cheap = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    const char *recipe_names[2] = { "lo", "hi" };
    const RegistryEntry *e;

    printf("expansion availability + policy:\n");
    if (make_btn(&hi, 4, 5, M_PORT, B_PORT) != 0 ||
        make_btn(&chunk, 16, 5, A_PORT, B_PORT) != 0) {
        CHECK(0, "availability setup");
        return;
    }
    /* registry WITHOUT "lo": the recipe references an absent primitive. */
    registry_init(&reg);
    registry_add(&reg, &hi, "hi");
    registry_add(&reg, &chunk, "chunk");
    registry_set_expansion(&reg, "chunk", recipe_names, 2, 100, 900, 0);
    reg.power_mode = CNET_POWER_LOW;
    reg.expand_in_low_enabled = 1;
    CHECK(route_plan(&reg, A_PORT, B_PORT, &plan) == 0 &&
          plan.length == 1 && strcmp(plan.names[0], "chunk") == 0,
          "unavailable recipe -> chunk stays usable (graceful, no planning failure)");
    registry_free(&reg);
    btn_free(&hi); btn_free(&chunk);

    /* policy: a compute-BENEFICIAL chunk is never expanded. */
    if (make_btn(&cheap, 16, 5, A_PORT, B_PORT) != 0 ||
        make_btn(&hi, 4, 5, M_PORT, B_PORT) != 0) {
        CHECK(0, "policy setup");
        return;
    }
    {
        BinaryTransformNetwork lo = {0};
        if (make_btn(&lo, 16, 4, A_PORT, M_PORT) != 0) { CHECK(0, "policy lo setup"); return; }
        registry_init(&reg);
        registry_add(&reg, &lo, "lo");
        registry_add(&reg, &hi, "hi");
        registry_add(&reg, &cheap, "chunk");
        /* compute_beneficial=1 -> expand_in_low must be 0 */
        registry_set_expansion(&reg, "chunk", recipe_names, 2, 900, 100, 1);
        e = find_entry(&reg, "chunk");
        CHECK(e != NULL && e->expand_in_low == 0,
              "compute-beneficial chunk -> expand_in_low NOT set");
        reg.power_mode = CNET_POWER_LOW;
        reg.expand_in_low_enabled = 1;
        CHECK(route_plan(&reg, A_PORT, B_PORT, &plan) == 0 &&
              plan.length == 1 && strcmp(plan.names[0], "chunk") == 0,
              "LOW + opt-in still keeps a compute-beneficial chunk");
        registry_free(&reg);
        btn_free(&lo);
    }
    btn_free(&hi); btn_free(&cheap);
}

/* ---- persistence round-trip ---------------------------------------------- */
static void test_expand_persistence(void) {
    BinaryTransformNetwork lo = {0}, hi = {0}, chunk = {0};
    PrimitiveRegistry reg;
    const char *recipe_names[2] = { "exp_lo", "exp_hi" };
    FILE *f;

    printf("expansion persistence round-trip:\n");
    if (make_btn(&lo, 16, 4, A_PORT, M_PORT) != 0 ||
        make_btn(&hi, 4, 5, M_PORT, B_PORT) != 0 ||
        make_btn(&chunk, 16, 5, A_PORT, B_PORT) != 0) {
        CHECK(0, "persistence setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &lo, "exp_lo");
    registry_add(&reg, &hi, "exp_hi");
    registry_add(&reg, &chunk, "exp_chunk");
    registry_set_expansion(&reg, "exp_chunk", recipe_names, 2, 100, 900, 0);

    CHECK(registry_save(&reg, "") == 0, "registry_save writes the entries (incl .expansion)");
    f = fopen("exp_chunk.expansion", "r");
    CHECK(f != NULL, "an .expansion sidecar exists for the chunk");
    if (f) fclose(f);
    /* a leaf primitive (no recipe) writes no sidecar */
    f = fopen("exp_lo.expansion", "r");
    CHECK(f == NULL, "no .expansion sidecar for a recipe-less primitive");
    if (f) fclose(f);
    registry_free(&reg);

    /* fresh registry over the SAME btns (borrowed); restore via the sidecar. */
    {
        PrimitiveRegistry reg2;
        RoutePlan plan;
        const RegistryEntry *e;
        registry_init(&reg2);
        registry_add(&reg2, &lo, "exp_lo");
        registry_add(&reg2, &hi, "exp_hi");
        registry_add(&reg2, &chunk, "exp_chunk");

        CHECK(registry_load_expansion(&reg2, "exp_chunk", "") == 0,
              "registry_load_expansion restores the chunk's sidecar");
        e = find_entry(&reg2, "exp_chunk");
        CHECK(e != NULL && e->recipe != NULL && e->recipe->primitive_count == 2 &&
              strcmp(e->recipe->primitives[0], "exp_lo") == 0 &&
              strcmp(e->recipe->primitives[1], "exp_hi") == 0,
              "recipe names round-trip");
        CHECK(e != NULL && e->expand_in_low == 1 &&
              e->teacher_mac == 100 && e->student_mac == 900 &&
              e->compute_beneficial == 0,
              "policy bit + cost truth round-trip");

        /* absent sidecar is a no-op success, not an error */
        CHECK(registry_load_expansion(&reg2, "exp_lo", "") == 0,
              "loading an absent sidecar is a no-op success");
        e = find_entry(&reg2, "exp_lo");
        CHECK(e != NULL && e->recipe == NULL, "recipe-less primitive stays recipe-less");

        /* the restored recipe drives expansion just like the freshly-minted one */
        reg2.power_mode = CNET_POWER_LOW;
        reg2.expand_in_low_enabled = 1;
        CHECK(route_plan(&reg2, A_PORT, B_PORT, &plan) == 0 &&
              plan.length == 2 && strcmp(plan.names[0], "exp_lo") == 0,
              "restored recipe expands under LOW + opt-in");
        registry_free(&reg2);
    }

    remove("exp_lo.cnu"); remove("exp_lo.stats"); remove("exp_lo.expansion");
    remove("exp_hi.cnu"); remove("exp_hi.stats"); remove("exp_hi.expansion");
    remove("exp_chunk.cnu"); remove("exp_chunk.stats"); remove("exp_chunk.expansion");
    btn_free(&lo); btn_free(&hi); btn_free(&chunk);
}

/* ---- robustness: self-referential recipe + over-cap teacher ------------- */
static void test_expand_robustness(void) {
    BinaryTransformNetwork hi = {0}, chunk = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    const RegistryEntry *e;
    const char *self_recipe[2] = { "chunk", "hi" };   /* self-referential */
    const char *big[EXPAND_MAX_PRIMS + 1];
    size_t i;

    printf("expansion robustness (self-ref + over-cap):\n");

    /* A self-referential recipe must NOT hide the chunk into an unplannable
       obligation -- it should degrade gracefully (chunk stays usable). */
    if (make_btn(&hi, 4, 5, M_PORT, B_PORT) != 0 ||
        make_btn(&chunk, 16, 5, A_PORT, B_PORT) != 0) { CHECK(0, "robustness setup"); return; }
    registry_init(&reg);
    registry_add(&reg, &hi, "hi");
    registry_add(&reg, &chunk, "chunk");
    registry_set_expansion(&reg, "chunk", self_recipe, 2, 100, 900, 0);
    reg.power_mode = CNET_POWER_LOW;
    reg.expand_in_low_enabled = 1;
    CHECK(route_plan(&reg, A_PORT, B_PORT, &plan) == 0 &&
          plan.length == 1 && strcmp(plan.names[0], "chunk") == 0,
          "self-referential recipe -> chunk stays usable (no self-hide)");
    registry_free(&reg);
    btn_free(&hi); btn_free(&chunk);

    /* An over-cap teacher (> EXPAND_MAX_PRIMS) attaches NO recipe, and
       expand_in_low must stay 0 (it tracks an actually-attached recipe). */
    if (make_btn(&chunk, 16, 5, A_PORT, B_PORT) != 0) { CHECK(0, "over-cap setup"); return; }
    registry_init(&reg);
    registry_add(&reg, &chunk, "chunk");
    for (i = 0; i < EXPAND_MAX_PRIMS + 1; ++i) big[i] = "x";
    registry_set_expansion(&reg, "chunk", big, EXPAND_MAX_PRIMS + 1, 100, 900, 0);
    e = find_entry(&reg, "chunk");
    CHECK(e != NULL && e->recipe == NULL && e->expand_in_low == 0,
          "over-cap teacher -> no recipe and expand_in_low stays 0");
    registry_free(&reg);
    btn_free(&chunk);
}

int run_test_expansion(void) {
    printf("=== expansion (3C dual-track) ===\n");
    test_expand_planner_gates();
    test_expand_availability_and_policy();
    test_expand_robustness();
    test_expand_persistence();
    if (failures == 0) { printf("\nAll expansion tests passed.\n"); return 0; }
    printf("\n%d expansion test(s) FAILED.\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) { return run_test_expansion(); }
#endif
