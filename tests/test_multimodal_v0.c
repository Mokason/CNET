/* Multimodal v0 gate: external teacher bridge + voice + vision mine/admit.
 * make multimodal_v0 → MULTIMODAL_V0_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/external_teacher.h"
#include "../include/modality_voice.h"
#include "../include/modality_vision.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/specialist.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int argmax_n(const double *v, int n) {
    int i, b = 0;
    for (i = 1; i < n; i++) if (v[i] > v[b]) b = i;
    return b;
}

int main(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork *voice = NULL, *vision = NULL;
    ExternalTeacher vt;
    OracleRegistry oracles;
    double feat[CNET_VOICE_FEAT_DIM], cmd[CNET_VOICE_N_CMD];
    double img[CNET_VISION_PIXELS], cls[CNET_VISION_N_CLASS];
    const double *out;
    int i;

    printf("== multimodal v0 (external teachers) ==\n");
    registry_init(&reg);
    memset(&oracles, 0, sizeof oracles);

    /* --- bridge: refuse zero identity --- */
    {
        ExternalTeacher t;
        CnetOracleIdentity id;
        external_teacher_init(&t);
        memset(&id, 0, sizeof id);
        id.abi_version = CNET_ORACLE_ABI_VERSION;
        id.struct_size = (uint32_t)sizeof id;
        check(external_teacher_bind_callback(
                  &t, CNET_MODALITY_VOICE, "bad", cnet_voice_input_port(),
                  cnet_voice_output_port(), cnet_voice_hermetic_teacher, NULL,
                  &id, 0) == -2,
              "zero artifact_digest refused");
    }

    /* --- voice teacher alone --- */
    {
        CnetOracleIdentity id;
        memset(&id, 0, sizeof id);
        id.abi_version = CNET_ORACLE_ABI_VERSION;
        id.struct_size = (uint32_t)sizeof id;
        id.artifact_digest = 0x544541434831ULL; /* TEACH1 */
        external_teacher_init(&vt);
        check(external_teacher_bind_callback(
                  &vt, CNET_MODALITY_VOICE, "voice_t",
                  cnet_voice_input_port(), cnet_voice_output_port(),
                  cnet_voice_hermetic_teacher, NULL, &id, 0) == 0,
              "voice teacher binds");
        check(strcmp(cnet_modality_name(vt.modality), "voice") == 0,
              "modality name voice");
        for (i = 0; i < CNET_VOICE_N_CMD; i++) {
            cnet_voice_hermetic_features(i, feat);
            check(cnet_voice_hermetic_teacher(feat, cmd, NULL) == 0 &&
                      argmax_n(cmd, CNET_VOICE_N_CMD) == i,
                  "hermetic voice teacher exact on class");
        }
        check(external_teacher_register(&oracles, &vt) == 0,
              "voice teacher registers as oracle");
    }

    /* --- voice mine + admit --- */
    check(cnet_voice_v0_mine_admit(&reg, 0x5630494345ULL, &voice, NULL) == 0 &&
              voice != NULL,
          "voice v0 mine+admit");
    check(reg.count >= 1 && reg.entries[0].certified,
          "voice unit certified in registry");
    for (i = 0; i < CNET_VOICE_N_CMD; i++) {
        cnet_voice_hermetic_features(i, feat);
        out = btn_forward(voice, feat);
        check(out && argmax_n(out, CNET_VOICE_N_CMD) == i,
              "voice student matches teacher class");
    }

    /* --- vision mine + admit --- */
    check(cnet_vision_v0_mine_admit(&reg, 0x563153494F4EULL, &vision, NULL) == 0 &&
              vision != NULL,
          "vision v0 mine+admit");
    check(reg.count >= 2, "registry holds voice + vision units");
    for (i = 0; i < CNET_VISION_N_CLASS; i++) {
        cnet_vision_hermetic_render(i, img);
        check(cnet_vision_hermetic_teacher(img, cls, NULL) == 0 &&
                  argmax_n(cls, CNET_VISION_N_CLASS) == i,
              "hermetic vision teacher exact");
        out = btn_forward(vision, img);
        check(out && argmax_n(out, CNET_VISION_N_CLASS) == i,
              "vision student matches teacher class");
    }

    /* --- table teacher path --- */
    {
        ExternalTeacher tt;
        double tin[4], tout[8];
        CnetOracleIdentity id;
        Port pin, pout;
        int r;
        memset(&pin, 0, sizeof pin);
        pin.family = PORT_ONEHOT;
        pin.field_width = 2;
        pin.field_count = 1;
        memset(&pout, 0, sizeof pout);
        pout.family = PORT_ONEHOT;
        pout.field_width = 2;
        pout.field_count = 1;
        /* rows: [1,0]->[1,0], [0,1]->[0,1] */
        tin[0] = 1; tin[1] = 0; tin[2] = 0; tin[3] = 1;
        tout[0] = 1; tout[1] = 0; tout[2] = 0; tout[3] = 1;
        memset(&id, 0, sizeof id);
        id.abi_version = CNET_ORACLE_ABI_VERSION;
        id.struct_size = (uint32_t)sizeof id;
        id.artifact_digest = 0x5441424c303031ULL; /* TABL001 */
        external_teacher_init(&tt);
        check(external_teacher_bind_table(&tt, CNET_MODALITY_TEXT, "table_t",
                                          pin, pout, tin, tout, 2, &id,
                                          0) == 0,
              "table teacher binds");
        {
            double o[2];
            double in0[2] = {1, 0};
            r = tt.fn(in0, o, tt.ctx);
            check(r == 0 && o[0] > 0.5, "table teacher row0");
        }
        external_teacher_unbind(&tt);
    }

    /* evidence port constructor exists for late-binding vision */
    check(cnet_vision_evidence_port().family == PORT_EVIDENCE,
          "vision evidence port family");

    external_teacher_unbind(&vt);
    if (voice) {
        btn_free(voice);
        free(voice);
    }
    if (vision) {
        btn_free(vision);
        free(vision);
    }
    registry_free(&reg);

    printf("MULTIMODAL_V0_PASS checks=%d\n", checks);
    return failures ? 1 : 0;
}
