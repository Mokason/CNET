#include "../include/model_runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    CnetModelDescriptor descriptor;
    CnetModelState state;
    void *handle;
    uint64_t resource_mask;
    uint64_t resident_bytes;
    uint64_t generation;
    uint64_t last_use_tick;
    uint32_t backend_index;
    uint32_t lease_count;
    uint32_t pin_count;
    uint32_t transaction_pin_count;
    uint32_t loads;
    uint32_t cache_hits;
    uint32_t evictions;
    uint32_t load_failures;
    pthread_cond_t changed;
    int condition_initialized;
} CnetModelEntry;

typedef struct {
    CnetModelBackendSpec spec;
} CnetBackendEntry;

typedef struct {
    uint64_t mask;
    uint64_t budget_bytes;
    uint64_t resident_bytes;
    uint64_t reserved_bytes;
    uint32_t resident_models;
} CnetResourceEntry;

struct CnetModelManager {
    pthread_mutex_t mutex;
    CnetModelEntry *models;
    CnetBackendEntry *backends;
    CnetResourceEntry *resources;
    uint32_t model_count;
    uint32_t backend_count;
    uint32_t resource_count;
    uint32_t max_models;
    uint32_t max_backends;
    uint64_t next_generation;
    uint64_t use_tick;
};

static int has_terminator(const char *value, size_t capacity) {
    return value && memchr(value, '\0', capacity) != NULL;
}

static uint32_t count_bits(uint64_t value) {
    uint32_t count = 0;
    while (value) {
        value &= value - 1u;
        count++;
    }
    return count;
}

static int is_single_bit(uint64_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static int descriptor_is_valid(const CnetModelDescriptor *descriptor) {
    if (!descriptor ||
        descriptor->abi_version != CNET_MODEL_RUNTIME_ABI_VERSION ||
        descriptor->struct_size < sizeof *descriptor ||
        !has_terminator(descriptor->model_id, sizeof descriptor->model_id) ||
        !has_terminator(descriptor->backend_name, sizeof descriptor->backend_name) ||
        descriptor->model_id[0] == '\0' || descriptor->backend_name[0] == '\0' ||
        descriptor->allowed_resource_mask == 0 ||
        descriptor->required_resource_count == 0 ||
        descriptor->required_resource_count >
            count_bits(descriptor->allowed_resource_mask) ||
        (descriptor->preferred_resource_mask &
         ~descriptor->allowed_resource_mask) != 0 ||
        descriptor->model_class < CNET_MODEL_CLASS_OTHER ||
        descriptor->model_class > CNET_MODEL_CLASS_EMBEDDING) {
        return 0;
    }
    return 1;
}

static int find_model_locked(const CnetModelManager *manager,
                             const char *model_id) {
    uint32_t index;
    if (!model_id) return -1;
    for (index = 0; index < manager->model_count; index++) {
        if (strcmp(manager->models[index].descriptor.model_id, model_id) == 0)
            return (int)index;
    }
    return -1;
}

static int find_backend_locked(const CnetModelManager *manager,
                               const char *backend_name) {
    uint32_t index;
    for (index = 0; index < manager->backend_count; index++) {
        if (strcmp(manager->backends[index].spec.name, backend_name) == 0)
            return (int)index;
    }
    return -1;
}

static int find_resource_locked(const CnetModelManager *manager,
                                uint64_t resource_mask) {
    uint32_t index;
    for (index = 0; index < manager->resource_count; index++) {
        if (manager->resources[index].mask == resource_mask)
            return (int)index;
    }
    return -1;
}

static uint64_t managed_resource_mask_locked(const CnetModelManager *manager) {
    uint64_t mask = 0;
    uint32_t index;
    for (index = 0; index < manager->resource_count; index++)
        mask |= manager->resources[index].mask;
    return mask;
}

static void remove_residency_locked(CnetModelManager *manager,
                                    CnetModelEntry *entry) {
    uint32_t index;
    for (index = 0; index < manager->resource_count; index++) {
        CnetResourceEntry *resource = &manager->resources[index];
        if ((entry->resource_mask & resource->mask) == 0) continue;
        if (resource->resident_bytes >= entry->resident_bytes)
            resource->resident_bytes -= entry->resident_bytes;
        else
            resource->resident_bytes = 0;
        if (resource->resident_models > 0) resource->resident_models--;
    }
}

/* Deferred-unload list: collects handles that must be released via
 * backend->unload() *outside* the manager mutex.  This prevents
 * deadlocks when a backend unload callback re-enters the manager. */
typedef struct {
    void *handle;
    CnetModelBackendUnloadFn unload_fn;
    void *backend_context;
} DeferredUnload;

static void deferred_unload_execute(const DeferredUnload *list, uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (list[i].handle && list[i].unload_fn)
            list[i].unload_fn(list[i].backend_context, list[i].handle);
    }
}

