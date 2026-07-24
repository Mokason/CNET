#ifndef CNET_REGISTRY_LORA_STORE_H
#define CNET_REGISTRY_LORA_STORE_H
#include "registry_lora.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Save certified adapter for unit under dir (created). path = dir/unit.lora */
int registry_lora_store_save(PrimitiveRegistry *reg, const char *unit,
                             const char *dir);
/* Load adapter from dir/unit.lora; leaves uncertified unless certify_after=0 and mark=1 */
int registry_lora_store_load(PrimitiveRegistry *reg, const char *unit,
                             const char *dir, int mark_certified);
#ifdef __cplusplus
}
#endif
#endif
