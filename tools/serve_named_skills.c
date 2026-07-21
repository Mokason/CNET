/* Native exercise of sealed skill_/research_ units.
 * make serve_named_skills && ./bin/serve_named_skills [base.cnb]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/soul_host.h"
#include "../include/nn.h"

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int exercise(SoulHost *h, const char *goal, const char *query) {
    double in[256];
    double out[256 * 8];
    int idx, rc, milli, src, idim = 0, odim = 0;
    char unit[96];
    int served = 0;

    memset(in, 0, sizeof in);
    memset(out, 0, sizeof out);
    idx = (int)(fnv(query && query[0] ? query : goal) % 256u);
    in[idx] = 1.0;
    snprintf(unit, sizeof unit, "acq_%s", goal);

    rc = soul_request(h,
                      PORT_ONEHOT, 256, 1, "w_cur",
                      PORT_ONEHOT, 256, 3, goal,
                      in, 256, out, (int)(sizeof out / sizeof out[0]));
    src = soul_last_source(h);
    milli = soul_unit_reliability_milli(h, unit);
    if (rc > 0) {
        served = 1;
    } else if (soul_unit_dims(h, unit, &idim, &odim) == 0 && idim > 0 &&
               idim <= 256) {
        rc = soul_run(h, unit, in, out, (int)(sizeof out / sizeof out[0]));
        if (rc > 0) {
            served = 1;
            src = SOUL_SOURCE_CERTIFIED;
        }
    }
    printf("| `%s` | served=%d src=%d rc=%d reli_m=%d dims=%d/%d |\n",
           goal, served, src, rc, milli, idim, odim);
    return served ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "soul_gemma4v2_final.cnb";
    SoulHost *h = NULL;
    int fails = 0;
    static const char *goals[] = {
        "research_unity_core_loop",
        "research_unity_game_feel_juice",
        "research_rpg_place_as_testimony",
        "research_rpg_oath_bearer_player",
        "research_gamedev_hit_pipeline",
        "skill_rpg_quest_prompt_craft",
        "skill_rpg_place_as_testimony",
        "skill_unity_vertical_slice_chec",
        NULL
    };
    int i;
    SoulServeStats st;

    if (soul_open(base, NULL, &h) != 0 || !h) {
        fprintf(stderr, "soul_open failed\n");
        return 2;
    }
    printf("## Results\n\n| skill | result |\n|---|---|\n");
    for (i = 0; goals[i]; i++) {
        if (exercise(h, goals[i], goals[i]) != 0) fails++;
    }
    memset(&st, 0, sizeof st);
    if (soul_serve_stats(h, &st) == 0)
        printf("\nServe stats: certified=%llu residual=%llu bound=%d units=%d\n",
               (unsigned long long)st.certified_serves,
               (unsigned long long)st.residual_serves,
               st.residual_bound, st.units);
    soul_close(h);
    printf("\nSERVE_NAMED_SKILLS_%s fails=%d\n", fails ? "PARTIAL" : "PASS", fails);
    return fails ? 1 : 0;
}
