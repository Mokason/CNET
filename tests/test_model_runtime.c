#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "../include/model_runtime.h"

static int checks;
static int failures;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", msg); } \
} while (0)

typedef struct {
    atomic_int loads;
    atomic_int unloads;
    int delay_ms;
    int fail_load;
    uint64_t actual_extra_bytes;
} MockBackendState;

static int mock_load(void *context,
                     const CnetModelDescriptor *descriptor,
                     uint64_t resource_mask,
                     void **handle_out,
                     uint64_t *resident_bytes_per_resource_out) {
    MockBackendState *state = (MockBackendState *)context;
    (void)resource_mask;
    state->loads++;
    if (state->delay_ms > 0) {
        struct timespec delay;
        delay.tv_sec = state->delay_ms / 1000;
        delay.tv_nsec = (long)(state->delay_ms % 1000) * 1000000L;
        nanosleep(&delay, NULL);
    }
    if (state->fail_load) {
        *handle_out = NULL;
        *resident_bytes_per_resource_out = 0;
        return -1;
    }
    *handle_out = malloc(1);
    *resident_bytes_per_resource_out = descriptor->resident_bytes_per_resource +
                                       state->actual_extra_bytes;
    return *handle_out ? 0 : -1;
}

static void mock_unload(void *context, void *handle) {
    MockBackendState *state = (MockBackendState *)context;
    state->unloads++;
    free(handle);
}

static CnetModelDescriptor descriptor(const char *id,
                                      CnetModelClass model_class,
                                      uint64_t allowed,
                                      uint32_t required_count,
                                      uint64_t bytes) {
    CnetModelDescriptor d;
    memset(&d, 0, sizeof d);
    d.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    d.struct_size = sizeof d;
    snprintf(d.model_id, sizeof d.model_id, "%s", id);
    snprintf(d.backend_name, sizeof d.backend_name, "mock");
    snprintf(d.architecture, sizeof d.architecture, "%s",
             model_class == CNET_MODEL_CLASS_MOE ? "deepseek" : "llama");
    snprintf(d.format, sizeof d.format, "gguf");
    d.model_class = model_class;
    d.capabilities = CNET_MODEL_CAP_TEXT_GENERATION;
    d.allowed_resource_mask = allowed;
    d.preferred_resource_mask = allowed & (~allowed + 1u);
    d.required_resource_count = required_count;
    d.resident_bytes_per_resource = bytes;
    return d;
}

typedef struct {
    CnetModelManager *manager;
    int result;
    CnetModelLease lease;
} AcquireThread;

static void *acquire_thread_main(void *opaque) {
    AcquireThread *thread = (AcquireThread *)opaque;
    memset(&thread->lease, 0, sizeof thread->lease);
    thread->result = cnet_model_acquire(thread->manager, "deduplicated", 0,
                                        &thread->lease);
    return NULL;
}

static void test_dense_and_moe_share_one_catalog(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budgets[] = {
        { CNET_MODEL_RESOURCE_GPU0, 16 },
        { CNET_MODEL_RESOURCE_GPU1, 16 },
    };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor dense;
    CnetModelDescriptor moe;
    CnetModelStats stats;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 8;
    options.max_backends = 4;
    options.budgets = budgets;
    options.budget_count = sizeof budgets / sizeof budgets[0];

    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK && manager,
          "manager opens");

    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "backend registers");

    dense = descriptor("dense-9b", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                       CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 1, 10);
    moe = descriptor("moe-86b", CNET_MODEL_CLASS_MOE,
                     CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 2, 12);
    CHECK(cnet_model_catalog_add(manager, &dense) == CNET_MODEL_OK,
          "dense model enters catalog");
    CHECK(cnet_model_catalog_add(manager, &moe) == CNET_MODEL_OK,
          "MoE model enters same catalog");
    CHECK(cnet_model_manager_model_count(manager) == 2,
          "one catalog owns both model classes");
    CHECK(cnet_model_stats(manager, "dense-9b", &stats) == CNET_MODEL_OK &&
          stats.model_class == CNET_MODEL_CLASS_DENSE_TRANSFORMER &&
          stats.state == CNET_MODEL_STATE_COLD,
          "dense descriptor remains cold after inspection");
    CHECK(cnet_model_stats(manager, "moe-86b", &stats) == CNET_MODEL_OK &&
          stats.model_class == CNET_MODEL_CLASS_MOE &&
          stats.state == CNET_MODEL_STATE_COLD,
          "MoE descriptor remains cold after inspection");

    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK,
          "empty manager closes");
}

