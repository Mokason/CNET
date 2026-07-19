/* Hermetic + optional real GGUF tokenizer tests.
 *   bin/test_gguf_tok [path.gguf]
 * Without path: only chat_template unit checks.
 * With path: encode/decode round-trip + specials.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_gguf_tok.h"

static int fails = 0, checks = 0;
#define CHECK(c, msg)                                                          \
    do {                                                                       \
        checks++;                                                              \
        if (c)                                                                 \
            printf("  ok  %s\n", msg);                                         \
        else {                                                                 \
            fails++;                                                           \
            printf("  FAIL %s\n", msg);                                        \
        }                                                                      \
    } while (0)

int main(int argc, char **argv) {
    char tmpl[2048];
    int n;

    printf("== test_gguf_tok ==\n");
    n = cce_gguf_tok_chat_template(tmpl, (int)sizeof tmpl, NULL,
                                   "What is 2+2?", 1);
    CHECK(n > 0, "chat template length");
    CHECK(strstr(tmpl, "<|im_start|>system") != NULL, "has system");
    CHECK(strstr(tmpl, "Qwythos") != NULL, "has identity");
    CHECK(strstr(tmpl, "<|im_start|>user") != NULL, "has user");
    CHECK(strstr(tmpl, "What is 2+2?") != NULL, "has prompt");
    CHECK(strstr(tmpl, "<|im_start|>assistant\n<think>\n") != NULL,
          "opens think");

    n = cce_gguf_tok_chat_template(tmpl, (int)sizeof tmpl, NULL, "Hi", 0);
    CHECK(n > 0 && strstr(tmpl, "</think>") != NULL, "no-think empty block");

    if (argc > 1) {
        cce_gguf_tok *tok = NULL;
        int ids[512], nd, ne;
        char out[4096];
        const char *path = argv[1];
        printf("  loading %s\n", path);
        CHECK(cce_gguf_tok_load(path, &tok) == CCE_OK && tok, "load gguf tok");
        if (tok) {
            CHECK(cce_gguf_tok_vocab_size(tok) > 1000, "vocab size");
            CHECK(cce_gguf_tok_eos_id(tok) > 0, "eos id");
            {
                const char *samples[] = {
                    "Hello", " Hello", "What is 2+2?",
                    "<|im_start|>user\nHello<|im_end|>\n", NULL};
                int s;
                for (s = 0; samples[s]; ++s) {
                    char label[128];
                    ne = cce_gguf_tok_encode(tok, samples[s], ids, 512);
                    snprintf(label, sizeof label, "encode '%s' n=%d",
                             samples[s], ne);
                    CHECK(ne > 0, label);
                    nd = cce_gguf_tok_decode(tok, ids, ne, out, (int)sizeof out,
                                             0);
                    CHECK(nd > 0, "decode keep specials");
                    /* skip-special decode should be printable for plain text */
                    nd = cce_gguf_tok_decode(tok, ids, ne, out, (int)sizeof out,
                                             1);
                    if (samples[s][0] != '<') {
                        CHECK(strstr(out, "Hello") != NULL ||
                                  strstr(out, "What") != NULL ||
                                  strstr(out, "2") != NULL,
                              "round-trip content");
                    }
                    printf("    enc/dec %s -> %s\n", samples[s], out);
                }
            }
            {
                ne = cce_gguf_tok_encode_chat(tok, NULL, "What is 2+2?", 1, ids,
                                              512);
                CHECK(ne > 10, "encode_chat");
                printf("    chat_ids n=%d first=", ne);
                {
                    int i;
                    for (i = 0; i < ne && i < 12; ++i) printf("%d ", ids[i]);
                }
                printf("\n");
                /* expect im_start as first token when specials work */
                if (ne > 0) {
                    const char *p0 = cce_gguf_tok_piece(tok, ids[0]);
                    CHECK(p0 && strcmp(p0, "<|im_start|>") == 0,
                          "chat starts with im_start");
                }
            }
            cce_gguf_tok_free(tok);
        }
    } else {
        printf("  (pass GGUF path for encode/decode tests)\n");
    }

    printf("GGUF_TOK_PASS checks=%d fails=%d\n", checks, fails);
    return fails ? 1 : 0;
}
