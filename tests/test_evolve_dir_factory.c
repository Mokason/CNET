/* A direction file must be able to say "no factory curriculum".
 *
 * RED marker: EVOLVE_DIR_FACTORY_RED
 *
 * load_file() ends with:
 *
 *     if (D->n_factory == 0) {
 *         // restore default factory entries if file omitted them
 *         D->factory[0] = ... q1_add16 ...
 *         D->factory[1] = ... q1_xor16 ...
 *         D->n_factory = 2;
 *     }
 *
 * so an empty factory list is indistinguishable from an absent one and always
 * collapses to the two hardcoded defaults. There was therefore NO way to write
 * a config meaning "build nothing" -- commenting the factory lines out looks
 * like disabling them and silently re-enables exactly the tags being removed.
 * Observed live: with both `factory=` lines commented out, the evolve tick
 * still logged `factory curriculum built=2` and recreated q1_add16/q1_xor16 in
 * the serve dir. The only working switch was allow_factory=0, which is a
 * bigger hammer -- it disables the factory wholesale rather than emptying the
 * curriculum.
 *
 * Contract this gate pins:
 *   - key ABSENT        -> defaults (backward compatible; existing configs
 *                          that never mentioned `factory` keep their two)
 *   - `factory=`        -> explicitly NONE
 *   - `factory=none`    -> explicitly NONE
 *   - `factory=a,0,t`   -> exactly what was listed
 *   - commented-out     -> same as absent, and that is now a documented
 *                          choice rather than a surprise
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* rmdir */

#include "../include/cnet_evolve_dir.h"

static int checks = 0, fails = 0;

static void check(int cond, const char *msg, const char *detail) {
    checks++;
    if (cond) printf("  ok   %-44s %s\n", msg, detail ? detail : "");
    else { fails++; printf("  FAIL %-44s %s\n", msg, detail ? detail : ""); }
}

/* Write a direction file into a scratch dir and load it. */
static int load_with(const char *dir, const char *body, CnetEvolveDirection *D) {
    char path[768];
    FILE *f;
    snprintf(path, sizeof path, "%s/evolve_direction.conf", dir);
    f = fopen(path, "w");
    if (!f) return -1;
    fputs(body, f);
    fclose(f);
    return cnet_evolve_dir_load(D, dir);
}

int main(void) {
    CnetEvolveDirection D;
    char dir[] = "/tmp/evdir_factory_XXXXXX";
    char det[160];

    if (!mkdtemp(dir)) { printf("EVOLVE_DIR_FACTORY_RED mkdtemp failed\n"); return 1; }
    /* cnet_evolve_dir_load prefers $CNET_EVOLVE_DIRECTION; clear it so the
       test controls which file is read. */
    unsetenv("CNET_EVOLVE_DIRECTION");

    printf("=== a direction file must be able to say 'no factory' ===\n");

    /* 1. Key absent -> defaults. Backward compatibility: configs that never
          mentioned factory must keep the two built-in entries. */
    load_with(dir, "allow_factory=1\nmax_new_per_tick=2\n", &D);
    snprintf(det, sizeof det, "n_factory=%d (want 2)", D.n_factory);
    check(D.n_factory == 2, "key absent keeps built-in defaults", det);

    /* 2. Commented out == absent. Documents the old surprise explicitly. */
    load_with(dir,
              "allow_factory=1\n"
              "#factory=q1_add16,0,blk.0.attn_q.weight\n"
              "#factory=q1_xor16,1,blk.0.attn_k.weight\n", &D);
    snprintf(det, sizeof det, "n_factory=%d (want 2, same as absent)", D.n_factory);
    check(D.n_factory == 2, "commented-out factory == absent", det);

    /* 3. Explicit empty -> NONE. This is the case that did not exist. */
    load_with(dir, "allow_factory=1\nfactory=\n", &D);
    snprintf(det, sizeof det, "n_factory=%d (want 0)", D.n_factory);
    check(D.n_factory == 0, "'factory=' means explicitly none", det);

    /* 4. Explicit 'none' keyword -> NONE, for readability. */
    load_with(dir, "allow_factory=1\nfactory=none\n", &D);
    snprintf(det, sizeof det, "n_factory=%d (want 0)", D.n_factory);
    check(D.n_factory == 0, "'factory=none' means explicitly none", det);

    /* 5. A real list is still honoured exactly. */
    load_with(dir, "allow_factory=1\nfactory=mytag,0,blk.0.attn_q.weight\n", &D);
    snprintf(det, sizeof det, "n_factory=%d tag=%s", D.n_factory,
             D.n_factory > 0 && D.factory[0].tag ? D.factory[0].tag : "-");
    check(D.n_factory == 1 &&
              D.factory[0].tag && strcmp(D.factory[0].tag, "mytag") == 0,
          "explicit list honoured exactly", det);

    /* 6. An explicit-none must not be resurrected by a later unrelated key. */
    load_with(dir, "factory=none\nallow_factory=1\nmax_new_per_tick=4\n", &D);
    snprintf(det, sizeof det, "n_factory=%d max_new=%d", D.n_factory,
             D.max_new_per_tick);
    check(D.n_factory == 0 && D.max_new_per_tick == 4,
          "explicit none survives later keys", det);

    {
        char path[768];
        snprintf(path, sizeof path, "%s/evolve_direction.conf", dir);
        remove(path);
        rmdir(dir);
    }

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails == 0) {
        printf("EVOLVE_DIR_FACTORY_PASS checks=%d fails=0 "
               "empty_means_none=1 absent_means_default=1\n", checks);
        return 0;
    }
    printf("EVOLVE_DIR_FACTORY_RED checks=%d fails=%d\n", checks, fails);
    return 1;
}
