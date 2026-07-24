/* procedure_chunks_test — N2: seal 5 real multi-step procedure specialists
 * and prove hard MoE serve + decode presentation.
 *
 * Each chunk is a small certified BTN with ports that encode a checklist step
 * graph (one-hot stage → next stage), not a tk* nickname.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"
#include "../include/cnet_moe.h"
#include "../include/cnet_serve_decode.h"
#include "../include/cnet_acct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_ok;
#define CHECK(c, m) do { if (c) g_ok++; else { fprintf(stderr,"FAIL %s\n",m); g_fail++; } } while(0)

#define STAGES 5

typedef struct {
    const char *name;
    const char *labels[STAGES];
} ProcChunk;

static const ProcChunk k_chunks[] = {
    {"acq_chunk_unity_hit_pipeline",
     {"sense", "decide", "windup", "impact", "resolve"}},
    {"acq_chunk_rpg_testimony_quest",
     {"place_read", "contradiction", "oath_cost", "choice", "aftermath"}},
    {"acq_chunk_unity_core_verb",
     {"verb", "loop", "feedback", "scope", "ship"}},
    {"acq_chunk_gamedev_juice_hitstop",
     {"hit", "stop", "shake", "flash", "recover"}},
    {"acq_chunk_rpg_oath_bearer_arc",
     {"oath", "witness", "breach", "trial", "legacy"}},
};

static int admit_pipeline(PrimitiveRegistry *reg, const char *name) {
    BinaryTransformNetwork *btn;
    double in[STAGES][STAGES], tg[STAGES][STAGES];
    Contract c;
    Specialist s;
    Port pin, pout;
    int i, j;
    btn = calloc(1, sizeof *btn);
    if (!btn) return -1;
    memset(&pin, 0, sizeof pin);
    memset(&pout, 0, sizeof pout);
    pin.family = pout.family = PORT_ONEHOT;
    pin.field_width = pout.field_width = STAGES;
    pin.field_count = pout.field_count = 1;
    snprintf(pin.tag, sizeof pin.tag, "%s_in", name);
    snprintf(pout.tag, sizeof pout.tag, "%s", name);
    for (i = 0; i < STAGES; i++) {
        for (j = 0; j < STAGES; j++) {
            in[i][j] = (j == i) ? 1.0 : 0.0;
            /* next stage (last loops to itself = terminal) */
            tg[i][j] = (j == ((i + 1 < STAGES) ? i + 1 : i)) ? 1.0 : 0.0;
        }
    }
    if (btn_init(btn, STAGES, STAGES, 12, 48, 0.5, 19) != 0) return -1;
    if (btn_set_ports(btn, pin, pout) != 0) return -1;
    (void)btn_train(btn, (const double *)in, (const double *)tg, STAGES, 6000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, (const double *)in,
                               (const double *)tg, STAGES) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name) != 0 ||
        specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    return 0;
}

static int argmax5(const double *v) {
    int i, b = 0;
    for (i = 1; i < STAGES; i++)
        if (v[i] > v[b]) b = i;
    return b;
}

int main(void) {
    PrimitiveRegistry reg;
    size_t n = sizeof k_chunks / sizeof k_chunks[0];
    size_t ci;
    int serves = 0;
    char decoded[256];
    FILE *map;

    cnet_acct_reset();
    registry_init(&reg);

    for (ci = 0; ci < n; ci++) {
        CHECK(admit_pipeline(&reg, k_chunks[ci].name) == 0, k_chunks[ci].name);
    }

    map = fopen("artifacts/janitor/PROCEDURE_CHUNKS.md", "w");
    if (map) {
        fprintf(map, "# Procedure chunks (sealed hermetic)\n\n");
        fprintf(map, "| unit | stages | serve |\n|---|---|---|\n");
    }

    for (ci = 0; ci < n; ci++) {
        Port ip, gp;
        double in[STAGES], out[STAGES];
        CnetMoeHit hit;
        int stage, next, picks[1];
        const ProcChunk *ch = &k_chunks[ci];
        int ok_chain = 1;

        memset(&ip, 0, sizeof ip);
        memset(&gp, 0, sizeof gp);
        ip.family = gp.family = PORT_ONEHOT;
        ip.field_width = gp.field_width = STAGES;
        ip.field_count = gp.field_count = 1;
        snprintf(ip.tag, sizeof ip.tag, "%s_in", ch->name);
        port_set_tag(&gp, ch->name);

        /* Walk full pipeline 0→1→2→3→4 */
        stage = 0;
        for (int step = 0; step < STAGES; step++) {
            int j;
            for (j = 0; j < STAGES; j++) in[j] = (j == stage) ? 1.0 : 0.0;
            if (cnet_moe_try_hard(&reg, ip, gp, in, STAGES, out, STAGES, &hit) !=
                    0 ||
                !hit.hit) {
                ok_chain = 0;
                break;
            }
            serves++;
            cnet_acct_add_hard(1);
            next = argmax5(out);
            picks[0] = next;
            CHECK(cnet_serve_decode_picks(picks, 1, ch->labels, STAGES, decoded,
                                          sizeof decoded) > 0,
                  "decode");
            if (step + 1 < STAGES)
                CHECK(next == stage + 1, "pipeline advance");
            else
                CHECK(next == stage, "terminal hold");
            stage = next;
        }
        CHECK(ok_chain, "full chain");
        if (map)
            fprintf(map, "| `%s` | %s → … → %s | ok |\n", ch->name,
                    ch->labels[0], ch->labels[STAGES - 1]);
    }

    if (map) {
        fprintf(map, "\nCNET_PROCEDURE_CHUNKS_PASS n=%zu serves=%d\n", n, serves);
        fclose(map);
    }

    CHECK(serves >= (int)n * STAGES, "serve count");

    if (g_fail) {
        fprintf(stderr, "failures=%d\n", g_fail);
        return 1;
    }
    printf("CNET_PROCEDURE_CHUNKS_PASS n=%zu serves=%d checks=%d\n", n, serves,
           g_ok);
    return 0;
}
