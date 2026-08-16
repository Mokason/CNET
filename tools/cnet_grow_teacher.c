/* 8B teacher → grow_lobe student.
   Runs until the student hits the learn bar, not a loop count. */
#include "../include/cnet_grow_lobe.h"
#include "../include/cnet_held_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const k_curriculum[] = {
    "Write two plain English sentences about rain on a roof.",
    "Describe a small kitchen in simple English.",
    "Explain why people sleep, in short English.",
    "Tell a short story about a lost key.",
    "Describe the smell of bread in an oven.",
    "Write three sentences about a river in spring.",
    "Explain what a library is for a child.",
    "Describe walking home after dark.",
    "Write about a dog waiting at a door.",
    "Explain how to boil water, simply.",
    "Describe a market on Saturday morning.",
    "Write two sentences about snow on trees.",
    "Explain why we ask questions.",
    "Describe a window with afternoon light.",
    "Write about a letter that never arrived.",
    "Explain what a map is used for.",
    "Describe the sound of rain on leaves.",
    "Write three sentences about a tired traveler.",
    "Explain how a seed becomes a plant.",
    "Describe a quiet street at dawn.",
    "Write about sharing food with a neighbor.",
    "Explain why books have pages.",
    "Describe a cat in a sunny chair.",
    "Write two sentences about the sea.",
    "Explain what work means in simple words.",
    "Describe a child learning to read.",
    "Write about a bridge over a stream.",
    "Explain why fire is hot, simply.",
    "Describe a winter coat on a hook.",
    "Write three sentences about friendship.",
    "Explain how day turns into night.",
    "Describe the taste of an apple.",
    "Write about a train leaving a station.",
    "Explain what a promise is.",
    "Describe a garden after rain.",
    "Write two sentences about a wooden table.",
    "Explain why we use names.",
    "Describe a bird on a fence.",
    "Write about finding a path in the woods.",
    "Explain what home means, simply.",
    "Describe a cup of tea on a desk.",
    "Write three sentences about a storm.",
    "Explain how people say thank you.",
    "Describe a dusty road in summer.",
    "Write about a lamp in a dark room.",
    "Explain why stories are told.",
    "Describe an old pair of shoes.",
    "Write two sentences about the moon.",
    "Explain what a question mark does.",
    "Describe a baker at first light.",
    "Write about a locked wooden box.",
    "Explain why water freezes.",
    "Describe a classroom with open windows.",
    "Write three sentences about a long walk.",
    "Explain what a calendar is for.",
    "Describe a harbor with small boats.",
    "Write about a song heard from another room.",
    "Explain why we rest after work.",
    "Describe a field of dry grass.",
    "Write two sentences about a clock.",
    "Explain what a neighbor is.",
    "Describe rain running down glass.",
    "Write about a door that will not close.",
    "Explain why people plant trees.",
    "Describe a market stall of oranges.",
    "Write three sentences about a quiet night.",
    "Explain how a letter is sent.",
    "Describe a hill above a town.",
    "Write about a torn map in a pocket.",
    "Explain what patience is, simply.",
    "Describe smoke from a chimney.",
    "Write two sentences about a river stone.",
    "Explain why we count things.",
    "Describe a wet dog shaking itself.",
    "Write about a window left open.",
    "Explain what a beginning is.",
    "Describe frost on a fence wire.",
    "Write three sentences about a kind stranger.",
    "Explain why the wind moves trees.",
    "Describe a bowl of warm soup.",
    "Write about a path worn in grass.",
    "Explain what it means to wait.",
    "Describe a bicycle against a wall.",
    "Write two sentences about a paper boat.",
    "Explain why people keep diaries.",
    "Describe a candle almost gone.",
    "Write about a name carved in wood.",
    "Explain what a harbor is for.",
    "Describe dust in a beam of light.",
    "Write three sentences about leaving home.",
    "Explain why rain helps farms.",
    "Describe a stair that creaks.",
    "Write about a coat left on a chair.",
    "Explain what a horizon is.",
    "Describe children playing in a yard.",
    "Write two sentences about a quiet well.",
    "Explain why we close doors at night.",
    "Describe a red barn in a field.",
    "Write about a last match in a box.",
    "Explain what it means to listen.",
    NULL
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    CnetGrowLobe *l = NULL;
    int n_cur = 0, i, got = 0, failed = 0, streak = 0, miss_row = 0;
    double t0, loss = 99.0, acc = 0.0, acc1 = 0.0, acc_all = 0.0;
    char spoken[160];
    char wrap[640];
    const char *ep;

    for (n_cur = 0; k_curriculum[n_cur] != NULL; ++n_cur) {
    }

    ep = getenv("CNET_HELD_MODEL_ENDPOINT");
    if (ep == NULL || ep[0] == '\0')
        cnet_held_model_set_endpoint(
            "http://127.0.0.1:8081/v1/chat/completions");
    else
        cnet_held_model_set_endpoint(ep);

    printf("CNET_GROW_TEACHER start fuel=%d done=recent8>=0.80&loss<=1.20"
           "&streak=3 no_loop_cap fp_recent=1\n",
           n_cur);
    fflush(stdout);
    if (cnet_grow_open(&l) != 0) {
        printf("CNET_GROW_TEACHER_FAIL open\n");
        return 1;
    }
    cnet_grow_seed_english(l);
    cnet_grow_seed_chat_template(l);
    t0 = now_s();

    for (i = 0;; ++i) {
        const char *one[1];
        const char *ask;
        int n, ok;

        if (i < n_cur) {
            snprintf(wrap, sizeof wrap,
                     "Reply in one short sentence of simple English. %s",
                     k_curriculum[i]);
            ask = wrap;
        } else {
            snprintf(wrap, sizeof wrap,
                     "Reply in one short sentence of simple English. "
                     "Say this again in new words: %s",
                     k_curriculum[i % n_cur]);
            ask = wrap;
        }
        one[0] = ask;
        n = cnet_grow_distill_held(l, one, 1);
        if (n == 1) {
            got++;
            miss_row = 0;
            /* Replay the window first, then overfit the new line on top. */
            cnet_grow_train_recent(l, 8, 3, &loss);
            cnet_grow_train_last(l, 20, &loss);
        } else {
            failed++;
            miss_row++;
            printf("  miss i=%d row=%d\n", i + 1, miss_row);
        }
        if (miss_row >= 8) {
            printf("CNET_GROW_TEACHER_FAIL teacher_dead miss_row=%d\n",
                   miss_row);
            cnet_grow_close(l);
            cnet_held_model_close();
            return 3;
        }

        /* Keep the in-house English table in the recent window. */
        if (got > 0 && (got % 25) == 0)
            cnet_grow_seed_english(l);

        ok = 0;
        if (got >= 8 && (got % 2) == 0) {
            /* Rare full replay, FP (train(1) must not flip QAT). */
            if ((got % 20) == 0)
                cnet_grow_train(l, 1, &loss);
            if (cnet_grow_prefix_acc_recent(l, 1, &acc1) != 0) acc1 = 0.0;
            if (cnet_grow_prefix_acc_recent(l, 8, &acc) != 0) acc = 0.0;
            if (cnet_grow_prefix_acc(l, &acc_all) != 0) acc_all = 0.0;
            if (acc >= 0.80 && loss <= 1.20)
                streak++;
            else
                streak = 0;
            ok = streak >= 3;
        }
        printf("  i=%d distilled=%d miss=%d n=%d loss=%.4f acc1=%.3f "
               "acc8=%.3f acc_all=%.3f streak=%d\n",
               i + 1, got, failed, cnet_grow_count(l), loss, acc1, acc,
               acc_all, streak);
        fflush(stdout);
        if (ok) break;
    }

    cnet_grow_speak(l, "the cat sat on the ma", spoken, sizeof spoken, 12);
    printf("CNET_GROW_TEACHER_DONE distilled=%d miss=%d n=%d refused=%d "
           "loss=%.4f acc=%.3f streak=%d speak=[%s] may_speak=%d "
           "claimed_cert=0 sec=%.1f python=0\n",
           got, failed, cnet_grow_count(l), cnet_grow_refused(l), loss, acc,
           streak, spoken, cnet_grow_may_speak(l), now_s() - t0);
    cnet_grow_close(l);
    cnet_held_model_close();
    return 0;
}
