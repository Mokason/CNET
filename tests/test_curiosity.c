/* Curiosity gate: budgeted idle self-seeding of teachable gaps.
 * make curiosity → CURIOSITY_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_curiosity.h"
#include "../include/router.h"
#include "../include/nn.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    int n = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) n++;
    fclose(f);
    return n;
}

int main(void) {
    CnetCuriosityConfig cfg;
    CnetCuriosityReport rep;
    PrimitiveRegistry reg;
    const char *inbox = "tmp_curiosity.inbox";
    const char *win = "tmp_curiosity_window.txt";
    const char *state = "tmp_curiosity_state.txt";
    FILE *f;
    int i;

    printf("== curiosity ==\n");
    remove(inbox);
    remove(win);
    remove(state);

    f = fopen(win, "w");
    check(f != NULL, "write window");
    if (f) {
        for (i = 100; i < 116; i++) fprintf(f, "%d\n", i);
        fclose(f);
    }

    cnet_curiosity_config_defaults(&cfg);
    check(cfg.enabled == 0, "default disabled");
    cfg.enabled = 1;
    cfg.max_per_hour = 10;
    cfg.max_per_tick = 3;
    cfg.k = 3;
    cfg.yield_if_open = 4;
    snprintf(cfg.window_path, sizeof cfg.window_path, "%s", win);
    snprintf(cfg.state_path, sizeof cfg.state_path, "%s", state);
    snprintf(cfg.inbox_path, sizeof cfg.inbox_path, "%s", inbox);

    registry_init_production(&reg);

    /* Busy yield */
    check(cnet_curiosity_tick(&cfg, &reg, 10, &rep) == 0, "tick busy ok");
    check(rep.skipped_busy == 1 && rep.proposed == 0, "yields when open gaps high");

    /* Propose novel tokens */
    check(cnet_curiosity_tick(&cfg, &reg, 0, &rep) == 0, "tick idle ok");
    check(rep.proposed == 3, "proposes max_per_tick");
    check(count_lines(inbox) == 3, "inbox has 3 NO_PLAN lines");
    {
        FILE *inb = fopen(inbox, "r");
        char line[256];
        int ok_shape = 0;
        if (inb && fgets(line, sizeof line, inb)) {
            /* NO_PLAN 1 16 1 w_cur 1 16 3 tk… */
            ok_shape = strstr(line, "NO_PLAN") && strstr(line, "w_cur") &&
                       strstr(line, "tk");
        }
        if (inb) fclose(inb);
        check(ok_shape, "lane-teachable shape in inbox");
    }

    /* Coverage: pretend token 100 is sealed */
    {
        BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
        btn_init(btn, 4, 4, 2, 4, 0.1, 1);
        registry_add(&reg, btn, "acq_tk100q100");
        check(cnet_curiosity_token_covered(&reg, 100) == 1, "covered token detected");
        check(cnet_curiosity_token_covered(&reg, 101) == 0, "uncovered token novel");
    }

    /* Budget: more ticks respect hourly cap */
    {
        size_t total = 3;
        int t;
        for (t = 0; t < 5; t++) {
            cnet_curiosity_tick(&cfg, &reg, 0, &rep);
            total += rep.proposed;
        }
        check(total <= 10, "hourly budget not exceeded");
    }

    /* Disabled */
    cfg.enabled = 0;
    remove(inbox);
    cnet_curiosity_tick(&cfg, &reg, 0, &rep);
    check(rep.enabled == 0 && count_lines(inbox) == 0, "disabled is no-op");

    registry_free(&reg);
    remove(inbox);
    remove(win);
    remove(state);

    if (failures) {
        printf("CURIOSITY_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("CURIOSITY_PASS checks=%d\n", checks);
    return 0;
}
