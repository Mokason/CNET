#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "G:\\AI\\CNET\\Models\\gemma-4-12B-it-MTP-Q8_0.gguf";
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("Cannot open: %s\n", path);
        return 1;
    }
    char magic[4];
    fread(magic, 1, 4, f);
    uint32_t version;
    fread(&version, 4, 1, f);
    uint64_t n_tensors, n_kv;
    fread(&n_tensors, 8, 1, f);
    fread(&n_kv, 8, 1, f);
    printf("Magic: %.4s\nVersion: %u\nn_tensors: %llu\nn_kv: %llu\n", magic, version, n_tensors, n_kv);

    // Read KV to find architecture and sizes
    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t klen;
        fread(&klen, 8, 1, f);
        char* key = (char*)malloc(klen + 1);
        fread(key, 1, klen, f);
        key[klen] = 0;
        uint32_t vtype;
        fread(&vtype, 4, 1, f);

        if (strcmp(key, "general.architecture") == 0 && vtype == 8) { // string
            uint64_t slen;
            fread(&slen, 8, 1, f);
            char* val = (char*)malloc(slen + 1);
            fread(val, 1, slen, f);
            val[slen] = 0;
            printf("architecture: %s\n", val);
            free(val);
        } else if (strstr(key, "block_count") || strstr(key, "embedding_length") || strstr(key, "head_count") || strstr(key, "vocab_size")) {
            if (vtype == 4 || vtype == 10) { // u32 or u64
                uint64_t val = 0;
                if (vtype == 4) {
                    uint32_t v32; fread(&v32, 4, 1, f); val = v32;
                } else {
                    fread(&val, 8, 1, f);
                }
                printf("%s: %llu\n", key, val);
            } else {
                // skip value for now
                // for simplicity, just note
            }
        } else {
            // skip value
            // rough skip for common types
            uint64_t skip = 0;
            if (vtype == 0 || vtype == 1 || vtype == 7) skip = 1; // u8/i8/bool
            else if (vtype == 2 || vtype == 3) skip = 2;
            else if (vtype == 4 || vtype == 5 || vtype == 6) skip = 4;
            else if (vtype == 8) { // string
                uint64_t sl; fread(&sl, 8, 1, f); skip = sl;
                fseek(f, sl, SEEK_CUR);
                continue;
            } else if (vtype == 9) { // array - rough skip, may not be perfect
                uint32_t et; uint64_t n;
                fread(&et, 4, 1, f); fread(&n, 8, 1, f);
                // skip elements, for strings it's complex, for now fseek conservatively or break
                // for this debug, if we hit array we can stop or skip basic
                if (et == 8) { // string array
                    for (uint64_t k=0; k<n; k++) {
                        uint64_t sl; fread(&sl, 8, 1, f); fseek(f, sl, SEEK_CUR);
                    }
                } else {
                    // assume 8 byte or less
                    fseek(f, n * 8, SEEK_CUR);
                }
                continue;
            } else if (vtype == 10 || vtype == 11 || vtype == 12) skip = 8;
            if (skip) fseek(f, skip, SEEK_CUR);
        }
        free(key);
    }
    fclose(f);
    return 0;
}