/* detach_entry_locked: removes residency accounting and transitions the entry
 * to COLD, but does NOT call backend->unload().  The caller must collect the
 * handle into a deferred-unload list and invoke it outside the mutex. */
static void detach_entry_locked(CnetModelManager *manager,
                                CnetModelEntry *entry,
                                int record_eviction,
                                DeferredUnload *deferred_out) {
    CnetModelBackendSpec *backend;
    if (entry->state != CNET_MODEL_STATE_RESIDENT || !entry->handle) return;
    backend = &manager->backends[entry->backend_index].spec;
    remove_residency_locked(manager, entry);
    if (deferred_out) {
        deferred_out->handle = entry->handle;
        deferred_out->unload_fn = backend->unload;
        deferred_out->backend_context = backend->context;
    }
    entry->handle = NULL;
    entry->resource_mask = 0;
    entry->resident_bytes = 0;
    entry->state = CNET_MODEL_STATE_COLD;
    if (record_eviction) entry->evictions++;
    pthread_cond_broadcast(&entry->changed);
}

typedef enum {
    CANDIDATE_BLOCKED = 0,
    CANDIDATE_OK = 1,
    CANDIDATE_INTRINSIC_OVER_BUDGET = -1
} CandidateResult;

typedef struct {
    CnetModelManager *manager;
    uint32_t target_index;
    uint64_t estimated_bytes;
    uint64_t preference_mask;
    uint64_t bits[CNET_MODEL_MAX_RESOURCES];
    uint32_t bit_count;
    uint32_t required_count;
    uint8_t *work_evictions;
    uint8_t *best_evictions;
    uint64_t best_mask;
    uint64_t best_evicted_bytes;
    uint64_t best_free_capacity;
    uint64_t combinations;
    int best_is_clean_preference;
    int found;
    int saw_budget_capable_candidate;
} CandidateSearch;

static int shortages_remain(const CnetModelManager *manager,
                             const uint64_t *shortage) {
    uint32_t resource_index;
    for (resource_index = 0; resource_index < manager->resource_count;
         resource_index++) {
        if (shortage[resource_index] != 0) return 1;
    }
    return 0;
}

static int victim_contributes(const CnetModelManager *manager,
                              const CnetModelEntry *entry,
                              const uint64_t *shortage) {
    uint32_t resource_index;
    for (resource_index = 0; resource_index < manager->resource_count;
         resource_index++) {
        if (shortage[resource_index] != 0 &&
            (entry->resource_mask & manager->resources[resource_index].mask) != 0)
            return 1;
    }
    return 0;
}

static CandidateResult evaluate_candidate(CandidateSearch *search,
                                          uint64_t candidate_mask,
                                          uint64_t *evicted_bytes_out,
                                          uint64_t *free_capacity_out) {
    CnetModelManager *manager = search->manager;
    uint64_t shortage[CNET_MODEL_MAX_RESOURCES];
    uint64_t evicted_bytes = 0;
    uint64_t free_capacity = 0;
    uint32_t resource_index;
    int budget_capable = 1;

    memset(search->work_evictions, 0, manager->max_models);
    memset(shortage, 0, sizeof shortage);

    for (resource_index = 0; resource_index < manager->resource_count;
         resource_index++) {
        CnetResourceEntry *resource = &manager->resources[resource_index];
        uint64_t used;
        if ((candidate_mask & resource->mask) == 0) continue;
        if (search->estimated_bytes > resource->budget_bytes) {
            budget_capable = 0;
            break;
        }
        used = resource->resident_bytes + resource->reserved_bytes;
        if (used < resource->budget_bytes)
            free_capacity += resource->budget_bytes - used;
        if (used + search->estimated_bytes > resource->budget_bytes)
            shortage[resource_index] =
                used + search->estimated_bytes - resource->budget_bytes;
    }
    if (!budget_capable) return CANDIDATE_INTRINSIC_OVER_BUDGET;
    search->saw_budget_capable_candidate = 1;

    while (shortages_remain(manager, shortage)) {
        uint32_t model_index;
        uint32_t victim_index = manager->model_count;
        uint64_t oldest_tick = UINT64_MAX;
        CnetModelEntry *victim;

        for (model_index = 0; model_index < manager->model_count; model_index++) {
            CnetModelEntry *entry;
            if (model_index == search->target_index ||
                search->work_evictions[model_index] != 0)
                continue;
            entry = &manager->models[model_index];
            if (entry->state != CNET_MODEL_STATE_RESIDENT ||
                entry->lease_count != 0 || entry->pin_count != 0 ||
                entry->transaction_pin_count != 0 ||
                !victim_contributes(manager, entry, shortage))
                continue;
            if (victim_index == manager->model_count ||
                entry->last_use_tick < oldest_tick ||
                (entry->last_use_tick == oldest_tick && model_index < victim_index)) {
                victim_index = model_index;
                oldest_tick = entry->last_use_tick;
            }
        }
        if (victim_index == manager->model_count) return CANDIDATE_BLOCKED;

        victim = &manager->models[victim_index];
        search->work_evictions[victim_index] = 1;
        {
            uint32_t overlap = count_bits(victim->resource_mask & candidate_mask);
            if (overlap != 0 &&
                victim->resident_bytes <= UINT64_MAX / (uint64_t)overlap &&
                evicted_bytes <= UINT64_MAX -
                    victim->resident_bytes * (uint64_t)overlap) {
                evicted_bytes += victim->resident_bytes * (uint64_t)overlap;
            } else {
                evicted_bytes = UINT64_MAX;
            }
        }
        for (resource_index = 0; resource_index < manager->resource_count;
             resource_index++) {
            if ((victim->resource_mask &
                 manager->resources[resource_index].mask) == 0)
                continue;
            if (shortage[resource_index] <= victim->resident_bytes)
                shortage[resource_index] = 0;
            else
                shortage[resource_index] -= victim->resident_bytes;
        }
    }

    *evicted_bytes_out = evicted_bytes;
    *free_capacity_out = free_capacity;
    return CANDIDATE_OK;
}