static void test_two_dense_models_remain_resident_and_reuse_hot_handles(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budgets[] = {
        { CNET_MODEL_RESOURCE_GPU0, 16 },
        { CNET_MODEL_RESOURCE_GPU1, 16 },
    };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor a;
    CnetModelDescriptor b;
    CnetModelLease lease_a;
    CnetModelLease lease_b;
    CnetModelLease lease_a_hot;
    CnetModelStats stats;
    CnetModelResourceStats resources;
    void *first_handle;
    uint64_t first_generation;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 8;
    options.max_backends = 4;
    options.budgets = budgets;
    options.budget_count = 2;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "dense manager opens");

    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "dense backend registers");

    a = descriptor("dense-a", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                   CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 1, 10);
    b = descriptor("dense-b", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                   CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 1, 10);
    CHECK(cnet_model_catalog_add(manager, &a) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &b) == CNET_MODEL_OK,
          "two dense models enter catalog");

    memset(&lease_a, 0, sizeof lease_a);
    CHECK(cnet_model_acquire(manager, "dense-a", CNET_MODEL_RESOURCE_GPU0,
                             &lease_a) == CNET_MODEL_OK &&
          lease_a.resource_mask == CNET_MODEL_RESOURCE_GPU0 && lease_a.handle,
          "first dense model loads on GPU0");
    first_handle = lease_a.handle;
    first_generation = lease_a.generation;
    CHECK(cnet_model_release(manager, &lease_a) == CNET_MODEL_OK,
          "first dense lease releases but stays resident");

    memset(&lease_b, 0, sizeof lease_b);
    CHECK(cnet_model_acquire(manager, "dense-b", CNET_MODEL_RESOURCE_GPU1,
                             &lease_b) == CNET_MODEL_OK &&
          lease_b.resource_mask == CNET_MODEL_RESOURCE_GPU1 && lease_b.handle,
          "second dense model loads independently on GPU1");
    CHECK(cnet_model_release(manager, &lease_b) == CNET_MODEL_OK,
          "second dense lease releases");

    memset(&lease_a_hot, 0, sizeof lease_a_hot);
    CHECK(cnet_model_acquire(manager, "dense-a", 0, &lease_a_hot) == CNET_MODEL_OK &&
          lease_a_hot.handle == first_handle &&
          lease_a_hot.generation == first_generation,
          "hot dense model reuses its resident handle");
    CHECK(backend_state.loads == 2,
          "two models cause exactly two backend loads");
    CHECK(cnet_model_stats(manager, "dense-a", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_RESIDENT && stats.cache_hits == 1 &&
          stats.lease_count == 1,
          "hot hit and active lease are observable");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU0,
                                    &resources) == CNET_MODEL_OK &&
          resources.resident_bytes == 10 && resources.resident_models == 1,
          "GPU0 accounting has one dense model");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU1,
                                    &resources) == CNET_MODEL_OK &&
          resources.resident_bytes == 10 && resources.resident_models == 1,
          "GPU1 accounting has one dense model");
    CHECK(cnet_model_release(manager, &lease_a_hot) == CNET_MODEL_OK,
          "hot lease releases");
    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK &&
          backend_state.unloads == 2,
          "close unloads both resident dense models");
}

