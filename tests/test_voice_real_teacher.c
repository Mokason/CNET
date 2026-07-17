/* Real voice teacher ABI: subprocess external teacher → mine/admit closed-set.
 * Uses tools/voice_teacher.py --mode hermetic for the hermetic gate (no Whisper
 * download). Whisper path is optional via env CNET_VOICE_TEACHER_CMD.
 *
 * make voice_real_teacher → VOICE_REAL_TEACHER_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/external_teacher.h"
#include "../include/modality_voice.h"
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

static int file_exists(const char *p) {
    return p && p[0] && access(p, R_OK) == 0;
}

int main(void) {
    PrimitiveRegistry reg;
    ExternalTeacher t;
    BinaryTransformNetwork *student = NULL;
    char cmdline[1024];
    const char *py = getenv("CNET_VOICE_TEACHER_PYTHON");
    const char *script = "tools/voice_teacher.py";
    double feat[CNET_VOICE_FEAT_DIM], cmd[CNET_VOICE_N_CMD];
    const double *out;
    int i;

    printf("== voice real teacher (subprocess ABI) ==\n");

    if (!file_exists(script)) {
        /* allow running from build dir */
        script = "../tools/voice_teacher.py";
    }
    check(file_exists(script), "voice_teacher.py present");

    if (!py || !py[0]) py = "python3";
    snprintf(cmdline, sizeof cmdline, "%s %s --mode hermetic", py, script);

    registry_init(&reg);
    external_teacher_init(&t);

    /* Refuse zero digest via bind_subprocess */
    {
        CnetOracleIdentity id;
        memset(&id, 0, sizeof id);
        id.abi_version = CNET_ORACLE_ABI_VERSION;
        id.struct_size = (uint32_t)sizeof id;
        check(external_teacher_bind_subprocess(
                  &t, CNET_MODALITY_VOICE, "bad", cnet_voice_input_port(),
                  cnet_voice_output_port(), cmdline, &id, 0) == -2,
              "subprocess zero artifact_digest refused");
    }

    check(cnet_voice_bind_subprocess(&t, "voice_sub_hermetic", cmdline, 0) == 0,
          "voice subprocess teacher binds");
    check(strcmp(t.kind, "subprocess") == 0, "kind=subprocess");
    check(t.identity.artifact_digest != 0, "artifact digest nonzero");

    /* Round-trip each class through the child. */
    for (i = 0; i < CNET_VOICE_N_CMD; i++) {
        char label[64];
        cnet_voice_hermetic_features(i, feat);
        memset(cmd, 0, sizeof cmd);
        check(t.fn(feat, cmd, t.ctx) == 0 &&
                  argmax_n(cmd, CNET_VOICE_N_CMD) == i,
              "subprocess teacher exact on class");
        snprintf(label, sizeof label, "  class %d via subprocess", i);
        (void)label;
    }

    check(cnet_voice_mine_admit_teacher(&t, &reg, "voice_cmd_sub_v0",
                                        &student) == 0 &&
              student != NULL,
          "mine+admit via subprocess teacher");
    check(reg.count >= 1 && reg.entries[0].certified,
          "student certified in registry");
    for (i = 0; i < CNET_VOICE_N_CMD; i++) {
        cnet_voice_hermetic_features(i, feat);
        out = btn_forward(student, feat);
        check(out && argmax_n(out, CNET_VOICE_N_CMD) == i,
              "student matches subprocess teacher class");
    }

    external_teacher_unbind(&t);

    /* Env bind path */
    {
        ExternalTeacher t2;
        char env_cmd[1024];
        snprintf(env_cmd, sizeof env_cmd, "%s", cmdline);
        setenv("CNET_VOICE_TEACHER_CMD", env_cmd, 1);
        setenv("CNET_VOICE_TEACHER_NAME", "voice_env", 1);
        external_teacher_init(&t2);
        check(cnet_voice_bind_from_env(&t2) == 0, "bind from env");
        cnet_voice_hermetic_features(3, feat);
        check(t2.fn(feat, cmd, t2.ctx) == 0 &&
                  argmax_n(cmd, CNET_VOICE_N_CMD) == 3,
              "env teacher classifies down");
        external_teacher_unbind(&t2);
        unsetenv("CNET_VOICE_TEACHER_CMD");
        unsetenv("CNET_VOICE_TEACHER_NAME");
    }

    /* Optional whisper readiness (does not fail gate if unavailable). */
    {
        char chk[1024];
        int rc;
        snprintf(chk, sizeof chk, "%s %s --mode whisper --check >/dev/null 2>&1",
                 py, script);
        rc = system(chk);
        if (rc == 0)
            printf("  (whisper backend available — real ASR ready)\n");
        else
            printf("  (whisper backend not loaded — hermetic ABI still green)\n");
        check(1, "whisper optional readiness probed");
    }

    /* registry borrows student; leave for process exit (same as multimodal_v0). */
    (void)student;
    registry_free(&reg);

    if (failures) {
        printf("VOICE_REAL_TEACHER_FAIL failures=%d checks=%d\n", failures,
               checks);
        return 1;
    }
    printf("VOICE_REAL_TEACHER_PASS checks=%d\n", checks);
    return 0;
}