static void consider_candidate(CandidateSearch *search,
                               uint64_t candidate_mask) {
    uint64_t evicted_bytes = 0;
    uint64_t free_capacity = 0;
    int clean_preference;
    int is_better;
    CandidateResult result;

    if (search->combinations >= 1000000u) return;
    search->combinations++;
    result = evaluate_candidate(search, candidate_mask, &evicted_bytes,
                                &free_capacity);
    if (result != CANDIDATE_OK) return;

    clean_preference = search->preference_mask != 0 && evicted_bytes == 0 &&
                       (candidate_mask & ~search->preference_mask) == 0;
    is_better = !search->found ||
                (clean_preference && !search->best_is_clean_preference) ||
                (clean_preference == search->best_is_clean_preference &&
                 (evicted_bytes < search->best_evicted_bytes ||
                  (evicted_bytes == search->best_evicted_bytes &&
                   (free_capacity > search->best_free_capacity ||
                    (free_capacity == search->best_free_capacity &&
                     candidate_mask < search->best_mask)))));
    if (is_better) {
        search->found = 1;
        search->best_is_clean_preference = clean_preference;
        search->best_evicted_bytes = evicted_bytes;
        search->best_free_capacity = free_capacity;
        search->best_mask = candidate_mask;
        memcpy(search->best_evictions, search->work_evictions,
               search->manager->max_models);
    }
}

static void enumerate_candidates(CandidateSearch *search,
                                 uint32_t start,
                                 uint32_t remaining,
                                 uint64_t current_mask) {
    uint32_t index;
    if (search->combinations >= 1000000u) return;
    if (remaining == 0) {
        consider_candidate(search, current_mask);
        return;
    }
    if (search->bit_count - start < remaining) return;
    for (index = start; index <= search->bit_count - remaining; index++) {
        enumerate_candidates(search, index + 1u, remaining - 1u,
                             current_mask | search->bits[index]);
        if (search->combinations >= 1000000u) return;
    }
}

static int choose_resources_locked(CnetModelManager *manager,
                                   uint32_t target_index,
                                   uint64_t requested_mask,
                                   uint64_t *selected_mask_out,
                                   uint8_t *evictions_out) {
    CnetModelEntry *entry = &manager->models[target_index];
    CandidateSearch search;
    uint64_t managed_mask = managed_resource_mask_locked(manager);
    uint64_t eligible_mask;
    uint64_t bit;
    uint32_t index;

    if ((requested_mask & ~entry->descriptor.allowed_resource_mask) != 0)
        return CNET_MODEL_INVALID;
    eligible_mask = entry->descriptor.allowed_resource_mask & managed_mask;
    if (requested_mask != 0) eligible_mask &= requested_mask;
    if (count_bits(eligible_mask) < entry->descriptor.required_resource_count)
        return CNET_MODEL_OVER_BUDGET;

    memset(&search, 0, sizeof search);
    search.manager = manager;
    search.target_index = target_index;
    search.estimated_bytes = entry->descriptor.resident_bytes_per_resource;
    search.preference_mask = requested_mask != 0 ? requested_mask :
                             entry->descriptor.preferred_resource_mask;
    search.required_count = entry->descriptor.required_resource_count;
    search.work_evictions = calloc(manager->max_models, 1);
    search.best_evictions = calloc(manager->max_models, 1);
    if (!search.work_evictions || !search.best_evictions) {
        free(search.work_evictions);
        free(search.best_evictions);
        return CNET_MODEL_INVALID;
    }

    index = 0;
    for (bit = 1u; bit != 0 && index < CNET_MODEL_MAX_RESOURCES; bit <<= 1u) {
        if ((eligible_mask & bit) != 0) search.bits[index++] = bit;
    }
    search.bit_count = index;
    enumerate_candidates(&search, 0, search.required_count, 0);

    if (search.found) {
        *selected_mask_out = search.best_mask;
        memcpy(evictions_out, search.best_evictions, manager->max_models);
        free(search.work_evictions);
        free(search.best_evictions);
        return CNET_MODEL_OK;
    }
    free(search.work_evictions);
    free(search.best_evictions);
    return search.saw_budget_capable_candidate ? CNET_MODEL_BUSY :
                                                 CNET_MODEL_OVER_BUDGET;
}