static void test_distributed_moe_atomically_evicts_inactive_dense_models(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budgets[] = {
        { CNET_MODEL_RESOURCE_GPU0, 16 },
        { CNET_MODEL_RESOURCE_GPU1, 16 },
    };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor dense_a;
    CnetModelDescriptor dense_b;
    CnetModelDescriptor moe;
    CnetModelLease lease;
    CnetModelStats stats;
    CnetModelResourceStats resources;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 8;
    options.max_backends = 4;
    options.budgets = budgets;
    options.budget_count = 2;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "MoE manager opens");

    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "MoE backend registers");

    dense_a = descriptor("evict-a", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                         CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 1, 10);
    dense_b = descriptor("evict-b", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                         CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 1, 10);
    moe = descriptor("distributed-moe", CNET_MODEL_CLASS_MOE,
                     CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 2, 12);
    CHECK(cnet_model_catalog_add(manager, &dense_a) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &dense_b) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &moe) == CNET_MODEL_OK,
          "dense and distributed MoE entries coexist");

    CHECK(cnet_model_acquire(manager, "evict-a", CNET_MODEL_RESOURCE_GPU0,
                             &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "inactive dense A occupies GPU0");
    CHECK(cnet_model_acquire(manager, "evict-b", CNET_MODEL_RESOURCE_GPU1,
                             &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "inactive dense B occupies GPU1");

    CHECK(cnet_model_acquire(manager, "distributed-moe",
                             CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                             &lease) == CNET_MODEL_OK &&
          lease.resource_mask ==
              (CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1),
          "MoE admission atomically acquires both R9700 resources");
    CHECK(backend_state.loads == 3 && backend_state.unloads == 2,
          "MoE load evicts exactly the two inactive dense handles");
    CHECK(cnet_model_stats(manager, "evict-a", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_COLD && stats.evictions == 1,
          "dense A becomes cold with an eviction record");
    CHECK(cnet_model_stats(manager, "evict-b", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_COLD && stats.evictions == 1,
          "dense B becomes cold with an eviction record");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU0,
                                    &resources) == CNET_MODEL_OK &&
          resources.resident_bytes == 12 && resources.resident_models == 1,
          "GPU0 accounts for distributed MoE residency");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU1,
                                    &resources) == CNET_MODEL_OK &&
          resources.resident_bytes == 12 && resources.resident_models == 1,
          "GPU1 accounts for distributed MoE residency");
    CHECK(cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "distributed MoE lease releases");
    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK &&
          backend_state.unloads == 3,
          "manager closes the remaining MoE handle");
}

static void test_leases_and_pins_block_unsafe_eviction(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budget = { CNET_MODEL_RESOURCE_GPU0, 16 };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor protected_model;
    CnetModelDescriptor challenger;
    CnetModelLease protected_lease;
    CnetModelLease challenger_lease;
    CnetModelStats stats;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 4;
    options.max_backends = 2;
    options.budgets = &budget;
    options.budget_count = 1;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "protection manager opens");

    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "protection backend registers");

    protected_model = descriptor("protected", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                                 CNET_MODEL_RESOURCE_GPU0, 1, 10);
    challenger = descriptor("challenger", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                            CNET_MODEL_RESOURCE_GPU0, 1, 12);
    CHECK(cnet_model_catalog_add(manager, &protected_model) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &challenger) == CNET_MODEL_OK,
          "protected and challenger models enter catalog");

    CHECK(cnet_model_acquire(manager, "protected", 0, &protected_lease) == CNET_MODEL_OK,
          "protected model loads");
    CHECK(cnet_model_acquire(manager, "challenger", 0, &challenger_lease) == CNET_MODEL_BUSY &&
          backend_state.unloads == 0,
          "active lease blocks eviction");
    CHECK(cnet_model_release(manager, &protected_lease) == CNET_MODEL_OK,
          "protected lease releases");
    CHECK(cnet_model_pin(manager, "protected") == CNET_MODEL_OK,
          "resident model pins");
    CHECK(cnet_model_acquire(manager, "challenger", 0, &challenger_lease) == CNET_MODEL_BUSY &&
          backend_state.unloads == 0,
          "pin blocks eviction without a lease");
    CHECK(cnet_model_unpin(manager, "protected") == CNET_MODEL_OK,
          "resident model unpins");
    CHECK(cnet_model_acquire(manager, "challenger", 0, &challenger_lease) == CNET_MODEL_OK &&
          backend_state.unloads == 1,
          "challenger loads only after protection is removed");
    CHECK(cnet_model_stats(manager, "protected", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_COLD && stats.evictions == 1,
          "formerly protected model is now safely cold");
    CHECK(cnet_model_release(manager, &challenger_lease) == CNET_MODEL_OK &&
          cnet_model_manager_close(manager) == CNET_MODEL_OK,
          "protection manager closes cleanly");
}

