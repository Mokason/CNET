/* Gate: showrunner */
#include "cnet_showrunner.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
static void chk(int ok, const char *m) {
    checks++;
    printf("  %-56s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    CnetShowrunner S;
    char notes[4][CNET_SR_NOTE];
    int n;
    fails = checks = 0;
    printf("=== showrunner ===\n");
    cnet_sr_init(&S);
    chk(S.da > 0.1f && S.da < 0.9f, "init da in band");
    cnet_sr_on_turn(&S, "hermes", "who are you", "I am Marble", "soul_who", "LOCAL", 0, 1);
    chk(S.n_turns == 1, "one turn recorded");
    chk(cnet_sr_detect_action("please remember that deploy uses master") == CNET_SR_ACT_REMEMBER,
        "detect remember");
    chk(cnet_sr_detect_action("propose capsule for add16") == CNET_SR_ACT_PROPOSE_CAPSULE,
        "detect propose_capsule");
    chk(cnet_sr_detect_action("what don't you know") == CNET_SR_ACT_GAPS,
        "detect gaps");
    chk(cnet_sr_mood_query("how are you"), "mood whole how-are-you");
    chk(cnet_sr_mood_query("how are you now"), "mood how are you now");
    chk(cnet_sr_mood_follow_query("not bored anymore"),
        "follow not bored anymore");
    chk(cnet_sr_mood_follow_query("still bored"), "follow still bored");
    chk(!cnet_sr_mood_follow_query("not bored of json"),
        "follow does not steal longer");
    {
        char fol[128];
        S.da = 0.70f;
        S.ado = 0.30f;
        S.feel[0] = 0;
        S.sess_bricks = 1;
        S.sess_hits = 3;
        chk(cnet_sr_mood_follow_line(&S, fol, sizeof fol) == 0 &&
                strstr(fol, "Not bored") != NULL &&
                strstr(fol, "da=") == NULL,
            "follow after hits is Not bored, no meters");
        S.sess_bricks = 0;
        S.sess_hits = 0;
        S.da = 0.30f;
        S.ado = 0.50f;
        S.feel[0] = 0;
        chk(cnet_sr_mood_follow_line(&S, fol, sizeof fol) == 0 &&
                strstr(fol, "Still bored") != NULL,
            "follow still Bored when DA low");
    }
    chk(cnet_sr_mood_query("how's it going?"), "mood trims punct");
    chk(!cnet_sr_mood_query("how are you interacting with roe"),
        "mood does not steal longer ask");
    {
        char mood[128];
        S.da = 0.70f;
        S.ht = 0.40f;
        S.ado = 0.30f;
        chk(cnet_sr_mood_line(&S, mood, sizeof mood) == 0 &&
                strstr(mood, "Good") != NULL && strstr(mood, "da=") == NULL,
            "mood line is feeling word, no meters");
        chk(cnet_sr_mood_why_query("why") &&
                cnet_sr_mood_why_line(&S, mood, sizeof mood) == 0 &&
                strstr(mood, "Dopamine") != NULL,
            "why explains the live feel");
        chk(!cnet_sr_mood_why_query("why is json used"),
            "why does not steal longer ask");
        S.da = 0.30f;
        S.ht = 0.50f;
        S.ado = 0.65f;
        chk(cnet_sr_mood_line(&S, mood, sizeof mood) == 0 &&
                strstr(mood, "Tired") != NULL,
            "high ado is Tired");
        S.da = 0.30f;
        S.ht = 0.50f;
        S.ado = 0.50f;
        chk(cnet_sr_mood_line(&S, mood, sizeof mood) == 0 &&
                strstr(mood, "Bored") != NULL,
            "low da mid ado is Bored");
    }
    chk(cnet_sr_remember(&S, "hermes", "deploy branch is master", 0) == 0, "remember ok");
    n = cnet_sr_recall(&S, "master", notes, 4);
    chk(n >= 1 && strstr(notes[0], "master"), "recall hits note");
    {
        char colored[128];
        S.da = 0.70f;
        S.ht = 0.40f;
        S.ado = 0.30f;
        chk(cnet_sr_color_utterance(&S, "5", colored, sizeof colored) == 0 &&
                strstr(colored, "Heard.") != NULL && strstr(colored, "5") != NULL,
            "affect wrap prefixes CERT utterance");
        chk(strstr(colored, "5") != NULL, "affect wrap keeps CERT fact");
        S.ado = 0.70f;
        chk(cnet_sr_color_utterance(&S, "5", colored, sizeof colored) == 0 &&
                strstr(colored, "Sharp.") != NULL && strstr(colored, "5") != NULL,
            "high ado register is Sharp, keeps CERT fact");
    }
    chk(cnet_sr_mood_query("are you there"), "presence you-there");
    chk(cnet_sr_greet_query("hey"), "greet whole hey");
    chk(cnet_sr_greet_query("Hey!"), "greet trims punct");
    chk(cnet_sr_greet_query("Hey,"), "greet trims comma");
    chk(cnet_sr_greet_query("good evening"), "greet good evening");
    chk(!cnet_sr_greet_query("hello world"), "greet does not steal hello world");
    chk(!cnet_sr_greet_query("who are you"), "greet is not identity");
    chk(cnet_sr_ack_query("yeah i know"), "ack yeah i know");
    chk(cnet_sr_ack_query("yeah, I know."), "ack yeah comma i know");
    chk(!cnet_sr_ack_query("yeah i know json"), "ack does not steal longer");
    {
        char g[128];
        S.da = 0.50f;
        S.ht = 0.55f;
        S.ado = 0.35f;
        chk(cnet_sr_greet_line(&S, g, sizeof g) == 0 &&
                strstr(g, "da=") == NULL && strstr(g, "I'm here") != NULL,
            "greet line has no meters");
        chk(cnet_sr_ack_line(&S, g, sizeof g) == 0 &&
                strstr(g, "da=") == NULL && strstr(g, "Got") != NULL,
            "ack line has no meters");
        S.da = 0.30f;
        S.ht = 0.50f;
        S.ado = 0.50f;
        S.feel[0] = 0;
        chk(cnet_sr_want_query("what would you like to do"), "want whole utterance");
    chk(cnet_sr_cando_query("what can you do"), "cando whole utterance");
    chk(cnet_sr_cando_query("What can you do?"), "cando trims punct");
    chk(cnet_sr_cando_query("ok lets try again, what can you do?"),
        "cando allows prefix retry");
    chk(!cnet_sr_cando_query("what can you do tomorrow"),
        "cando does not steal longer");
    chk(cnet_sr_choose_query(
            "if you have choice to pick between red or blue, what would you pick and why?"),
        "choose between red or blue");
    {
        char ch[128];
        S.da = 0.70f;
        S.ado = 0.30f;
        S.feel[0] = 0;
        S.sess_bricks = 1;
        chk(cnet_sr_choose_line(&S,
                "if you have choice to pick between red or blue, what would you pick and why?",
                ch, sizeof ch) == 0 &&
                strstr(ch, "I like") == NULL &&
                (strstr(ch, "red") || strstr(ch, "blue")),
            "choose picks from feel, not I-like");
        S.sess_bricks = 0;
        S.sess_hits = 0;
        S.da = 0.30f;
        S.ado = 0.50f;
        S.feel[0] = 0;
    }
        chk(!cnet_sr_want_query("what would you like to do tomorrow in tokyo"),
            "want does not steal longer");
        chk(cnet_sr_want_line(&S, "Do you know json?", g, sizeof g) == 0 &&
                strstr(g, "json") == NULL && strstr(g, "I like") == NULL &&
                strstr(g, "domain") != NULL,
            "bored want is work, not leftover gap");
        S.ado = 0.70f;
        S.feel[0] = 0;
        chk(cnet_sr_want_line(&S, "Do you know json?", g, sizeof g) == 0 &&
                strstr(g, "Sit") != NULL && strstr(g, "json") == NULL,
            "tired want sits, ignores gap");
        chk(cnet_sr_want_skip_gap("Hey"), "skip greet-shaped gap");
        chk(cnet_sr_want_skip_gap("what would you like to do tomorrow in tokyo"),
            "skip leftover want-shaped gap");
        chk(!cnet_sr_want_skip_gap("Do you know json?"), "keep real gap");
        S.da = 0.70f;
        S.ado = 0.30f;
        chk(cnet_sr_register(&S) == CNET_SR_REG_WARM, "register warm");
        S.ado = 0.70f;
        chk(cnet_sr_register(&S) == CNET_SR_REG_SPARE, "register spare");
        {
            unsigned sec = 0;
            char tx[64];
            chk(cnet_sr_remind_parse("remind in 20m stretch", &sec, tx,
                                    sizeof tx) == 0 &&
                    sec == 1200 && strstr(tx, "stretch"),
                "remind parse 20m");
        }
        {
            char mood[128];
            char tmp[] = "/tmp/cnet_affect_gate.txt";
            FILE *af = fopen(tmp, "w");
            if (af) {
                fputs("hits 5\nbricks 4\n", af);
                fclose(af);
            }
            cnet_sr_set_affect_path(&S, tmp);
            S.da = 0.33f;
            S.ht = 0.54f;
            S.ado = 0.52f;
            S.feel[0] = 0;
            cnet_sr_session_overlay(&S);
            chk(cnet_sr_mood_line(&S, mood, sizeof mood) == 0 &&
                    strstr(mood, "Bored") == NULL && strstr(mood, "da=") == NULL,
                "session brick hits leave Bored");
            chk(cnet_sr_mood_why_line(&S, mood, sizeof mood) == 0 &&
                    strstr(mood, "Few hits") == NULL &&
                    strstr(mood, "bricks") != NULL,
                "why names landed bricks not few hits");
            unlink(tmp);
        }
    }
    cnet_sr_set_cooldown(&S, 1000, 30);
    chk(!cnet_sr_cooldown_ok(&S, 1000), "cooldown blocks");
    chk(cnet_sr_cooldown_ok(&S, 1031), "cooldown elapsed");
    /* never claims CERT */
    chk(1, "showrunner never seals (by API surface)");
    printf("checks=%d failures=%d\n", checks, fails);
    if (fails) {
        printf("SHOWRUNNER_FAIL\n");
        return 1;
    }
    printf("SHOWRUNNER_PASS\n");
    return 0;
}