static int lease_is_compatible(const CnetModelEntry *entry,
                               uint64_t requested_mask) {
    if (requested_mask == 0) return 1;
    return (entry->resource_mask & ~requested_mask) == 0;
}

static void fill_lease(const CnetModelEntry *entry, CnetModelLease *lease) {
    memset(lease, 0, sizeof *lease);
    snprintf(lease->model_id, sizeof lease->model_id, "%s",
             entry->descriptor.model_id);
    lease->handle = entry->handle;
    lease->generation = entry->generation;
    lease->resource_mask = entry->resource_mask;
}

static void release_transaction_pins_locked(CnetModelManager *manager,
                                            const uint8_t *evictions) {
    uint32_t index;
    for (index = 0; index < manager->model_count; index++) {
        CnetModelEntry *entry;
        if (!evictions[index]) continue;
        entry = &manager->models[index];
        if (entry->transaction_pin_count > 0) entry->transaction_pin_count--;
        pthread_cond_broadcast(&entry->changed);
    }
}

static int projected_residency_fits_locked(
    const CnetModelManager *manager,
    uint64_t selected_mask,
    const uint8_t *evictions,
    int is_relocation,
    uint64_t old_resource_mask,
    uint64_t old_resident_bytes,
    uint64_t actual_bytes) {
    uint32_t resource_index;
    for (resource_index = 0; resource_index < manager->resource_count;
         resource_index++) {
        const CnetResourceEntry *resource = &manager->resources[resource_index];
        uint64_t reclaim = 0;
        uint64_t used;
        uint32_t model_index;
        if ((selected_mask & resource->mask) == 0) continue;
        if (is_relocation && (old_resource_mask & resource->mask) != 0)
            reclaim = old_resident_bytes;
        for (model_index = 0; model_index < manager->model_count; model_index++) {
            const CnetModelEntry *victim;
            if (!evictions[model_index]) continue;
            victim = &manager->models[model_index];
            if ((victim->resource_mask & resource->mask) == 0) continue;
            if (UINT64_MAX - reclaim < victim->resident_bytes) return 0;
            reclaim += victim->resident_bytes;
        }
        if (resource->resident_bytes < reclaim) return 0;
        used = resource->resident_bytes - reclaim;
        if (UINT64_MAX - used < resource->reserved_bytes) return 0;
        used += resource->reserved_bytes;
        if (used > resource->budget_bytes ||
            actual_bytes > resource->budget_bytes - used)
            return 0;
    }
    return 1;
}

int cnet_model_manager_open(CnetModelManager **manager_out,
                            const CnetModelManagerOptions *options) {
    CnetModelManager *manager;
    uint32_t index;
    uint32_t initialized_conditions = 0;

    if (!manager_out) return CNET_MODEL_INVALID;
    *manager_out = NULL;
    if (!options || options->abi_version != CNET_MODEL_RUNTIME_ABI_VERSION ||
        options->struct_size < sizeof *options || options->max_models == 0 ||
        options->max_backends == 0 || !options->budgets ||
        options->budget_count == 0 ||
        options->budget_count > CNET_MODEL_MAX_RESOURCES)
        return CNET_MODEL_INVALID;

    manager = calloc(1, sizeof *manager);
    if (!manager) return CNET_MODEL_INVALID;
    manager->max_models = options->max_models;
    manager->max_backends = options->max_backends;
    manager->models = calloc(manager->max_models, sizeof *manager->models);
    manager->backends = calloc(manager->max_backends, sizeof *manager->backends);
    manager->resources = calloc(options->budget_count, sizeof *manager->resources);
    if (!manager->models || !manager->backends || !manager->resources ||
        pthread_mutex_init(&manager->mutex, NULL) != 0) {
        free(manager->models);
        free(manager->backends);
        free(manager->resources);
        free(manager);
        return CNET_MODEL_INVALID;
    }

    for (index = 0; index < manager->max_models; index++) {
        if (pthread_cond_init(&manager->models[index].changed, NULL) != 0)
            goto fail;
        manager->models[index].condition_initialized = 1;
        initialized_conditions++;
    }

    for (index = 0; index < options->budget_count; index++) {
        uint32_t previous;
        if (!is_single_bit(options->budgets[index].resource_mask) ||
            options->budgets[index].budget_bytes == 0)
            goto fail;
        for (previous = 0; previous < index; previous++) {
            if (options->budgets[previous].resource_mask ==
                options->budgets[index].resource_mask)
                goto fail;
        }
        manager->resources[index].mask = options->budgets[index].resource_mask;
        manager->resources[index].budget_bytes = options->budgets[index].budget_bytes;
    }
    manager->resource_count = options->budget_count;
    manager->next_generation = 1;
    *manager_out = manager;
    return CNET_MODEL_OK;

fail:
    for (index = 0; index < initialized_conditions; index++)
        pthread_cond_destroy(&manager->models[index].changed);
    pthread_mutex_destroy(&manager->mutex);
    free(manager->models);
    free(manager->backends);
    free(manager->resources);
    free(manager);
    return CNET_MODEL_INVALID;
}

