/* A raw table must never answer a turn it was not addressed by.
 *
 * RED marker: CORE_SERVE_UNTAGGED_RED
 *
 * cnet_serve_result guards brick selection with
 *
 *     if (tag[0] && strcmp(tag, br->tag) != 0) continue;
 *
 * so when parse_turn yields an EMPTY tag the guard is skipped entirely and the
 * FIRST live brick in the bank answers -- with out->claimed_cert = 1.
 * parse_turn leaves the tag empty whenever the turn does not begin with a
 * letter or '_', which is every symbolic arithmetic query: "2+2" parses to
 * tag="" x=2, and whatever brick happens to have loaded first returns lut[2]
 * as a certified answer.
 *
 * Observed live on the running front door before this gate existed, with the
 * scenario fixture bricks in CNET_CORE_BUS_BRICKS_DIR:
 *     "2+2"      -> user_pref -> 3  verified=1   (2+2 is 4)
 *     "3+4"      -> user_pref -> 4  verified=1   (3+4 is 7)
 *     "7 apples" -> user_pref -> 8  verified=1
 * Bank order is readdir order, so WHICH brick answers is arbitrary.
 *
 * This is the coverage/abstain law -- "a unit never answers outside its
 * certified domain" -- violated on the live path, and it fails OPEN rather
 * than closed: it emits a confident wrong CERT instead of abstaining.
 *
 * Correct behaviour: an unaddressed turn abstains. Bricks are always addressed
 * by tag ("user_pref 7", "q1_add16 3"); there is no such thing as a turn that
 * legitimately means "whichever brick you loaded first".
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_core_serve.h"

static int checks = 0, fails = 0;

static void check(int cond, const char *msg, const char *detail) {
    checks++;
    if (cond) {
        printf("  ok   %-46s %s\n", msg, detail ? detail : "");
    } else {
        fails++;
        printf("  FAIL %-46s %s\n", msg, detail ? detail : "");
    }
}

static void add_brick(CnetServeBank *b, const char *tag, const char *name,
                      float base) {
    CnetServeBrick *br = &b->bricks[b->n++];
    int i;
    memset(br, 0, sizeof *br);
    snprintf(br->tag, sizeof br->tag, "%s", tag);
    snprintf(br->name, sizeof br->name, "%s", name);
    for (i = 0; i < 16; ++i) br->lut[i] = (float)(((unsigned)base + (unsigned)i) & 15u);
    br->live = 1;
}

int main(void) {
    CnetServeBank b;
    CnetServeResult r;
    char det[160];

    printf("=== Raw table selection never grants certification ===\n");

    cnet_serve_bank_init(&b);
    /* Two bricks so "first in the bank" is a real, arbitrary choice. */
    add_brick(&b, "alpha", "alpha_v1", 100.0f);
    add_brick(&b, "beta", "beta_v1", 200.0f);

    /* Addressed turns still work -- the fix must not break normal serving. */
    memset(&r, 0, sizeof r);
    cnet_serve_result(&b, "alpha 3", &r);
    snprintf(det, sizeof det, "proved=%d brick=%s out=%u", r.proved, r.brick,
             r.out_nibble);
    check(r.proved == 0 && r.claimed_cert == 0 && !r.abstained && r.out_nibble == 7 &&
          strcmp(r.brick, "alpha_v1") == 0,
          "addressed alpha calculates without certification", det);

    memset(&r, 0, sizeof r);
    cnet_serve_result(&b, "beta 3", &r);
    snprintf(det, sizeof det, "proved=%d brick=%s", r.proved, r.brick);
    check(r.proved == 0 && r.claimed_cert == 0 && !r.abstained && r.out_nibble == 11 &&
          strcmp(r.brick, "beta_v1") == 0,
          "addressed turn 'beta 3' picks beta not alpha", det);

    /* A tag that matches nothing must abstain -- this already worked. */
    memset(&r, 0, sizeof r);
    cnet_serve_result(&b, "gamma 3", &r);
    snprintf(det, sizeof det, "proved=%d abstained=%d", r.proved, r.abstained);
    check(r.proved == 0 && r.abstained == 1,
          "unknown tag 'gamma 3' abstains", det);

    /* The defect: unaddressed turns. Each of these must abstain, and above all
       must never come back claimed_cert. */
    {
        const char *unaddressed[] = { "2+2", "3+4", "7 apples", "5", "12 items" };
        size_t i;
        for (i = 0; i < sizeof unaddressed / sizeof unaddressed[0]; ++i) {
            memset(&r, 0, sizeof r);
            cnet_serve_result(&b, unaddressed[i], &r);
            /* Bound the copied fields: r.spoken is CNET_SERVE_TEXT (256) and
               would overflow det, which -Werror=format-truncation catches. */
            snprintf(det, sizeof det,
                     "%-10s proved=%d claimed_cert=%d brick=%.24s spoken=%.40s",
                     unaddressed[i], r.proved, r.claimed_cert,
                     r.brick[0] ? r.brick : "-", r.spoken);
            check(r.proved == 0 && r.claimed_cert == 0 && r.abstained == 1,
                  "unaddressed turn abstains, never claims CERT", det);
        }
    }

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails == 0) {
        printf("CORE_SERVE_UNTAGGED_PASS checks=%d fails=0 "
               "unaddressed_claims_cert=0 broader_claims=WITHHELD\n", checks);
        return 0;
    }
    printf("CORE_SERVE_UNTAGGED_RED checks=%d fails=%d\n", checks, fails);
    return 1;
}
