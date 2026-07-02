#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/cce/cce_safetensors.h"

int main() {
    printf("=== Testing Supra Console Chat (MOCK) ===\n");

    cce_supra_tokenizer* tok = NULL;
    /* tokenizer.json lives in the download-once cache (supra_cache/); fall back to cwd. */
    if (cce_supra_tokenizer_load(&tok, "supra_cache/tokenizer.json") != CCE_OK &&
        cce_supra_tokenizer_load(&tok, "tokenizer.json") != CCE_OK) {
        printf("Failed to load tokenizer (run `make supra_console` once to populate supra_cache/)\n");
        return 1;
    }

    char* test_inputs[] = {"hello there", "how are you today", NULL};
    for (int ti=0; test_inputs[ti]; ti++) {
        char* line = test_inputs[ti];
        int ids[256];
        int n = cce_supra_encode_text(tok, line, ids, 256);
        char resp[512];
        snprintf(resp, sizeof(resp), "Simulated Supra response to: %s", line);
        printf("User: %s\nResponse: [MOCK] Encoded %d tokens. %s [decomposed CNet chat demo]\n", line, n, resp);
    }

    cce_supra_tokenizer_free(tok);
    printf("=== Test complete ===\n");
    return 0;
}