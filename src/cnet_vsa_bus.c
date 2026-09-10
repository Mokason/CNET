#include "../include/cnet_vsa_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cnet_vsa_bus_init(CnetVsaBus *bus, int dim) {
    if (!bus || dim <= 0) return -1;
    bus->dim = dim;
    bus->count = 0;
    memset(bus->specialists, 0, sizeof(bus->specialists));
    return 0;
}

void cnet_vsa_bus_free(CnetVsaBus *bus) {
    if (!bus) return;
    for (size_t i = 0; i < bus->count; ++i) {
        free(bus->specialists[i].contract.centroid);
    }
    memset(bus, 0, sizeof(*bus));
}

int cnet_vsa_bus_register(CnetVsaBus *bus, const char *name,
                           const float *centroid, float radius_epsilon,
                           float margin_floor, CnetVsaSpecialistFn fn,
                           void *user_ctx) {
    if (!bus || !name || !centroid || !fn || bus->count >= CNET_VSA_MAX_SPECIALISTS)
        return -1;

    CnetVsaSpecialist *s = &bus->specialists[bus->count];
    strncpy(s->name, name, CNET_VSA_NAME_MAX - 1);
    s->name[CNET_VSA_NAME_MAX - 1] = '\0';
    strncpy(s->contract.name, name, CNET_VSA_NAME_MAX - 1);
    s->contract.dim = bus->dim;
    s->contract.radius_epsilon = radius_epsilon;
    s->contract.margin_floor = margin_floor;
    s->contract.centroid = (float *)malloc((size_t)bus->dim * sizeof(float));
    if (!s->contract.centroid) return -1;
    memcpy(s->contract.centroid, centroid, (size_t)bus->dim * sizeof(float));

    s->forward = fn;
    s->user_ctx = user_ctx;
    s->invocations = 0;
    bus->count++;
    return 0;
}

CnetVsaStatus cnet_vsa_bus_route_step(CnetVsaBus *bus, float *io_vector,
                                      char *out_specialist_name,
                                      size_t name_cap, float *out_margin) {
    if (!bus || !io_vector || bus->count == 0) return CNET_VSA_STATUS_EXEC_ERR;

    int best_idx = -1;
    float best_margin = -1e9f;

    for (size_t i = 0; i < bus->count; ++i) {
        int admitted = 0;
        float m = 0.0f;
        cnet_vsa_contract_verify(&bus->specialists[i].contract, io_vector, &admitted, &m);
        if (admitted && m > best_margin) {
            best_margin = m;
            best_idx = (int)i;
        }
    }

    if (best_idx < 0) {
        /* Fail-Closed Abstention */
        return CNET_VSA_STATUS_ABSTAIN;
    }

    CnetVsaSpecialist *s = &bus->specialists[best_idx];
    if (out_specialist_name && name_cap > 0) {
        strncpy(out_specialist_name, s->name, name_cap - 1);
        out_specialist_name[name_cap - 1] = '\0';
    }
    if (out_margin) {
        *out_margin = best_margin;
    }

    float delta[CNET_VSA_DEFAULT_DIM];
    float *p_delta = delta;
    if (bus->dim > CNET_VSA_DEFAULT_DIM) {
        p_delta = (float *)calloc((size_t)bus->dim, sizeof(float));
    } else {
        memset(delta, 0, (size_t)bus->dim * sizeof(float));
    }

    int rc = s->forward(io_vector, p_delta, bus->dim, s->user_ctx);
    if (rc != 0) {
        if (p_delta != delta) free(p_delta);
        return CNET_VSA_STATUS_EXEC_ERR;
    }

    /* Additive Residual Update */
    for (int i = 0; i < bus->dim; ++i) {
        io_vector[i] += p_delta[i];
    }
    cnet_vsa_normalize(io_vector, bus->dim);
    s->invocations++;

    if (p_delta != delta) free(p_delta);
    return CNET_VSA_STATUS_SUCCESS;
}

CnetVsaStatus cnet_vsa_bus_execute_chain(CnetVsaBus *bus,
                                         const char *const *specialist_names,
                                         size_t hop_count, float *io_vector,
                                         size_t *hops_executed) {
    if (!bus || !specialist_names || !io_vector || !hops_executed)
        return CNET_VSA_STATUS_EXEC_ERR;

    *hops_executed = 0;

    for (size_t h = 0; h < hop_count; ++h) {
        const char *target_name = specialist_names[h];
        int found_idx = -1;
        for (size_t i = 0; i < bus->count; ++i) {
            if (strncmp(bus->specialists[i].name, target_name, CNET_VSA_NAME_MAX) == 0) {
                found_idx = (int)i;
                break;
            }
        }
        if (found_idx < 0) return CNET_VSA_STATUS_EXEC_ERR;

        CnetVsaSpecialist *s = &bus->specialists[found_idx];

        /* Guard every hop: verify contract on incoming vector */
        int admitted = 0;
        float margin = 0.0f;
        cnet_vsa_contract_verify(&s->contract, io_vector, &admitted, &margin);
        if (!admitted) {
            /* Abstain at this intermediate hop */
            return CNET_VSA_STATUS_ABSTAIN;
        }

        float delta[CNET_VSA_DEFAULT_DIM];
        float *p_delta = delta;
        if (bus->dim > CNET_VSA_DEFAULT_DIM) {
            p_delta = (float *)calloc((size_t)bus->dim, sizeof(float));
        } else {
            memset(delta, 0, (size_t)bus->dim * sizeof(float));
        }

        int rc = s->forward(io_vector, p_delta, bus->dim, s->user_ctx);
        if (rc != 0) {
            if (p_delta != delta) free(p_delta);
            return CNET_VSA_STATUS_EXEC_ERR;
        }

        /* Additive Residual Update */
        for (int i = 0; i < bus->dim; ++i) {
            io_vector[i] += p_delta[i];
        }
        cnet_vsa_normalize(io_vector, bus->dim);
        s->invocations++;
        (*hops_executed)++;

        if (p_delta != delta) free(p_delta);
    }

    return CNET_VSA_STATUS_SUCCESS;
}
