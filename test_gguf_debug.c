#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

int main(void) {
    const char* path = "G:\\AI\\Marble's\\Marble-s-BitnetMamba\\models\\qwen2.5-0.5b-f32.gguf";
    FILE* f = fopen(path, "rb");
    if (!f) { printf("Cannot open: %s\n", path); return 1; }
    char magic[4];
    if (fread(magic, 1, 4, f) != 4) { printf("read magic fail\n"); fclose(f); return 1; }
    uint32_t version;
    fread(&version, 4, 1, f);
    uint64_t n_tensors, n_kv;
    fread(&n_tensors, 8, 1, f);
    fread(&n_kv, 8, 1, f);
    printf("Magic: %.4s\n", magic);
    printf("Version: %u\n", version);
    printf("n_tensors: %" PRIu64 "\n", n_tensors);
    printf("n_kv: %" PRIu64 "\n", n_kv);
    fclose(f);
    printf("Header read successfully.\n");
    return 0;
}