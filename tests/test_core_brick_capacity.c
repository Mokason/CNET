/* BRICK_CAPACITY_RED: capacity is storage, never certification evidence. */
#include "cnet_core_bus.h"
#include "cnet_core_serve.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures;
static void check(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "BRICK_CAPACITY_RED %s\n", name); failures++; }
}
static void make_table(const char *dir, unsigned index) {
    char tag[32];
    float lut[16];
    snprintf(tag, sizeof tag, "capacity_%03u", index);
    for (unsigned key = 0; key < 16; key++) lut[key] = (float)((key + index) & 15u);
    if (cnet_serve_save_lut(dir, tag, tag, lut) != 0) exit(2);
}
int main(void) {
    char dir[] = "/tmp/cnet-brick-capacity-XXXXXX", path[256], turn[64];
    CnetServeBank bank, prior, initial;
    CnetServeResult result;
    unsigned verified = 0;
    check(CNET_SERVE_MAX_BRICKS == 256 && CNET_CORE_BUS_MAX_BRICKS == 256,
          "both bounded banks must support 256");
    check(sizeof(CnetServeBank) <= 128 * 1024 && sizeof(CnetCoreBus) <= 4 * 1024 * 1024,
          "bounded inline memory");
    if (!mkdtemp(dir)) return 2;
    for (unsigned i = 0; i < 25; i++) make_table(dir, i);
    check(cnet_serve_bank_load_dir(&bank, dir) == 0 && bank.n == 25, "25th brick loads");
    for (unsigned i = 25; i < 256; i++) make_table(dir, i);
    check(cnet_serve_bank_reload(&bank) == 0 && bank.n == 256, "full expanded bank reloads");
    for (unsigned i = 0; i < 256; i++) for (unsigned key = 0; key < 16; key++) {
        snprintf(turn, sizeof turn, "capacity_%03u %u", i, key);
        if (cnet_serve_result(&bank, turn, &result) == 0 &&
            result.out_nibble == ((key + i) & 15u) && !result.proved && !result.claimed_cert)
            verified++;
    }
    check(verified == 4096, "all named table values match; all uncertified");
    if (setenv("CNET_CORE_BUS_BRICKS_DIR", dir, 1)) return 2;
    check(cnet_serve_global_load_env() == 0 && cnet_serve_global()->n == 256,
          "global bank loads full snapshot");
    CnetCoreBus pending = {0}, pending_before;
    pending.state = CNET_CORE_BUS_CERTIFIED;
    pending.student_live = pending.certified = 1;
    snprintf(pending.name, sizeof pending.name, "pending");
    for (unsigned i = 0; i < 16; i++) pending.lut_table[i] = (float)i;
    pending_before = pending;
    check(cnet_core_bus_park_brick(&pending, "pending") < 0 &&
          memcmp(&pending, &pending_before, sizeof pending) == 0,
          "persistence refusal keeps pending ownership and propagates to factory");
    snprintf(path, sizeof path, "%s/pending.lut", dir); unlink(path);
    prior = bank;
    float identity[16]; for (unsigned i = 0; i < 16; i++) identity[i] = (float)i;
    snprintf(path, sizeof path, "%s/overflow.lut", dir);
    check(cnet_serve_save_lut(dir, "overflow", "overflow", identity) < 0 &&
          access(path, F_OK) != 0, "publisher refuses the 257th file before writing");
    unlink(path); /* exact fixture only, including cleanup on the RED version */
    check(cnet_serve_save_lut(dir, "capacity_000", "replacement", identity) == 0,
          "replacement at full capacity consumes no additional slot");
    snprintf(path, sizeof path, "%s/capacity_256.lut", dir);
    FILE *f = fopen(path, "w"); if (!f) return 2;
    fputs("tag=capacity_256\nlut=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n", f); fclose(f);
    check(cnet_serve_global_load_env() < 0 && cnet_serve_global() &&
          cnet_serve_global()->n == 256, "global compose/reload path retains previous bank");
    check(cnet_serve_bank_reload(&bank) < 0 && memcmp(&bank, &prior, sizeof bank) == 0,
          "overflow reload retains the complete previous bank and counters");
    check(cnet_serve_bank_load_dir(&initial, dir) < 0 && initial.n == 0,
          "overflow startup never exposes a partial bank");
    snprintf(path, sizeof path, "%s/capacity_256.lut", dir); unlink(path);
    snprintf(path, sizeof path, "%s/broken.lut", dir);
    f = fopen(path, "w"); if (!f) return 2;
    fputs("tag=bad\nlut=nan\n", f); fclose(f);
    check(cnet_serve_bank_reload(&bank) < 0 && memcmp(&bank, &prior, sizeof bank) == 0,
          "malformed reload retains complete previous bank");
    unlink(path);
    snprintf(path, sizeof path, "%s/capacity_255.lut", dir); unlink(path);
    if (cnet_serve_save_lut(dir, "duplicate", "duplicate", identity)) return 2;
    snprintf(path, sizeof path, "%s/duplicate.lut", dir);
    f = fopen(path, "w"); if (!f) return 2;
    fputs("tag=capacity_000\nlut=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n", f); fclose(f);
    check(cnet_serve_bank_reload(&bank) < 0 && memcmp(&bank, &prior, sizeof bank) == 0,
          "duplicate tag cannot make lookup depend on directory ordering");
    unlink(path);
    check(cnet_serve_bank_reload(&bank) == 0 && bank.n == 255 && bank.proves == prior.proves &&
          bank.reloads == prior.reloads + 1, "valid reload replaces snapshot and preserves counters");
    check(cnet_serve_result(&bank, "capacity_000 16", &result) != 0 && result.abstained,
          "outside domain remains refused");
    snprintf(path, sizeof path, "%s/invalid.lut", dir);
    identity[0] = NAN;
    check(cnet_serve_save_lut(dir, "invalid", "invalid", identity) < 0 &&
          access(path, F_OK) != 0, "publisher refuses invalid numeric data");
    unlink(path);
    identity[0] = 0;
    snprintf(path, sizeof path, "%s/bad\ntag.lut", dir);
    check(cnet_serve_save_lut(dir, "bad\ntag", "invalid", identity) < 0 &&
          access(path, F_OK) != 0, "publisher refuses multiline tags");
    unlink(path);
    snprintf(path, sizeof path, "%s/invalid.lut", dir);
    check(cnet_serve_save_lut(dir, "invalid", "bad\nname", identity) < 0 &&
          access(path, F_OK) != 0, "publisher refuses multiline names");
    unlink(path);
    const char *bad_tags[] = {" ", " capacity_000", "bad tag", "0bad"};
    for (unsigned i = 0; i < sizeof bad_tags / sizeof bad_tags[0]; i++) {
        snprintf(path, sizeof path, "%s/%s.lut", dir, bad_tags[i]);
        check(cnet_serve_save_lut(dir, bad_tags[i], "invalid", identity) < 0 &&
              access(path, F_OK) != 0, "publisher requires canonical addressable tags");
        unlink(path);
    }
    pid_t writers[2];
    for (int i = 0; i < 2; i++) {
        writers[i] = fork();
        if (writers[i] < 0) return 2;
        if (!writers[i])
            _exit(cnet_serve_save_lut(dir, i ? "race_b" : "race_a", "race", identity) == 0 ? 0 : 1);
    }
    int successes = 0;
    for (int i = 0; i < 2; i++) {
        int status;
        if (waitpid(writers[i], &status, 0) != writers[i]) return 2;
        check(WIFEXITED(status), "publisher child exits normally");
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) successes++;
    }
    check(successes == 1 && cnet_serve_bank_load_dir(&initial, dir) == 0 && initial.n == 256,
          "competing publishers cannot consume the same final slot");
    snprintf(path, sizeof path, "%s/race_a.lut", dir); unlink(path);
    snprintf(path, sizeof path, "%s/race_b.lut", dir); unlink(path);
    for (unsigned i = 0; i < 256; i++) {
        snprintf(path, sizeof path, "%s/capacity_%03u.lut", dir, i); unlink(path);
    }
    rmdir(dir);
    printf("BRICK_CAPACITY_%s failures=%d slots=%d checked_values=%u raw_certified=0 "
           "serve_bank_bytes=%zu core_bus_bytes=%zu\n", failures ? "RED" : "PASS", failures,
           CNET_SERVE_MAX_BRICKS, verified, sizeof(CnetServeBank), sizeof(CnetCoreBus));
    return failures ? 1 : 0;
}
