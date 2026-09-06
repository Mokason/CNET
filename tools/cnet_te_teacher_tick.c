/* Harvest English irregulars from residual Teacher (27B). Never CERT.
 *   ./bin/cnet_te_teacher_tick --lemmas FILE --propose TSV
 * Live: CNET_HELD_MODEL_ENDPOINT=http://127.0.0.1:8081/v1/chat/completions
 */
#include "../include/cnet_typed_en.h"
#include "../include/cnet_held_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int live_ask(const char *q, char *out, size_t cap) {
    return cnet_held_model_ask(q, out, cap);
}

int main(int argc, char **argv) {
    const char *lemmas = "config/en_teacher_lemmas.txt";
    const char *propose = NULL;
    const char *ep;
    const char *pk;
    char propbuf[1024];
    int i, n;

    pk = getenv("CNET_PACKS_ROOT");
    if (pk && pk[0]) {
        snprintf(propbuf, sizeof propbuf, "%s/en_irregular_propose.tsv", pk);
        propose = propbuf;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lemmas") && i + 1 < argc)
            lemmas = argv[++i];
        else if (!strcmp(argv[i], "--propose") && i + 1 < argc)
            propose = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "cnet_te_teacher_tick [--lemmas file] [--propose tsv]\n"
                    "27B residual harvest. auto_cert=0. Does not overlay.\n");
            return 0;
        }
    }
    if (!propose)
        propose = "/tmp/en_irregular_propose.tsv";
    ep = getenv("CNET_HELD_MODEL_ENDPOINT");
    if (!ep || !ep[0])
        ep = "http://127.0.0.1:8081/v1/chat/completions";
    (void)cnet_held_model_set_endpoint(ep);
    (void)cnet_held_model_set_name("held");
    n = cnet_te_teacher_tick(lemmas, propose, live_ask);
    cnet_held_model_close();
    if (n < 0) {
        printf("te_teacher_tick new=-1\n");
        return 1;
    }
    printf("te_teacher_tick new=%d propose=%s auto_cert=0\n", n, propose);
    printf("CNET_TE_TEACHER_TICK_OK\n");
    return 0;
}
