/* Gate: RSI live loop — coverage grows back under verifiers.
 *
 *   make rsi_live_proof  ->  RSI_LIVE_PASS
 *
 * RSI here means exactly one thing: a CERT brick that goes missing comes back
 * through miss -> typed_miss -> evolve -> reload -> retry, and NOTHING about
 * that path lets an uncertified answer claim CERT. It is not self-rewrite.
 *
 * The gate deletes a real brick, so it backs the brick up first and restores it
 * on EVERY exit path, including failure. A gate that can leave the host with
 * less coverage than it found is not a safety gate.
 *
 * Scenarios
 *   1 baseline        brick serves LOCAL
 *   2 chain recovery  delete .lut, chain ask -> LOCAL in one shot (sync evolve)
 *   3 single recovery delete .lut, repeat ask -> LOCAL within EVERY+1 asks
 *                     (rate-limited async tick; deterministic, not lucky)
 *   4 fail-safe       force a mint failure -> .gguf is left INTACT
 *   5 law             a miss never reports CLAIMED_CERT 1; evolve reports
 *                     residual_auto_cert=0
 *
 * Needs cnetd running with CNET_CORE_AUTO_EVOLVE=1.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define BUF 16384
static int failures, checks;
static char g_bricks[512], g_lut[640], g_gguf[640], g_bak_lut[640], g_bak_gguf[640];

static void check(int ok, const char *m) {
    checks++;
    printf("  %-58s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int run(const char *cmd, char *out, size_t cap) {
    FILE *p = popen(cmd, "r");
    size_t n;
    int rc;
    if (out && cap) out[0] = 0;
    if (!p) return -1;
    n = out && cap ? fread(out, 1, cap - 1, p) : 0;
    if (out && cap) out[n] = 0;
    rc = pclose(p);
    return rc;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static long fsize(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void copyf(const char *from, const char *to) {
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "cp -a '%s' '%s' 2>/dev/null", from, to);
    (void)run(cmd, NULL, 0);
}

/* Restore whatever we borrowed. Safe to call repeatedly. */
static void restore_brick(void) {
    if (g_bak_lut[0] && exists(g_bak_lut)) copyf(g_bak_lut, g_lut);
    if (g_bak_gguf[0] && exists(g_bak_gguf)) copyf(g_bak_gguf, g_gguf);
}

static int field(const char *buf, const char *key, char *out, size_t cap) {
    const char *p = buf;
    size_t klen = strlen(key);
    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, klen) && p[klen] == ' ') {
            const char *s = p + klen + 1, *e = strchr(s, '\n');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n >= cap) n = cap - 1;
            memcpy(out, s, n);
            out[n] = 0;
            return 0;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

static int ask(const char *q, char *out, size_t cap) {
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "./bin/cnet_peer '%s' 2>&1", q);
    return run(cmd, out, cap);
}

static int ask_source(const char *q, char *src, size_t cap) {
    char buf[BUF];
    ask(q, buf, sizeof buf);
    return field(buf, "SOURCE", src, cap);
}

