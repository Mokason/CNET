#ifndef CNET_REGISTRY_LORA_STORE_H
#define CNET_REGISTRY_LORA_STORE_H
#include "registry_lora.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Save certified adapter for unit under dir (created). path = dir/unit.lora */
int registry_lora_store_save(PrimitiveRegistry *reg, const char *unit,
                             const char *dir);
/* Load adapter from dir/unit.lora; mark_certified=1 opens serve gate. */
int registry_lora_store_load(PrimitiveRegistry *reg, const char *unit,
                             const char *dir, int mark_certified);
/* Save every certified attached adapter. Returns count saved or -1. */
int registry_lora_store_save_all(PrimitiveRegistry *reg, const char *dir);
/* Load every *.lora in dir for matching unit names. Returns count loaded. */
int registry_lora_store_load_all(PrimitiveRegistry *reg, const char *dir,
                                 int mark_certified);
/* dir from CNET_LORA_STORE_DIR, or NULL if unset. */
const char *registry_lora_store_dir_env(void);
#ifdef __cplusplus
}
#endif
#endif
