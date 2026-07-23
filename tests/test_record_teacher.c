/* Record-as-oracle teaching — rung 1 of the non-parrot ladder.
 *
 * The claim under test: a specialist can be mined, trained, and CERTIFIED
 * against RECORDED text (a user's correction) with no language model in the
 * loop, and afterwards the sealed unit answers from weights — reproducing
 * the record's word transitions exactly. The record is knowledge the teacher
 * LM never produced; certifying against it is the precise sense in which the
 * system stops being bounded by its teachers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/gap_lane.h"
#include "../include/cnet_auto_learn.h"
#include "../include/cnet_record_teacher.h"
#include "../include/nn.h"
#include "../include/router.h"

#define W 8

static int failures;
static int checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

static Port onehot_port(const char *tag, size_t count) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = W;
    p.field_count = count;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

int main(void) {
    const char *base_path = "tmp_record_teacher.cnb";
    const char *ledger_path = "tmp_record_teacher.gaps.txt";
    const char *inbox_path = "tmp_record_teacher.inbox";
    const char *words_path = "tmp_record_teacher.words.txt";
    const char *records_dir = "tmp_record_teacher.records";
    GapLane lane;
    GapLaneTickReport tick;
    char cmd[256];
    int i;

    remove(base_path);
    remove(ledger_path);
    remove(inbox_path);
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", records_dir, records_dir);
    if (system(cmd) != 0) { printf("fixture setup failed\n"); return 1; }

    printf("== record teacher: certify against the record, not the model ==\n");

    /* An 8-word window, sidecar format "<id><tab><word>". */
    write_file(words_path,
               "10\tthe\n11\tanswer\n12\tis\n13\twrong\n"
               "14\tport\n15\tnine\n16\tcorrect\n17\tafter\n");

    /* The record: a correction. Its in-window transitions:
       the->answer, answer->is, is->wrong (first occurrence wins),
       wrong->the (skip-over of OOV "actually,"), port->is, nine->correct.
       Note "is" appears twice; the SECOND successor (nine) must NOT
       overwrite the first (wrong). */
    write_file("tmp_record_teacher.records/skill_corr_t1.txt",
               "the answer is wrong, actually the port is nine correct after");

    /* ── unit surfaces ── */
    {
        static char words[CNET_RECORD_W_MAX][CNET_RECORD_WORD_MAX];
        CnetRecordCtx ctx;
        double in[W], out[W];
        int n = cnet_record_words_load(words_path, words, CNET_RECORD_W_MAX);
        check(n == W, "words sidecar loads all window words");
        check(strcmp(words[0], "the") == 0 && strcmp(words[7], "after") == 0,
              "sidecar parse keeps window order");

        check(cnet_record_table_build(&ctx, W, 1,
                  "the answer is wrong, actually the port is nine correct after",
                  words) == 7,
              "record compiles to its in-window transitions");
        check(ctx.next[0] == 1 && ctx.next[1] == 2 && ctx.next[2] == 3,
              "the->answer->is->wrong recorded");
        check(ctx.next[3] == 0, "OOV words are skipped over, not breaks");
        check(ctx.next[6] == 7, "correct->after recorded");
        check(ctx.next[5] == 6, "nine->correct recorded");

        memset(in, 0, sizeof in);
        in[2] = 1.0;   /* "is" */
        check(cnet_record_teacher(in, out, &ctx) == 0 && out[3] == 1.0,
              "teacher answers is->wrong (first occurrence wins)");
        memset(in, 0, sizeof in);
        in[7] = 1.0;   /* "after": no recorded successor */
        check(cnet_record_teacher(in, out, &ctx) == 0 && out[7] == 1.0,
              "unknown continuation answers identity, never abstains");
    }

    /* ── the full lane: note -> bind -> tick -> certified unit ── */
    check(gap_lane_open(&lane, base_path, ledger_path, inbox_path) == 0,
          "fresh lane opens");
    lane.acq.init_hidden = 4;
    lane.acq.max_hidden = 64;
    lane.acq.min_evidence = W;   /* tiny fixture domain */

    {
        Port in_port = onehot_port("w_cur", 1);
        Port goal_port = onehot_port("skill_corr_t1", 1);
        check(gap_inbox_note_no_plan(inbox_path, in_port, goal_port) == 0,
              "correction gap noted to the inbox");
    }

    check(cnet_record_bind(&lane, records_dir, words_path, 0) == 0,
          "bind before ingest finds no candidate gaps yet");

    /* generate-and-verify records (skill_vrf_*) are record-owned too */
    write_file("tmp_record_teacher.records/skill_vrf_t2.txt",
               "port nine is correct");
    {
        Port in_port = onehot_port("w_cur", 1);
        Port goal_port = onehot_port("skill_vrf_t2", 1);
        check(cnet_record_tag_owned("skill_vrf_t2") &&
              cnet_record_tag_owned("skill_corr_t1") &&
              cnet_record_tag_owned("skill_obs_t3") &&
              !cnet_record_tag_owned("skill_gh_x"),
              "record-owned tag families are corr_, vrf_, obs_ exactly");
        check(gap_inbox_note_no_plan(inbox_path, in_port, goal_port) == 0,
              "verified-record gap noted to the inbox");
    }

    check(gap_lane_tick(&lane, &tick, 0) == 0 && tick.inbox_ingested == 2,
          "tick ingests the correction gap");
    /* The gap either parks waiting_oracle (no teacher yet) or stays open;
       binding now must find it. */
    check(cnet_record_bind(&lane, records_dir, words_path, 0) == 2,
          "record oracles bind to both record-owned gaps");

    check(gap_lane_tick(&lane, &tick, 0) == 0 && tick.drain.closed == 2,
          "drain closes both gaps: mined from RECORDS, trained, sealed");
    check(tick.drain.last_verdict == CERT_PROVEN,
          "certified as PROOF over the whole window domain");

    /* ── the money shot: answers from weights ── */
    {
        Port in_port = onehot_port("w_cur", 1);
        Port goal_port = onehot_port("skill_corr_t1", 1);
        RoutePlan plan;
        double input[W], output[W];
        int from_weights_ok = 1;
        int expect[W] = {1, 2, 3, 0, 2, 6, 7, 7};   /* the record's function */

        memset(&plan, 0, sizeof plan);
        check(route_plan(&lane.reg, in_port, goal_port, &plan) == 0 &&
              plan.length >= 1,
              "planner routes the correction goal through the sealed unit");

        for (i = 0; i < W; i++) {
            int j, hot = -1;
            memset(input, 0, sizeof input);
            input[i] = 1.0;
            if (route_execute(&plan, input, W, output, W) != 0) {
                from_weights_ok = 0;
                break;
            }
            for (j = 0; j < W; j++)
                if (output[j] == 1.0) { hot = j; break; }
            if (hot != expect[i]) { from_weights_ok = 0; break; }
        }
        check(from_weights_ok,
              "sealed unit reproduces the record EXACTLY, from weights");
    }

    /* ── provenance: the oracle identity is the record's identity ── */
    {
        int found = 0;
        size_t g;
        for (g = 0; g < lane.ledger.count; g++)
            if (strncmp(lane.ledger.gaps[g].oracle, "rec_skill_corr_t1", 17) == 0)
                found = 1;
        check(found, "ledger records the RECORD teacher as provenance, not an LM");
    }

    printf("%s (%d checks, %d failures)\n",
           failures == 0 ? "RECORD_TEACHER_PASS" : "RECORD_TEACHER_FAIL",
           checks, failures);
    return failures != 0;
}
