#ifndef CNET_HEALTH_LAYERS_H
#define CNET_HEALTH_LAYERS_H

/* Layered health diagnostics for specialists — separate from the maintenance
 * optimizer (specialist_health_pass), which *fixes*. This module *measures*
 * five independent layers so a miss is attributed honestly:
 *
 *   0 REGISTRY   — name present in the live registry
 *   1 LOADABLE   — BTN resident, ports/dims coherent, invocable shape
 *   2 EXECUTION  — forward runs without crash/NULL on a probe input
 *   3 SEMANTIC   — contract replay / certification acceptance
 *   4 UTILITY    — production usefulness (trust, reliability, evidence)
 *
 * Layers are ordered: a FAIL/SKIP at layer k makes layers > k SKIP (not
 * FAIL) so the report shows the deepest consecutive PASS. Each layer still
 * has its own verdict atom for telemetry.
 *
 * Gate: make health_layers → HEALTH_LAYERS_PASS
 */

#include <stddef.h>

#include "cnet_export.h"
#include "router.h"
#include "contract/contract.h"
#include "specialist_health.h"  /* SpecialistContractLookup */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNET_HEALTH_LAYER_REGISTRY  = 0,
    CNET_HEALTH_LAYER_LOADABLE  = 1,
    CNET_HEALTH_LAYER_EXECUTION = 2,
    CNET_HEALTH_LAYER_SEMANTIC  = 3,
    CNET_HEALTH_LAYER_UTILITY   = 4,
    CNET_HEALTH_LAYER_COUNT     = 5
} CnetHealthLayer;

typedef enum {
    CNET_HEALTH_PASS = 0,
    CNET_HEALTH_FAIL = 1,
    CNET_HEALTH_SKIP = 2   /* not evaluated (dependency failed / no contract) */
} CnetHealthVerdict;

typedef struct {
    /* Utility thresholds (defaults match lifecycle promotion). */
    double utility_reliability_floor;  /* default 0.9 */
    size_t utility_min_evidence;       /* default 16  */
    /* Semantic: require btn_certify when contract available (default 1). */
    int require_certify;
    SpecialistContractLookup contracts; /* NULL => semantic may SKIP */
    void *contracts_ctx;
} CnetHealthLayerConfig;

typedef struct {
    char unit_name[64];
    CnetHealthVerdict layer[CNET_HEALTH_LAYER_COUNT];
    char reason[CNET_HEALTH_LAYER_COUNT][48]; /* stable atom per layer */
    int deepest_pass;   /* max k with layers[0..k] all PASS, or -1 */
    int production_ready; /* 1 iff all five PASS */
    double reliability; /* Laplace, or -1 if unknown */
    size_t evidence;    /* successes+failures */
} CnetUnitHealthLayers;

typedef struct {
    size_t units;
    size_t pass[CNET_HEALTH_LAYER_COUNT];
    size_t fail[CNET_HEALTH_LAYER_COUNT];
    size_t skip[CNET_HEALTH_LAYER_COUNT];
    size_t production_ready;
    size_t deepest_hist[CNET_HEALTH_LAYER_COUNT + 1]; /* index = deepest+1 */
} CnetRegistryHealthLayers;

CNET_API void cnet_health_layer_config_defaults(CnetHealthLayerConfig *cfg);

CNET_API const char *cnet_health_layer_name(CnetHealthLayer layer);
CNET_API const char *cnet_health_verdict_name(CnetHealthVerdict v);

/* Check one unit. name may be NULL to use entry index via reg scan — use
 * name. Returns 0 with *out filled, or -1 on bad args / unknown name. */
CNET_API int cnet_health_check_unit(const PrimitiveRegistry *reg,
                                    const char *name,
                                    const CnetHealthLayerConfig *cfg,
                                    CnetUnitHealthLayers *out);

/* Check every registry entry; optional per-unit callback (may be NULL).
 * Fills *agg when non-NULL. Returns 0, or -1. */
typedef void (*CnetHealthUnitFn)(const CnetUnitHealthLayers *u, void *ctx);
CNET_API int cnet_health_check_registry(const PrimitiveRegistry *reg,
                                        const CnetHealthLayerConfig *cfg,
                                        CnetRegistryHealthLayers *agg,
                                        CnetHealthUnitFn on_unit,
                                        void *on_unit_ctx);

/* Compact JSON for one unit (no trailing newline). */
CNET_API int cnet_health_layers_format_json(const CnetUnitHealthLayers *u,
                                            char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HEALTH_LAYERS_H */
