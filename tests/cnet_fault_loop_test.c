/* cnet_fault_loop_test — close the bus loop:
 *   write labeled vectors to JSONL
 *   fresh registry (empty queues)
 *   registry_lora_ingest_fault_bus
 *   registry_lora_tick teaches+certifies from ingested pairs
 */
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"
#include "../include/cnet_fault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t S = 0xB0A71100;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static void sample_feat(double *feat, int nfeat) {
    int i, k, j;
    for (i = 0; i < nfeat; i++) feat[i] = 0.0;
    k = 1 + (int)(rnd() % 4);
    for (j = 0; j < k; j++) feat[rnd() % nfeat] = 1.0;
}

int main(void) {
    char path[] = "/tmp/cnet_fault_loop_XXXXXX";
    int fd = mkstemp(path);
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    const char *unit;
    int IN, OUT, i, faults = 0;
    CnetFaultLog log;
    registry_lora_tick_opts topt;
    registry_lora_tick_report trep;
    int ingested;
    char envbuf[640];

    if (fd < 0) {
        printf("FAIL mkstemp\n");
        return 1;
    }
    close(fd);
    unlink(path);

    registry_init(&reg);
    if (cnet_jtc_v0_mine_admit(&reg, 0x4A54435F99ULL, &student, NULL) != 0 ||
        !student) {
        printf("FAIL mine_admit\n");
        return 1;
    }
    IN = (int)student->input_count;
    OUT = (int)student->output_count;
    unit = CNET_JTC_UNIT_NAME;

    /* Phase A: park real faults via registry path WITH mirror → bus */
    snprintf(envbuf, sizeof envbuf, "CNET_FAULT_LOG=%s", path);
    putenv(envbuf);
    unsetenv("CNET_FAULT_MIRROR"); /* default on when fault.o linked */

    {
        double feat[64], toh[32], bad[32];
        RoutePlan plan;
        memset(&plan, 0, sizeof plan);
        plan.steps[0] = student;
        plan.names[0] = unit;
        plan.length = 1;
        for (i = 0; i < 400; i++) {
            int teach_tool, served;
            sample_feat(feat, IN);
            cnet_jtc_hermetic_teacher(feat, toh, NULL);
            teach_tool = cnet_jtc_decode_tool(toh);
            {
                double out[32];
                if (route_execute_ex(&plan, feat, (size_t)IN, out, (size_t)OUT,
                                     NULL) != 0)
                    continue;
                served = cnet_jtc_decode_tool(out);
            }
            if (served != teach_tool) {
                const double *so = btn_forward(student, feat);
                int o;
                for (o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0.0;
                if (registry_record_fault(&reg, unit, feat, bad) == 0 &&
                    registry_supply_label(&reg, unit, feat, toh) == 0)
                    faults++;
            }
        }
    }
    printf("phase A: parked %d labeled faults; bus lines=%zu\n", faults,
           cnet_fault_count_file(path));
    if (faults < 32) {
        printf("FAIL too few faults %d\n", faults);
        return 1;
    }
    if (cnet_fault_count_file(path) < (size_t)faults / 2) {
        printf("FAIL bus under-filled (mirror not linked?)\n");
        return 1;
    }

    /* Phase B: fresh registry — only bus has the pairs */
    registry_free(&reg);
    registry_init(&reg);
    student = NULL;
    if (cnet_jtc_v0_mine_admit(&reg, 0x4A54435F99ULL, &student, NULL) != 0 ||
        !student) {
        printf("FAIL remine\n");
        return 1;
    }
    /* same unit name, empty queues */

    ingested = registry_lora_ingest_fault_bus(&reg, path, unit);
    printf("phase B: ingested %d pairs from bus\n", ingested);
    if (ingested < 32) {
        printf("FAIL ingest %d\n", ingested);
        return 1;
    }

    /* Phase C: tick teaches from ingested queue (lower min_faults for test) */
    topt = registry_lora_tick_defaults();
    topt.min_faults = 32;
    topt.holdout_frac = 0.25;
    topt.teach.rank = 4;
    topt.teach.alpha = 8.f;
    topt.teach.train.epochs = 400;
    topt.teach.train.lr = 0.03f;
    topt.cert.argmax_mode = 1;
    topt.cert.max_regressions = -1;
    topt.cert.min_net_gain = 1;
    memset(&trep, 0, sizeof trep);
    /* Don't re-ingest duplicates: temporarily clear env during tick? 
     * Tick auto-ingests — double count OK for teach; use path still set. */
    if (registry_lora_tick(&reg, &topt, &trep) != 0) {
        printf("FAIL tick\n");
        return 1;
    }
    printf("phase C: tick seen=%zu taught=%zu certified=%zu rejected=%zu\n",
           trep.units_seen, trep.taught, trep.certified, trep.rejected);
    if (trep.taught < 1) {
        printf("FAIL no teach\n");
        return 1;
    }
    if (trep.certified < 1 && trep.rejected < 1) {
        printf("FAIL no cert decision\n");
        return 1;
    }

    /* Direct append_labeled round-trip */
    if (cnet_fault_open(&log, path) != 0) {
        printf("FAIL reopen log\n");
        return 1;
    }
    {
        CnetFaultRecord rec;
        double in[64], tg[32];
        size_t n;
        memset(&rec, 0, sizeof rec);
        rec.source = CNET_FAULT_SRC_GHOST;
        snprintf(rec.unit, sizeof rec.unit, "%s", unit);
        rec.in_dim = IN;
        rec.out_dim = OUT;
        sample_feat(in, IN);
        cnet_jtc_hermetic_teacher(in, tg, NULL);
        if (cnet_fault_append_labeled(&log, &rec, in, tg) != 0) {
            printf("FAIL append_labeled\n");
            return 1;
        }
        cnet_fault_close(&log);
        n = cnet_fault_load_vectors(path, unit, IN, OUT, in, tg, 1);
        /* load into temp buffers */
        {
            double *I = malloc(8 * (size_t)IN * sizeof(double));
            double *T = malloc(8 * (size_t)OUT * sizeof(double));
            n = cnet_fault_load_vectors(path, unit, IN, OUT, I, T, 8);
            free(I);
            free(T);
            if (n < 1) {
                printf("FAIL load_vectors\n");
                return 1;
            }
        }
    }

    unlink(path);
    printf("CNET_FAULT_LOOP_PASS faults=%d ingested=%d taught=%zu certified=%zu\n",
           faults, ingested, trep.taught, trep.certified);
    return 0;
}
