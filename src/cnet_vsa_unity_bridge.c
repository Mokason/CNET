#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cnet_vsa.h"
#include "cnet_vsa_gen_capsule.h"

static CnetVsaGenRegistry g_unity_registry;
static int g_registry_initialized = 0;

int cnet_unity_init_registry(const char *capsules_dir) {
    if (!capsules_dir) return -1;
    if (g_registry_initialized) {
        memset(&g_unity_registry, 0, sizeof(g_unity_registry));
        g_registry_initialized = 0;
    }
    cnet_vsa_registry_init(&g_unity_registry, CNET_VSA_DEFAULT_DIM);
    int count = cnet_vsa_registry_load_dir(&g_unity_registry, capsules_dir);
    if (count > 0) {
        g_registry_initialized = 1;
    }
    return count;
}

int cnet_unity_get_capsule_count(void) {
    if (!g_registry_initialized) return 0;
    return (int)g_unity_registry.count;
}

int cnet_unity_get_capsule_name(int index, char *out_name, int max_len) {
    if (!g_registry_initialized || !out_name || max_len < 1) return -1;
    if (index < 0 || (size_t)index >= g_unity_registry.count) return -2;
    snprintf(out_name, (size_t)max_len, "%s", g_unity_registry.capsules[index].header.name);
    return 0;
}

int cnet_unity_route(const char *prompt, char *out_capsule, int max_cap_len, float *out_dist) {
    if (!g_registry_initialized || !prompt) return -1;
    float q_vec[CNET_VSA_DEFAULT_DIM];
    if (cnet_vsa_gencap_encode_intent(prompt, q_vec, g_unity_registry.dim) != 0) return -2;
    int best_idx = -1;
    float best_dist = 1.0f;
    int winner = cnet_vsa_registry_route(&g_unity_registry, q_vec, &best_idx, &best_dist);
    if (out_dist) *out_dist = best_dist;
    if (out_capsule && max_cap_len > 0) {
        if (winner >= 0) {
            snprintf(out_capsule, (size_t)max_cap_len, "%s", g_unity_registry.capsules[winner].header.name);
        } else if (best_idx >= 0) {
            snprintf(out_capsule, (size_t)max_cap_len, "%s", g_unity_registry.capsules[best_idx].header.name);
        } else {
            out_capsule[0] = '\0';
        }
    }
    return winner;
}

int cnet_unity_dispatch(const char *prompt, char *out_text, int max_text_len, char *out_capsule, float *out_dist) {
    if (!g_registry_initialized || !prompt || !out_text || max_text_len < 16) return -1;
    return cnet_vsa_registry_dispatch(&g_unity_registry, prompt, out_text, (size_t)max_text_len, out_capsule, out_dist);
}

void cnet_unity_free_registry(void) {
    if (g_registry_initialized) {
        memset(&g_unity_registry, 0, sizeof(g_unity_registry));
        g_registry_initialized = 0;
    }
}