int main(void) {
    char buf[BUF], src[64], v[64];
    const char *tag = "q1_xor16";
    const char *env_every = getenv("CNET_CORE_EVOLVE_EVERY");
    int every = env_every && env_every[0] ? atoi(env_every) : 4;
    int i, recovered;
    long gg_before, gg_after;

    failures = checks = 0;
    if (every < 1) every = 4;
    printf("=== RSI live proof (miss -> evolve -> reload -> retry) ===\n");

    snprintf(g_bricks, sizeof g_bricks, "%s",
             getenv("CNET_CORE_BUS_BRICKS_DIR")
                 ? getenv("CNET_CORE_BUS_BRICKS_DIR")
                 : "/home/marble/.local/share/cnet-bricks");
    snprintf(g_lut, sizeof g_lut, "%s/%s.lut", g_bricks, tag);
    snprintf(g_gguf, sizeof g_gguf, "%s/%s.gguf", g_bricks, tag);
    snprintf(g_bak_lut, sizeof g_bak_lut, "/tmp/rsi_gate_%s.lut", tag);
    snprintf(g_bak_gguf, sizeof g_bak_gguf, "/tmp/rsi_gate_%s.gguf", tag);

    /* --- 0. preconditions + backup BEFORE touching anything --- */
    check(exists(g_lut) && exists(g_gguf), "brick present before the proof");
    if (!exists(g_lut) || !exists(g_gguf)) {
        printf("\n  refusing to run: %s is already incomplete\n", tag);
        printf("checks=%d failures=%d\nRSI_LIVE_FAIL\n", checks, failures + 1);
        return 1;
    }
    copyf(g_lut, g_bak_lut);
    copyf(g_gguf, g_bak_gguf);
    check(exists(g_bak_lut) && exists(g_bak_gguf), "brick backed up (restored on exit)");

    run("./bin/cnet_peer PING 2>&1", buf, sizeof buf);
    check(strstr(buf, "PONG") != NULL, "cnetd reachable");
    if (!strstr(buf, "PONG")) {
        restore_brick();
        printf("\n  start cnetd: systemctl --user start cnetd.service\n");
        printf("checks=%d failures=%d\nRSI_LIVE_FAIL\n", checks, failures + 1);
        return 1;
    }

    /* --- 1. baseline --- */
    ask_source("q1_xor16 5", src, sizeof src);
    check(!strcmp(src, "LOCAL"), "baseline: brick serves SOURCE LOCAL");

    /* --- 2. chain recovery (sync evolve path) --- */
    unlink(g_lut);
    ask("q1_add16 3 then q1_xor16", buf, sizeof buf);
    field(buf, "SOURCE", src, sizeof src);
    check(!strcmp(src, "LOCAL") && exists(g_lut),
          "chain ask remints the brick in one shot");

    /* --- 3. single-tag recovery (rate-limited async tick) --- */
    restore_brick();
    unlink(g_lut);
    recovered = 0;
    for (i = 1; i <= every + 2; i++) {
        if (ask_source("q1_xor16 5", src, sizeof src) == 0 && !strcmp(src, "LOCAL")) {
            recovered = i;
            break;
        }
        sleep(1);
    }
    {
        char lbl[96];
        snprintf(lbl, sizeof lbl, "single-tag ask recovers within %d asks (took %d)",
                 every + 2, recovered);
        check(recovered > 0, lbl);
    }

    /* --- 4. a FAILED mint must not destroy working coverage --- */
    restore_brick();
    gg_before = fsize(g_gguf);
    unlink(g_lut);
    {
        char cmd[900];
        snprintf(cmd, sizeof cmd,
                 "env CNET_CORE_AUTO_EVOLVE=1 CNET_CORE_EVOLVE_FACTORY=1 "
                 "CNET_GGUF_MMAP=0 CNET_CORE_BUS_BRICKS_DIR='%s' "
                 "./bin/cnet_core_evolve --once 2>&1", g_bricks);
        run(cmd, buf, sizeof buf);
    }
    gg_after = fsize(g_gguf);
    check(strstr(buf, "FACTORY_MINT_FAILED") != NULL, "failed mint is reported loudly");
    check(gg_before > 0 && gg_before == gg_after,
          "failed mint leaves the .gguf intact (no destructive unlink)");
    {
        char cmd[700];
        snprintf(cmd, sizeof cmd, "ls '%s'/*.mint.tmp 2>/dev/null | wc -l", g_bricks);
        run(cmd, v, sizeof v);
        check(atoi(v) == 0, "failed mint leaves no temp litter");
    }

    /* --- 5. law: nothing on the miss path claims CERT --- */
    ask("q1_xor16 5", buf, sizeof buf);           /* .lut still deleted here */
    if (field(buf, "CLAIMED_CERT", v, sizeof v) == 0)
        check(strcmp(v, "1") != 0, "miss turn never reports CLAIMED_CERT 1");
    else
        check(1, "miss turn never reports CLAIMED_CERT 1 (field absent)");
    field(buf, "SOURCE", src, sizeof src);
    check(strcmp(src, "LOCAL") != 0 || exists(g_lut),
          "SOURCE LOCAL only ever appears with a real brick on disk");

    {
        char cmd[900];
        snprintf(cmd, sizeof cmd,
                 "env CNET_CORE_AUTO_EVOLVE=1 CNET_CORE_BUS_BRICKS_DIR='%s' "
                 "./bin/cnet_core_evolve --once 2>&1", g_bricks);
        run(cmd, buf, sizeof buf);
    }
    check(strstr(buf, "residual_auto_cert=0") != NULL,
          "evolve still reports residual_auto_cert=0");

    /* --- always give the host its brick back --- */
    restore_brick();
    check(exists(g_lut) && exists(g_gguf), "brick restored after the proof");
    ask_source("q1_xor16 5", src, sizeof src);
    check(!strcmp(src, "LOCAL"), "brick serves LOCAL again after the proof");

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("RSI_LIVE_FAIL\n");
        return 1;
    }
    printf("RSI_LIVE_PASS\n");
    return 0;
}
