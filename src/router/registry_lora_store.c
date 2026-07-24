#include "../../include/router/registry_lora_store.h"
#include "../../include/cce/cce_lora.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static RegistryEntry *find_entry(PrimitiveRegistry *reg, const char *name) {
    size_t i;
    if (!reg || !name) return NULL;
    for (i = 0; i < reg->count; i++)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    return NULL;
}

static int ensure_dir(const char *dir) {
    struct stat st;
    if (!dir || !dir[0]) return -1;
    if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(dir, 0755);
}

int registry_lora_store_save(PrimitiveRegistry *reg, const char *unit,
                             const char *dir) {
    RegistryEntry *e;
    char path[768];
    if (!reg || !unit || !dir) return -1;
    e = find_entry(reg, unit);
    if (!e || !e->lora || !e->lora_certified) return -1;
    if (ensure_dir(dir) != 0) return -1;
    snprintf(path, sizeof path, "%s/%s.lora", dir, unit);
    return cce_lora_save(e->lora, path) == CCE_OK ? 0 : -1;
}

int registry_lora_store_load(PrimitiveRegistry *reg, const char *unit,
                             const char *dir, int mark_certified) {
    RegistryEntry *e;
    char path[768];
    cce_lora *adp;
    if (!reg || !unit || !dir) return -1;
    e = find_entry(reg, unit);
    if (!e) return -1;
    snprintf(path, sizeof path, "%s/%s.lora", dir, unit);
    adp = malloc(sizeof(*adp));
    if (!adp) return -1;
    memset(adp, 0, sizeof(*adp));
    if (cce_lora_load(adp, path) != CCE_OK) {
        free(adp);
        return -1;
    }
    if (e->lora) {
        cce_lora_free(e->lora);
        free(e->lora);
    }
    e->lora = adp;
    e->lora_certified = mark_certified ? 1 : 0;
    return 0;
}