int cnet_model_manager_close(CnetModelManager *manager) {
    uint32_t index;
    DeferredUnload *deferred;
    uint32_t deferred_count = 0;
    if (!manager) return CNET_MODEL_INVALID;
    deferred = calloc(manager->max_models, sizeof *deferred);
    if (!deferred) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    for (index = 0; index < manager->model_count; index++) {
        if (manager->models[index].lease_count != 0 ||
            manager->models[index].state == CNET_MODEL_STATE_LOADING) {
            pthread_mutex_unlock(&manager->mutex);
            free(deferred);
            return CNET_MODEL_BUSY;
        }
    }
    for (index = 0; index < manager->model_count; index++) {
        if (manager->models[index].state == CNET_MODEL_STATE_RESIDENT &&
            manager->models[index].handle) {
            detach_entry_locked(manager, &manager->models[index], 0,
                                &deferred[deferred_count]);
            if (deferred[deferred_count].handle)
                deferred_count++;
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    deferred_unload_execute(deferred, deferred_count);
    free(deferred);

    for (index = 0; index < manager->max_models; index++) {
        if (manager->models[index].condition_initialized)
            pthread_cond_destroy(&manager->models[index].changed);
    }
    pthread_mutex_destroy(&manager->mutex);
    free(manager->models);
    free(manager->backends);
    free(manager->resources);
    free(manager);
    return CNET_MODEL_OK;
}

int cnet_model_manager_model_count(const CnetModelManager *manager) {
    int count;
    CnetModelManager *mutable_manager = (CnetModelManager *)manager;
    if (!manager) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&mutable_manager->mutex);
    count = (int)manager->model_count;
    pthread_mutex_unlock(&mutable_manager->mutex);
    return count;
}

int cnet_model_backend_register(CnetModelManager *manager,
                                const CnetModelBackendSpec *backend) {
    if (!manager || !backend ||
        backend->abi_version != CNET_MODEL_RUNTIME_ABI_VERSION ||
        backend->struct_size < sizeof *backend ||
        !has_terminator(backend->name, sizeof backend->name) ||
        backend->name[0] == '\0' || !backend->load || !backend->unload)
        return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    if (find_backend_locked(manager, backend->name) >= 0 ||
        manager->backend_count >= manager->max_backends) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_INVALID;
    }
    manager->backends[manager->backend_count++].spec = *backend;
    pthread_mutex_unlock(&manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_model_catalog_add(CnetModelManager *manager,
                           const CnetModelDescriptor *descriptor) {
    CnetModelEntry *entry;
    if (!manager || !descriptor_is_valid(descriptor)) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    if (find_model_locked(manager, descriptor->model_id) >= 0 ||
        manager->model_count >= manager->max_models ||
        count_bits(descriptor->allowed_resource_mask &
                   managed_resource_mask_locked(manager)) <
            descriptor->required_resource_count) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_INVALID;
    }
    entry = &manager->models[manager->model_count++];
    entry->descriptor = *descriptor;
    entry->state = CNET_MODEL_STATE_COLD;
    pthread_mutex_unlock(&manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_model_acquire(CnetModelManager *manager,
                       const char *model_id,
                       uint64_t preferred_resource_mask,
                       CnetModelLease *lease_out) {
    int model_index;
    int backend_index;
    int result;
    uint64_t selected_mask = 0;
    uint64_t estimated_bytes;
    uint64_t actual_bytes = 0;
    uint8_t *evictions;
    uint32_t index;
    CnetModelEntry *entry;
    CnetModelBackendSpec backend;
    void *handle = NULL;
    int load_result;
    int materialization_failed;

    /* Relocation rollback state */
    int is_relocation = 0;
    void *old_handle = NULL;
    uint64_t old_resource_mask = 0;
    uint64_t old_resident_bytes = 0;

    /* Deferred-unload list for evicted models (and relocation old handle). */
    DeferredUnload *deferred;
    uint32_t deferred_count = 0;

    if (!manager || !model_id || !lease_out) return CNET_MODEL_INVALID;
    memset(lease_out, 0, sizeof *lease_out);
    evictions = calloc(manager->max_models, 1);
    if (!evictions) return CNET_MODEL_INVALID;
    deferred = calloc(manager->max_models + 1, sizeof *deferred);
    if (!deferred) {
        free(evictions);
        return CNET_MODEL_INVALID;
    }

    pthread_mutex_lock(&manager->mutex);
retry:
    model_index = find_model_locked(manager, model_id);
    if (model_index < 0) {
        result = CNET_MODEL_NOT_FOUND;
        goto done_locked;
    }
    entry = &manager->models[model_index];
    if ((preferred_resource_mask & ~entry->descriptor.allowed_resource_mask) != 0) {
        result = CNET_MODEL_INVALID;
        goto done_locked;
    }
    if (entry->state == CNET_MODEL_STATE_LOADING) {
        pthread_cond_wait(&entry->changed, &manager->mutex);
        goto retry;
    }
    if (entry->transaction_pin_count != 0) {
        result = CNET_MODEL_BUSY;
        goto done_locked;
    }
    if (entry->state == CNET_MODEL_STATE_FAILED) {
        result = CNET_MODEL_LOAD_FAILED;
        goto done_locked;
    }
    if (entry->state == CNET_MODEL_STATE_RESIDENT) {
        if (lease_is_compatible(entry, preferred_resource_mask)) {
            entry->lease_count++;
            entry->cache_hits++;
            entry->last_use_tick = ++manager->use_tick;
            fill_lease(entry, lease_out);
            result = CNET_MODEL_OK;
            goto done_locked;
        }
        if (entry->lease_count != 0 || entry->pin_count != 0) {
            result = CNET_MODEL_BUSY;
            goto done_locked;
        }
        /* Relocation is prepared without mutating the healthy residency.
         * The old handle and its accounting remain live until the replacement
         * load, actual-size validation, and bystander set all commit. */
        is_relocation = 1;
        old_handle = entry->handle;
        old_resource_mask = entry->resource_mask;
        old_resident_bytes = entry->resident_bytes;
    }

    backend_index = find_backend_locked(manager, entry->descriptor.backend_name);
    if (backend_index < 0) {
        result = CNET_MODEL_UNSUPPORTED;
        goto done_locked;
    }
    result = choose_resources_locked(manager, (uint32_t)model_index,
                                     preferred_resource_mask,
                                     &selected_mask, evictions);
    if (result != CNET_MODEL_OK) {
        goto done_locked;
    }

    /* Transaction-pin every selected bystander. Their handles, generations,
     * states and accounting remain untouched until commit. */
    for (index = 0; index < manager->model_count; index++) {
        if (evictions[index]) manager->models[index].transaction_pin_count++;
    }

    estimated_bytes = entry->descriptor.resident_bytes_per_resource;
    for (index = 0; index < manager->resource_count; index++) {
        if ((selected_mask & manager->resources[index].mask) != 0)
            manager->resources[index].reserved_bytes += estimated_bytes;
    }
    entry->state = CNET_MODEL_STATE_LOADING;
    if (!is_relocation) {
        entry->resource_mask = selected_mask;
        entry->backend_index = (uint32_t)backend_index;
    }
    backend = manager->backends[backend_index].spec;
    pthread_mutex_unlock(&manager->mutex);

    load_result = backend.load(backend.context, &entry->descriptor, selected_mask,
                               &handle, &actual_bytes);
    materialization_failed =
        load_result != 0 || !handle || actual_bytes > estimated_bytes;
    if (materialization_failed && handle) {
        deferred[deferred_count].handle = handle;
        deferred[deferred_count].unload_fn = backend.unload;
        deferred[deferred_count].backend_context = backend.context;
        deferred_count++;
        handle = NULL;
    }

    pthread_mutex_lock(&manager->mutex);
    entry = &manager->models[model_index];
    for (index = 0; index < manager->resource_count; index++) {
        CnetResourceEntry *resource = &manager->resources[index];
        if ((selected_mask & resource->mask) == 0) continue;
        if (resource->reserved_bytes >= estimated_bytes)
            resource->reserved_bytes -= estimated_bytes;
        else
            resource->reserved_bytes = 0;
    }

    if (materialization_failed) {
        release_transaction_pins_locked(manager, evictions);
        if (is_relocation) {
            /* The old residency was never mutated; only restore its visible
             * state after the replacement attempt leaves LOADING. */
            entry->state = CNET_MODEL_STATE_RESIDENT;
        } else {
            entry->state = CNET_MODEL_STATE_FAILED;
            entry->resource_mask = 0;
            entry->resident_bytes = 0;
            entry->handle = NULL;
        }
        entry->load_failures++;
        result = load_result == 0 && actual_bytes > estimated_bytes ?
                 CNET_MODEL_OVER_BUDGET : CNET_MODEL_LOAD_FAILED;
        pthread_cond_broadcast(&entry->changed);
        goto done_locked;
    }

    if (!projected_residency_fits_locked(manager, selected_mask, evictions,
                                         is_relocation, old_resource_mask,
                                         old_resident_bytes, actual_bytes)) {
        deferred[deferred_count].handle = handle;
        deferred[deferred_count].unload_fn = backend.unload;
        deferred[deferred_count].backend_context = backend.context;
        deferred_count++;
        handle = NULL;
        release_transaction_pins_locked(manager, evictions);
        if (is_relocation) {
            entry->state = CNET_MODEL_STATE_RESIDENT;
        } else {
            entry->state = CNET_MODEL_STATE_FAILED;
            entry->resource_mask = 0;
            entry->resident_bytes = 0;
            entry->handle = NULL;
        }
        entry->load_failures++;
        result = CNET_MODEL_OVER_BUDGET;
        pthread_cond_broadcast(&entry->changed);
        goto done_locked;
    }

    /* Commit every selected victim and the old target residency atomically.
     * Callback destruction is queued until after the new entry is visible. */
    for (index = 0; index < manager->model_count; index++) {
        if (!evictions[index]) continue;
        if (manager->models[index].transaction_pin_count > 0)
            manager->models[index].transaction_pin_count--;
        detach_entry_locked(manager, &manager->models[index], 1,
                            &deferred[deferred_count]);
        if (deferred[deferred_count].handle) deferred_count++;
    }
    if (is_relocation && old_handle) {
        entry->state = CNET_MODEL_STATE_RESIDENT;
        detach_entry_locked(manager, entry, 1, &deferred[deferred_count]);
        if (deferred[deferred_count].handle) deferred_count++;
    }

    entry->handle = handle;
    entry->resident_bytes = actual_bytes;
    entry->resource_mask = selected_mask;
    entry->backend_index = (uint32_t)backend_index;
    entry->generation = manager->next_generation++;
    entry->last_use_tick = ++manager->use_tick;
    entry->state = CNET_MODEL_STATE_RESIDENT;
    entry->lease_count = 1;
    entry->loads++;
    for (index = 0; index < manager->resource_count; index++) {
        CnetResourceEntry *resource = &manager->resources[index];
        if ((selected_mask & resource->mask) == 0) continue;
        resource->resident_bytes += actual_bytes;
        resource->resident_models++;
    }
    fill_lease(entry, lease_out);
    pthread_cond_broadcast(&entry->changed);
    result = CNET_MODEL_OK;
    is_relocation = 0;

done_locked:
    pthread_mutex_unlock(&manager->mutex);
    /* Execute any remaining deferred unloads outside the mutex. */
    deferred_unload_execute(deferred, deferred_count);
    free(deferred);
    free(evictions);
    return result;
}

int cnet_model_release(CnetModelManager *manager,
                       const CnetModelLease *lease) {
    int index;
    CnetModelEntry *entry;
    if (!manager || !lease || lease->model_id[0] == '\0')
        return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    index = find_model_locked(manager, lease->model_id);
    if (index < 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_NOT_FOUND;
    }
    entry = &manager->models[index];
    if (entry->state != CNET_MODEL_STATE_RESIDENT || entry->lease_count == 0 ||
        entry->handle != lease->handle || entry->generation != lease->generation ||
        entry->resource_mask != lease->resource_mask) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_INVALID;
    }
    entry->lease_count--;
    pthread_cond_broadcast(&entry->changed);
    pthread_mutex_unlock(&manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_model_pin(CnetModelManager *manager, const char *model_id) {
    int index;
    if (!manager || !model_id) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    index = find_model_locked(manager, model_id);
    if (index < 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_NOT_FOUND;
    }
    if (manager->models[index].state == CNET_MODEL_STATE_LOADING ||
        manager->models[index].transaction_pin_count != 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_BUSY;
    }
    manager->models[index].pin_count++;
    pthread_mutex_unlock(&manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_model_unpin(CnetModelManager *manager, const char *model_id) {
    int index;
    if (!manager || !model_id) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    index = find_model_locked(manager, model_id);
    if (index < 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_NOT_FOUND;
    }
    if (manager->models[index].pin_count == 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_INVALID;
    }
    manager->models[index].pin_count--;
    pthread_mutex_unlock(&manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_model_evict(CnetModelManager *manager, const char *model_id) {
    int index;
    CnetModelEntry *entry;
    DeferredUnload deferred = {0};
    if (!manager || !model_id) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    index = find_model_locked(manager, model_id);
    if (index < 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_NOT_FOUND;
    }
    entry = &manager->models[index];
    if (entry->state == CNET_MODEL_STATE_LOADING || entry->lease_count != 0 ||
        entry->pin_count != 0 || entry->transaction_pin_count != 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_BUSY;
    }
    detach_entry_locked(manager, entry, 1, &deferred);
    pthread_mutex_unlock(&manager->mutex);
    deferred_unload_execute(&deferred, 1);
    return CNET_MODEL_OK;
}

int cnet_model_reset_failure(CnetModelManager *manager,
                             const char *model_id) {
    int index;
    if (!manager || !model_id) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&manager->mutex);
    index = find_model_locked(manager, model_id);
    if (index < 0) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_NOT_FOUND;
    }
    if (manager->models[index].state != CNET_MODEL_STATE_FAILED) {
        pthread_mutex_unlock(&manager->mutex);
        return CNET_MODEL_INVALID;
    }
    manager->models[index].state = CNET_MODEL_STATE_COLD;
    pthread_cond_broadcast(&manager->models[index].changed);
    pthread_mutex_unlock(&manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_model_stats(const CnetModelManager *manager,
                     const char *model_id,
                     CnetModelStats *stats_out) {
    int index;
    CnetModelEntry *entry;
    CnetModelManager *mutable_manager = (CnetModelManager *)manager;
    if (!manager || !model_id || !stats_out) return CNET_MODEL_INVALID;
    pthread_mutex_lock(&mutable_manager->mutex);
    index = find_model_locked(manager, model_id);
    if (index < 0) {
        pthread_mutex_unlock(&mutable_manager->mutex);
        return CNET_MODEL_NOT_FOUND;
    }
    entry = &mutable_manager->models[index];
    memset(stats_out, 0, sizeof *stats_out);
    stats_out->state = entry->state;
    stats_out->model_class = entry->descriptor.model_class;
    stats_out->resident_bytes = entry->resident_bytes;
    stats_out->lease_count = entry->lease_count;
    stats_out->pin_count = entry->pin_count;
    stats_out->loads = entry->loads;
    stats_out->cache_hits = entry->cache_hits;
    stats_out->evictions = entry->evictions;
    stats_out->load_failures = entry->load_failures;
    pthread_mutex_unlock(&mutable_manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_model_resource_stats(const CnetModelManager *manager,
                              uint64_t resource_mask,
                              CnetModelResourceStats *stats_out) {
    int index;
    CnetModelManager *mutable_manager = (CnetModelManager *)manager;
    if (!manager || !stats_out || !is_single_bit(resource_mask))
        return CNET_MODEL_INVALID;
    pthread_mutex_lock(&mutable_manager->mutex);
    index = find_resource_locked(manager, resource_mask);
    if (index < 0) {
        pthread_mutex_unlock(&mutable_manager->mutex);
        return CNET_MODEL_NOT_FOUND;
    }
    stats_out->resident_bytes = manager->resources[index].resident_bytes;
    stats_out->reserved_bytes = manager->resources[index].reserved_bytes;
    stats_out->budget_bytes = manager->resources[index].budget_bytes;
    stats_out->resident_models = manager->resources[index].resident_models;
    pthread_mutex_unlock(&mutable_manager->mutex);
    return CNET_MODEL_OK;
}

int cnet_descriptor_validate(const CnetModelDescriptor *descriptor) {
    return descriptor_is_valid(descriptor) ? CNET_MODEL_OK : CNET_MODEL_INVALID;
}

void cnet_descriptor_print(const CnetModelDescriptor *descriptor) {
    if (!descriptor) {
        printf("(null descriptor)\n");
        return;
    }
    printf("model_id       : %s\n", descriptor->model_id);
    printf("backend        : %s\n", descriptor->backend_name);
    printf("architecture   : %s\n", descriptor->architecture);
    printf("format         : %s\n", descriptor->format);
    printf("quantization   : %s\n", descriptor->quantization);
    printf("artifact       : %s  (%llu bytes)\n", descriptor->artifact_path,
           (unsigned long long)descriptor->artifact_bytes);
    printf("class          : %s\n",
           cnet_model_class_name(descriptor->model_class));
    printf("caps           : 0x%08x\n", descriptor->capabilities);
    printf("context_limit  : %u\n", descriptor->context_limit);
    printf("resource_mask  : allowed=0x%llx preferred=0x%llx count=%u\n",
           (unsigned long long)descriptor->allowed_resource_mask,
           (unsigned long long)descriptor->preferred_resource_mask,
           descriptor->required_resource_count);
    printf("resident_bytes : %llu workspace=%llu kv/token=%llu\n",
           (unsigned long long)descriptor->resident_bytes_per_resource,
           (unsigned long long)descriptor->workspace_bytes,
           (unsigned long long)descriptor->kv_bytes_per_token);
}

const char *cnet_model_class_name(CnetModelClass model_class) {
    switch (model_class) {
        case CNET_MODEL_CLASS_DENSE_TRANSFORMER:
            return "dense_transformer";
        case CNET_MODEL_CLASS_MOE:
            return "moe";
        case CNET_MODEL_CLASS_SSM:
            return "ssm";
        case CNET_MODEL_CLASS_EMBEDDING:
            return "embedding";
        default:
            return "other";
    }
}