static void test_concurrent_cold_acquires_share_one_backend_load(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budget = { CNET_MODEL_RESOURCE_GPU0, 16 };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor model;
    AcquireThread first;
    AcquireThread second;
    CnetModelStats stats;
    pthread_t first_thread;
    pthread_t second_thread;
    struct timespec overlap_delay = {0, 20000000L};

    backend_state.delay_ms = 100;
    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 2;
    options.max_backends = 1;
    options.budgets = &budget;
    options.budget_count = 1;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "dedup manager opens");

    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "dedup backend registers");
    model = descriptor("deduplicated", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                       CNET_MODEL_RESOURCE_GPU0, 1, 10);
    CHECK(cnet_model_catalog_add(manager, &model) == CNET_MODEL_OK,
          "dedup model enters catalog");

    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    first.manager = manager;
    second.manager = manager;
    CHECK(pthread_create(&first_thread, NULL, acquire_thread_main, &first) == 0,
          "first cold acquire starts");
    nanosleep(&overlap_delay, NULL);
    CHECK(pthread_create(&second_thread, NULL, acquire_thread_main, &second) == 0,
          "second overlapping acquire starts");
    pthread_join(first_thread, NULL);
    pthread_join(second_thread, NULL);

    CHECK(first.result == CNET_MODEL_OK && second.result == CNET_MODEL_OK,
          "both cold acquires receive leases");
    CHECK(first.lease.handle == second.lease.handle &&
          first.lease.generation == second.lease.generation,
          "both leases reference one resident generation");
    CHECK(backend_state.loads == 1,
          "concurrent cold acquires invoke one backend load");
    CHECK(cnet_model_stats(manager, "deduplicated", &stats) == CNET_MODEL_OK &&
          stats.loads == 1 && stats.cache_hits == 1 && stats.lease_count == 2,
          "deduplicated load and waiting hit are observable");
    CHECK(cnet_model_release(manager, &first.lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &second.lease) == CNET_MODEL_OK,
          "both shared leases release");
    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK &&
          backend_state.unloads == 1,
          "dedup manager unloads one handle");
}

