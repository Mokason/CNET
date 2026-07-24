#include "../../include/cce/cce_adapter_bank.h"
#include <string.h>

void cce_adapter_bank_init(cce_adapter_bank *b) {
    if (!b) return;
    memset(b, 0, sizeof *b);
    b->active = -1;
}

int cce_adapter_bank_find(const cce_adapter_bank *b, const char *name) {
    size_t i;
    if (!b || !name) return -1;
    for (i = 0; i < b->count; i++) {
        if (strcmp(b->slots[i].name, name) == 0) return (int)i;
    }
    return -1;
}

int cce_adapter_bank_put(cce_adapter_bank *b, const char *name, void *handle,
                         int kind, int certified) {
    int idx;
    if (!b || !name || !name[0] || !handle) return -1;
    idx = cce_adapter_bank_find(b, name);
    if (idx < 0) {
        if (b->count >= CCE_ADAPTER_BANK_MAX) return -1;
        idx = (int)b->count++;
        memset(&b->slots[idx], 0, sizeof b->slots[idx]);
        strncpy(b->slots[idx].name, name, CCE_ADAPTER_NAME_MAX - 1);
    }
    b->slots[idx].handle = handle;
    b->slots[idx].kind = kind;
    b->slots[idx].certified = certified ? 1 : 0;
    b->slots[idx].in_use = 1;
    return idx;
}

int cce_adapter_bank_select(cce_adapter_bank *b, const char *name) {
    int idx;
    if (!b) return -1;
    idx = cce_adapter_bank_find(b, name);
    if (idx < 0) return -1;
    if (!b->slots[idx].certified) return -1;
    b->active = idx;
    return 0;
}

void *cce_adapter_bank_active(const cce_adapter_bank *b, int *out_kind) {
    if (!b || b->active < 0 || (size_t)b->active >= b->count) {
        if (out_kind) *out_kind = 0;
        return NULL;
    }
    if (out_kind) *out_kind = b->slots[b->active].kind;
    return b->slots[b->active].handle;
}

void cce_adapter_bank_clear_active(cce_adapter_bank *b) {
    if (b) b->active = -1;
}
