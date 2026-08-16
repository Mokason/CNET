#ifndef CNET_EVOLVE_DIR_H
#define CNET_EVOLVE_DIR_H

/* Load evolve direction/curriculum for unattended CORE growth. */

#include "cnet_core_paths.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_EVDIR_MAX_FACTORY 16
#define CNET_EVDIR_MAX_GOALS 32
#define CNET_EVDIR_MAX_DOM 32
#define CNET_EVDIR_NAME 64

typedef struct {
    int allow_live_miss;
    int allow_factory;
    int allow_goals;
    int allow_agi_tick;
    int factory_if_empty;
    int max_new_per_tick;
    CnetPath2Spec factory[CNET_EVDIR_MAX_FACTORY];
    int n_factory;
    char goals[CNET_EVDIR_MAX_GOALS][256];
    int n_goals;
    char prefer[CNET_EVDIR_MAX_DOM][CNET_EVDIR_NAME];
    int n_prefer;
    char deny[CNET_EVDIR_MAX_DOM][CNET_EVDIR_NAME];
    int n_deny;
    char loaded_from[512];
} CnetEvolveDirection;

void cnet_evolve_dir_defaults(CnetEvolveDirection *D);

/* Search order: CNET_EVOLVE_DIRECTION, bricks_dir/evolve_direction.conf,
 * config/cnet_evolve_direction.conf. Missing file → defaults. */
int cnet_evolve_dir_load(CnetEvolveDirection *D, const char *bricks_dir);

/* 1 if domain allowed by prefer/deny lists. */
int cnet_evolve_dir_domain_ok(const CnetEvolveDirection *D, const char *domain);

#ifdef __cplusplus
}
#endif

#endif