static void test_failed_and_oversized_loads_leave_no_accounting_leak(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budget = { CNET_MODEL_RESOURCE_GPU0, 16 };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor retryable;
    CnetModelDescriptor oversized;
    CnetModelLease lease;
    CnetModelStats stats;
    CnetModelResourceStats resources;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 4;
    options.max_backends = 1;
    options.budgets = &budget;
    options.budget_count = 1;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "failure manager opens");
    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "failure backend registers");
    retryable = descriptor("retryable", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                           CNET_MODEL_RESOURCE_GPU0, 1, 10);
    oversized = descriptor("oversized", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                           CNET_MODEL_RESOURCE_GPU0, 1, 10);
    CHECK(cnet_model_catalog_add(manager, &retryable) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &oversized) == CNET_MODEL_OK,
          "failure fixtures enter catalog");

    backend_state.fail_load = 1;
    CHECK(cnet_model_acquire(manager, "retryable", 0, &lease) == CNET_MODEL_LOAD_FAILED,
          "backend load failure is explicit");
    CHECK(cnet_model_stats(manager, "retryable", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_FAILED && stats.load_failures == 1,
          "failed state and count are observable");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU0,
                                    &resources) == CNET_MODEL_OK &&
          resources.resident_bytes == 0 && resources.reserved_bytes == 0,
          "failed load releases its reservation");

    backend_state.fail_load = 0;
    CHECK(cnet_model_reset_failure(manager, "retryable") == CNET_MODEL_OK,
          "operator explicitly resets a failed model");
    CHECK(cnet_model_acquire(manager, "retryable", 0, &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "reset model loads successfully");
    CHECK(cnet_model_evict(manager, "retryable") == CNET_MODEL_OK,
          "successful retry can be explicitly evicted");

    backend_state.actual_extra_bytes = 1;
    CHECK(cnet_model_acquire(manager, "oversized", 0, &lease) == CNET_MODEL_OVER_BUDGET,
          "backend usage above its estimate is rejected");
    CHECK(cnet_model_stats(manager, "oversized", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_FAILED && stats.load_failures == 1,
          "oversized model remains failed and nonresident");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU0,
                                    &resources) == CNET_MODEL_OK &&
          resources.resident_bytes == 0 && resources.reserved_bytes == 0 &&
          resources.resident_models == 0,
          "oversized load leaves no resident or reserved bytes");
    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK &&
          backend_state.loads == 3 && backend_state.unloads == 2,
          "failure manager closes with balanced materialized handles");
}

