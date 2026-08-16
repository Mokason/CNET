#include "cnet_brain_mirror.h"
#include "cnet_hemisphere.h"
#include "cnet_held_model.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks;
static int g_fail;

static void expect(int cond, const char *name) {
    g_checks++;
    if (cond)
        printf("  ok   %s\n", name);
    else {
        g_fail++;
        printf("  FAIL %s\n", name);
    }
}

static int count_cands(const char *dir) {
    char inbox[1024];
    DIR *d;
    struct dirent *e;
    int n = 0;
    snprintf(inbox, sizeof inbox, "%s/inbox", dir);
    d = opendir(inbox);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (strstr(e->d_name, "cand_") && strstr(e->d_name, ".json")) n++;
    }
    closedir(d);
    return n;
}

static int hook_res(const char *turn, char *out, size_t cap) {
    (void)turn;
    snprintf(out, cap, "residual-draft");
    return 0;
}

int main(void) {
    char tmp[] = "/tmp/cnet_brain_mirror_test_XXXXXX";
    char path[1100];
    char line[2048];
    CnetHemiPolicy pol;
    CnetHemiResult r;
    FILE *f;
    int before, after;

    g_checks = 0;
    g_fail = 0;
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 2;
    }
    cnet_brain_mirror_set_dir(tmp);
    unsetenv("CNET_HELD_MODEL_ENDPOINT");
    unsetenv("CNET_HELD_MODEL_PATH");
    cnet_held_model_set_hook(NULL);
    cnet_held_model_set_endpoint(NULL);
    cnet_held_model_set_path(NULL);

    printf("cnet_brain_mirror tests dir=%s\n", tmp);

    /* residual never mirrors */
    memset(&r, 0, sizeof r);
    r.hemi = CNET_HEMI_RESIDUAL;
    r.bound = 1;
    r.claimed_cert = 1; /* hostile */
    r.source = CNET_HEMI_SRC_HELD_LLM;
    snprintf(r.skill, sizeof r.skill, "held_model_v1");
    snprintf(r.spoken, sizeof r.spoken, "nope");
    expect(cnet_brain_mirror_core(&r) == 1, "skip_residual");
    before = count_cands(tmp);
    expect(before == 0, "no_cand_after_residual");

    /* CORE cert from hemi_ask_core writes jsonl + cand */
    cnet_hemi_policy_default(&pol);
    pol.residual_enabled = 0;
    pol.allow_wiki = 0;
    expect(cnet_hemi_ask_core("what is 4 plus 5", &pol, &r) == 0, "core_math");
    expect(r.hemi == CNET_HEMI_CORE && r.claimed_cert == 1, "core_cert");
    snprintf(path, sizeof path, "%s/core_admit.jsonl", tmp);
    f = fopen(path, "r");
    expect(f != NULL, "jsonl_exists");
    if (f) {
        expect(fgets(line, sizeof line, f) != NULL, "jsonl_line");
        expect(strstr(line, "\"hemi\":\"CORE\"") != NULL, "jsonl_hemi");
        expect(strstr(line, "add_u32") != NULL || strstr(line, "ood_math") != NULL ||
                   strstr(line, "\"value\":\"9\"") != NULL,
               "jsonl_payload");
        expect(strstr(line, "mirror_not_self_cert") != NULL, "jsonl_law");
        fclose(f);
    }
    after = count_cands(tmp);
    expect(after >= 1, "cand_written");

    /* residual bind does not grow mirror even via classify+mirror */
    cnet_held_model_set_hook(hook_res);
    pol.residual_enabled = 1;
    expect(cnet_hemi_ask("leftover residual phrase zz99", &pol, &r) == 0,
           "residual_ask");
    expect(r.hemi == CNET_HEMI_RESIDUAL, "is_residual");
    expect(cnet_brain_mirror_core(&r) == 1, "mirror_skips_residual_again");
    expect(count_cands(tmp) == after, "cand_count_stable");

    cnet_held_model_set_hook(NULL);
    cnet_brain_mirror_set_dir(NULL);

    printf("CNET_BRAIN_MIRROR_PASS\n");
    printf("checks=%d fail=%d residual_never_mirrors=1 core_writes_jsonl=1 "
           "python=0 broader_claims=WITHHELD\n",
           g_checks, g_fail);
    return g_fail ? 1 : 0;
}
