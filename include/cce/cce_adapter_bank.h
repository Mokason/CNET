/* Hard-routed multi-adapter bank: one active slot serves; others idle. */
#ifndef CCE_ADAPTER_BANK_H
#define CCE_ADAPTER_BANK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_ADAPTER_BANK_MAX 16
#define CCE_ADAPTER_NAME_MAX 64

typedef struct cce_lora cce_lora; /* opaque optional */

typedef struct {
    char name[CCE_ADAPTER_NAME_MAX];
    void *handle;       /* borrowed cce_lora* or cce_lily* or cce_dora* */
    int kind;           /* 0=none 1=lora 2=lily 3=dora */
    int certified;
    int in_use;
} cce_adapter_slot;

typedef struct {
    cce_adapter_slot slots[CCE_ADAPTER_BANK_MAX];
    size_t count;
    int active; /* index or -1 */
} cce_adapter_bank;

void cce_adapter_bank_init(cce_adapter_bank *b);
/* Register or replace slot by name. handle borrowed. Returns slot index or -1. */
int cce_adapter_bank_put(cce_adapter_bank *b, const char *name, void *handle,
                         int kind, int certified);
/* Hard-select active by name. Returns 0 or -1 if missing/uncertified. */
int cce_adapter_bank_select(cce_adapter_bank *b, const char *name);
/* Active handle or NULL. */
void *cce_adapter_bank_active(const cce_adapter_bank *b, int *out_kind);
void cce_adapter_bank_clear_active(cce_adapter_bank *b);
int cce_adapter_bank_find(const cce_adapter_bank *b, const char *name);

#ifdef __cplusplus
}
#endif
#endif