static void test_placement_minimizes_evicted_bytes_after_preference_misses(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budgets[] = {
        { CNET_MODEL_RESOURCE_GPU0, 16 },
        { CNET_MODEL_RESOURCE_GPU1, 16 },
    };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor small;
    CnetModelDescriptor large;
    CnetModelDescriptor challenger;
    CnetModelLease lease;
    CnetModelStats stats;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 6;
    options.max_backends = 1;
    options.budgets = budgets;
    options.budget_count = 2;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "placement manager opens");
    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "placement backend registers");

    small = descriptor("small-victim", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                       CNET_MODEL_RESOURCE_GPU0, 1, 6);
    large = descriptor("large-victim", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                       CNET_MODEL_RESOURCE_GPU1, 1, 10);
    challenger = descriptor("placement-challenger",
                            CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                            CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                            1, 12);
    challenger.preferred_resource_mask = CNET_MODEL_RESOURCE_GPU1;
    CHECK(cnet_model_catalog_add(manager, &small) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &large) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &challenger) == CNET_MODEL_OK,
          "placement fixtures enter catalog");
    CHECK(cnet_model_acquire(manager, "small-victim", 0, &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK &&
          cnet_model_acquire(manager, "large-victim", 0, &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "both candidate resources contain inactive residents");

    CHECK(cnet_model_acquire(manager, "placement-challenger", 0, &lease) ==
              CNET_MODEL_OK && lease.resource_mask == CNET_MODEL_RESOURCE_GPU0,
          "placement evicts fewer bytes after preferred GPU cannot fit cleanly");
    CHECK(cnet_model_stats(manager, "small-victim", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_COLD && stats.evictions == 1,
          "smaller GPU0 victim is selected");
    CHECK(cnet_model_stats(manager, "large-victim", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_RESIDENT && stats.evictions == 0,
          "larger preferred-GPU victim remains resident");
    CHECK(cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "placement challenger lease releases");
    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK &&
          backend_state.loads == 3 && backend_state.unloads == 3,
          "placement manager closes balanced handles");
}

static void test_eviction_uses_lru_not_catalog_order(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budget = { CNET_MODEL_RESOURCE_GPU0, 20 };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor a;
    CnetModelDescriptor b;
    CnetModelDescriptor c;
    CnetModelLease lease;
    CnetModelStats stats;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 5;
    options.max_backends = 1;
    options.budgets = &budget;
    options.budget_count = 1;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "LRU manager opens");
    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "LRU backend registers");

    a = descriptor("lru-a", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                   CNET_MODEL_RESOURCE_GPU0, 1, 10);
    b = descriptor("lru-b", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                   CNET_MODEL_RESOURCE_GPU0, 1, 10);
    c = descriptor("lru-c", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                   CNET_MODEL_RESOURCE_GPU0, 1, 10);
    CHECK(cnet_model_catalog_add(manager, &a) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &b) == CNET_MODEL_OK &&
          cnet_model_catalog_add(manager, &c) == CNET_MODEL_OK,
          "LRU fixtures enter catalog");
    CHECK(cnet_model_acquire(manager, "lru-a", 0, &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK &&
          cnet_model_acquire(manager, "lru-b", 0, &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "two residents fill the resource");
    CHECK(cnet_model_acquire(manager, "lru-a", 0, &lease) == CNET_MODEL_OK &&
          cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "catalog-first model is refreshed as most recently used");

    CHECK(cnet_model_acquire(manager, "lru-c", 0, &lease) == CNET_MODEL_OK,
          "third model is admitted under pressure");
    CHECK(cnet_model_stats(manager, "lru-a", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_RESIDENT && stats.evictions == 0,
          "recently reused catalog-first model remains resident");
    CHECK(cnet_model_stats(manager, "lru-b", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_COLD && stats.evictions == 1,
          "least recently used model is evicted");
    CHECK(cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "LRU challenger lease releases");
    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK &&
          backend_state.loads == 3 && backend_state.unloads == 3,
          "LRU manager closes balanced handles");
}

/* ------------------------------------------------------------------ */
/* 5.1.1 Integrity Slice: Transactional Model Residency              */
/* ------------------------------------------------------------------ */

/* --- Test A: Reentrant unload callback must not deadlock --- */

typedef struct {
    CnetModelManager *manager;
    atomic_int loads;
    atomic_int unloads;
    atomic_int reentry_attempted;
    atomic_int reentry_succeeded;
    int fail_load;
} ReentrantBackendState;

static int reentrant_load(void *context,
                          const CnetModelDescriptor *descriptor,
                          uint64_t resource_mask,
                          void **handle_out,
                          uint64_t *resident_bytes_per_resource_out) {
    ReentrantBackendState *state = (ReentrantBackendState *)context;
    (void)descriptor; (void)resource_mask;
    state->loads++;
    *handle_out = malloc(1);
    *resident_bytes_per_resource_out = 10;
    return *handle_out ? 0 : -1;
}

static void reentrant_unload(void *context, void *handle) {
    ReentrantBackendState *state = (ReentrantBackendState *)context;
    state->unloads++;
    /* Simulate a backend that calls back into the manager during unload.
     * This must not deadlock — the manager mutex must not be held here. */
    atomic_store(&state->reentry_attempted, 1);
    int count = cnet_model_manager_model_count(state->manager);
    if (count >= 0) {
        atomic_store(&state->reentry_succeeded, 1);
    }
    free(handle);
}

static void test_unload_callback_runs_outside_mutex(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budget = { CNET_MODEL_RESOURCE_GPU0, 100 };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    ReentrantBackendState backend_state = {0};
    CnetModelDescriptor model;
    CnetModelLease lease;
    CnetModelStats stats;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 4;
    options.max_backends = 1;
    options.budgets = &budget;
    options.budget_count = 1;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "reentrant manager opens");

    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = reentrant_load;
    backend.unload = reentrant_unload;
    backend_state.manager = manager;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "reentrant backend registers");

    model = descriptor("reentrant-model", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                       CNET_MODEL_RESOURCE_GPU0, 1, 10);
    CHECK(cnet_model_catalog_add(manager, &model) == CNET_MODEL_OK,
          "reentrant model enters catalog");

    CHECK(cnet_model_acquire(manager, "reentrant-model", 0, &lease) == CNET_MODEL_OK,
          "reentrant model loads");
    CHECK(cnet_model_release(manager, &lease) == CNET_MODEL_OK,
          "reentrant lease releases");

    /* Eviction triggers unload; the callback re-enters the manager.
     * If the mutex is held, this deadlocks and the watchdog kills us. */
    CHECK(cnet_model_evict(manager, "reentrant-model") == CNET_MODEL_OK,
          "reentrant model evicts without deadlock");
    CHECK(atomic_load(&backend_state.reentry_attempted) == 1,
          "unload callback was invoked");
    CHECK(atomic_load(&backend_state.reentry_succeeded) == 1,
          "unload callback re-entered manager without deadlock");
    CHECK(cnet_model_stats(manager, "reentrant-model", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_COLD && stats.evictions == 1,
          "reentrant model is cold after eviction");
    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK,
          "reentrant manager closes cleanly");
}

