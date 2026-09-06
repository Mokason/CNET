/* Internal budget contract, independent of how many capsules admission allows. */
#include "../src/serve/cnet_capsule_core.c"
int main(void) {
    int failures = 0;
    if (admission_work(0) != 2000000 || admission_work(256) != 2000000 ||
        admission_work(257) != 4000000 || admission_work(4096) != 32000000 ||
        admission_work(4097) != 32000000) failures++;
    CnetCapsuleCore core = {0};
    core.registry.count = 65537;
    core.registry.entries = calloc(core.registry.count, sizeof *core.registry.entries);
    if (!core.registry.entries) return 2;
    Port p = {PORT_BINARY_MSB, 1, 1, "alpha"};
    double input = 0;
    CnetCapsuleCoreReply reply = {0}; CoreBudget aggregate;
    budget_init(&aggregate, 2000000);
    if (!search(&core, p, p, &input, &reply, &aggregate) ||
        strcmp(reply.reason, "search_budget_exhausted")) failures++;
    free(core.registry.entries);
    /* Charge lookup work on an admissible indexed fixture, not only an
     * intentionally invalid oversized registry. Equal keys retain tie order. */
    BinaryTransformNetwork indexed_btn[4] = {0}; RegistryEntry indexed_entries[4] = {0};
    const char *tags[] = {"zeta", "alpha", "beta", "alpha"};
    core.registry.count = 4; core.registry.entries = indexed_entries;
    for (size_t i = 0; i < 4; i++) {
        indexed_btn[i].input_port_count = 1;
        indexed_btn[i].input_ports[0] = p;
        snprintf(indexed_btn[i].input_ports[0].tag, PORT_TAG_MAX, "%s", tags[i]);
        indexed_entries[i].btn = &indexed_btn[i];
    }
    if (build_edge_index(&core)) return 2;
    budget_init(&aggregate, 0);
    if (!search(&core, p, p, &input, &reply, &aggregate) ||
        strcmp(reply.reason, "search_budget_exhausted")) failures++;
    size_t begin = 0, end = 0, local = 1;
    budget_init(&aggregate, 2000000);
    if (!edge_range(&core, p, &begin, &end, &aggregate, &local)) failures++;
    budget_init(&aggregate, 2000000); local = 65536;
    if (edge_range(&core, p, &begin, &end, &aggregate, &local) || end-begin != 2 ||
        core.edge_order[begin] != 1 || core.edge_order[begin+1] != 3) failures++;
    /* A demoted/mutated BTN must not corrupt lookup order for other entries. */
    strcpy(indexed_btn[0].input_ports[0].tag, "aardvark");
    if (edge_range(&core, p, &begin, &end, &aggregate, &local) || end-begin != 2) failures++;
    /* Identical labels still cost comparisons: admission must charge them. */
    BinaryTransformNetwork btn = {0};
    btn.input_port_count = btn.output_port_count = 1;
    btn.input_ports[0] = btn.output_ports[0] = p;
    double *rows = calloc(2048, sizeof *rows);
    if (!rows) return 2;
    RegistryCertCoverage labels = {0};
    labels.n_rows = 2048; labels.in_dim = labels.out_dim = 1;
    labels.inputs = labels.targets = rows;
    RegistryEntry entries[2] = {0};
    for (size_t i=0; i<2; i++) { entries[i].btn = &btn; entries[i].cert_cov = &labels; }
    PrimitiveRegistry registry = {0}; registry.entries = entries; registry.count = 2;
    if (contracts_consistent(&registry)) failures++;
    free(rows);
    /* The last coverage bank is indexed by identity, not by import order.
     * Duplicate identities must invalidate the index, never disable a guard. */
    HybridAi first = {0}, last = {0};
    first.coverage_count = last.coverage_count = 1;
    first.coverage[0].active = last.coverage[0].active = 1;
    strcpy(first.coverage[0].unit, "zeta"); strcpy(last.coverage[0].unit, "alpha");
    core.coverage[0] = &first; core.coverage[CORE_COVERAGE_BANKS-1] = &last;
    if (build_guard_index(&core) || core.guard_count != 2 ||
        !find_guard(&core,"alpha") || find_guard(&core,"alpha")->bank != &last ||
        find_guard(&core,"absent")) failures++;
    strcpy(last.coverage[0].unit,"zeta");
    if (!build_guard_index(&core) || core.guards_ready) failures++;
    printf("CAPSULE_CORE_BUDGET_%s failures=%d\n", failures ? "RED" : "PASS", failures);
    return failures != 0;
}