/* --- Test B: Failed relocation preserves prior healthy residency --- */

static void test_failed_relocation_preserves_prior_residency(void) {
    CnetModelManager *manager = NULL;
    CnetModelBudget budgets[] = {
        { CNET_MODEL_RESOURCE_GPU0, 100 },
        { CNET_MODEL_RESOURCE_GPU1, 100 },
    };
    CnetModelManagerOptions options;
    CnetModelBackendSpec backend;
    MockBackendState backend_state = {0};
    CnetModelDescriptor model;
    CnetModelLease lease_a1;
    CnetModelLease lease_a2;
    CnetModelLease failed_lease;
    CnetModelStats stats;
    CnetModelResourceStats res_gpu0;
    CnetModelResourceStats res_gpu1;
    void *original_handle;
    uint64_t original_generation;

    memset(&options, 0, sizeof options);
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 4;
    options.max_backends = 1;
    options.budgets = budgets;
    options.budget_count = 2;
    CHECK(cnet_model_manager_open(&manager, &options) == CNET_MODEL_OK,
          "relocation manager opens");

    memset(&backend, 0, sizeof backend);
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    snprintf(backend.name, sizeof backend.name, "mock");
    backend.context = &backend_state;
    backend.load = mock_load;
    backend.unload = mock_unload;
    CHECK(cnet_model_backend_register(manager, &backend) == CNET_MODEL_OK,
          "relocation backend registers");

    /* Model allowed on both GPU0 and GPU1, requires 1 resource */
    model = descriptor("relocatable", CNET_MODEL_CLASS_DENSE_TRANSFORMER,
                       CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1, 1, 10);
    CHECK(cnet_model_catalog_add(manager, &model) == CNET_MODEL_OK,
          "relocatable model enters catalog");

    /* Load on GPU0, release the lease so it's resident but unleased */
    memset(&lease_a1, 0, sizeof lease_a1);
    CHECK(cnet_model_acquire(manager, "relocatable", CNET_MODEL_RESOURCE_GPU0,
                             &lease_a1) == CNET_MODEL_OK &&
          lease_a1.resource_mask == CNET_MODEL_RESOURCE_GPU0 && lease_a1.handle,
          "model loads on GPU0");
    original_handle = lease_a1.handle;
    original_generation = lease_a1.generation;
    CHECK(cnet_model_release(manager, &lease_a1) == CNET_MODEL_OK,
          "GPU0 lease releases, model stays resident");

    /* Verify residency on GPU0 before relocation attempt */
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU0,
                                    &res_gpu0) == CNET_MODEL_OK &&
          res_gpu0.resident_bytes == 10 && res_gpu0.resident_models == 1,
          "GPU0 has one resident model before relocation");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU1,
                                    &res_gpu1) == CNET_MODEL_OK &&
          res_gpu1.resident_bytes == 0 && res_gpu1.resident_models == 0,
          "GPU1 is empty before relocation");

    /* Now attempt to relocate to GPU1 with load failure.
     * The model is resident on GPU0 with no active lease.
     * Requesting GPU1 (incompatible with current GPU0 residency)
     * forces an unload-then-reload. If the reload fails, the old
     * healthy residency on GPU0 must be preserved. */
    backend_state.fail_load = 1;
    memset(&failed_lease, 0, sizeof failed_lease);
    CHECK(cnet_model_acquire(manager, "relocatable", CNET_MODEL_RESOURCE_GPU1,
                             &failed_lease) == CNET_MODEL_LOAD_FAILED,
          "relocation to GPU1 fails as expected");

    /* The prior healthy residency on GPU0 must be intact */
    CHECK(cnet_model_stats(manager, "relocatable", &stats) == CNET_MODEL_OK,
          "model stats are accessible after failed relocation");

    /* The old handle and generation must be preserved — the model should
     * still be RESIDENT on GPU0, not FAILED or COLD. */
    CHECK(stats.state == CNET_MODEL_STATE_RESIDENT,
          "model remains RESIDENT on GPU0 after failed relocation to GPU1");
    CHECK(stats.load_failures == 1,
          "load failure is recorded for the failed relocation attempt");

    /* Resource accounting: GPU0 should still have the model, GPU1 should be empty */
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU0,
                                    &res_gpu0) == CNET_MODEL_OK &&
          res_gpu0.resident_bytes == 10 && res_gpu0.resident_models == 1,
          "GPU0 accounting preserved after failed relocation");
    CHECK(cnet_model_resource_stats(manager, CNET_MODEL_RESOURCE_GPU1,
                                    &res_gpu1) == CNET_MODEL_OK &&
          res_gpu1.resident_bytes == 0 && res_gpu1.resident_models == 0,
          "GPU1 accounting clean after failed relocation");

    /* The model must be reacquirable on GPU0 with the original handle */
    backend_state.fail_load = 0;
    memset(&lease_a2, 0, sizeof lease_a2);
    CHECK(cnet_model_acquire(manager, "relocatable", CNET_MODEL_RESOURCE_GPU0,
                             &lease_a2) == CNET_MODEL_OK &&
          lease_a2.handle == original_handle &&
          lease_a2.generation == original_generation,
          "model is reacquirable on GPU0 with original handle and generation");
    CHECK(cnet_model_stats(manager, "relocatable", &stats) == CNET_MODEL_OK &&
          stats.state == CNET_MODEL_STATE_RESIDENT && stats.cache_hits == 1,
          "reacquire is a cache hit, not a new load");
    CHECK(cnet_model_release(manager, &lease_a2) == CNET_MODEL_OK,
          "reacquired lease releases");

    CHECK(cnet_model_manager_close(manager) == CNET_MODEL_OK &&
          backend_state.unloads == 1,
          "relocation manager closes with only one unload (the final eviction)");
}

int main(void) {
    test_dense_and_moe_share_one_catalog();
    test_two_dense_models_remain_resident_and_reuse_hot_handles();
    test_distributed_moe_atomically_evicts_inactive_dense_models();
    test_leases_and_pins_block_unsafe_eviction();
    test_concurrent_cold_acquires_share_one_backend_load();
    test_failed_and_oversized_loads_leave_no_accounting_leak();
    test_placement_minimizes_evicted_bytes_after_preference_misses();
    test_eviction_uses_lru_not_catalog_order();
    test_unload_callback_runs_outside_mutex();
    test_failed_relocation_preserves_prior_residency();
    if (failures) {
        fprintf(stderr, "MODEL_RUNTIME_FAIL checks=%d failures=%d\n", checks, failures);
        return 1;
    }
    printf("MODEL_RUNTIME_PASS checks=%d\n", checks);
    return 0;
}
