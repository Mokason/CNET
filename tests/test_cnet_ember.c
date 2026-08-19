#include "cnet_ember.h"
#include "cnet_held_model.h"
#include "cnet_rlm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_ok;

static void expect(int cond, const char *name) {
    if (cond) {
        printf("  ok   %s\n", name);
        g_ok++;
    } else {
        printf("  FAIL %s\n", name);
        g_fail++;
    }
}

static int hook_draft(const char *turn, char *out, size_t cap) {
    snprintf(out, cap, "ember-draft:%s", turn ? turn : "");
    return 0;
}

int main(void) {
    char home[] = "/tmp/cnet_ember_test_XXXXXX";
    char missp[] = "/tmp/cnet_ember_miss_XXXXXX";
    char steer_path[512];
    CnetEmber e;
    CnetEmberDraftReport dr;
    CnetEmberSyncResult sr;
    char out[1024];
    FILE *sf;

    printf("cnet_ember + rlm session tests\n");
    if (!mkdtemp(home)) return 2;
    if (!mkdtemp(missp)) return 2;
    {
        char missf[768];
        snprintf(missf, sizeof missf, "%s/miss.jsonl", missp);
        setenv("CNET_MISS_LOG", missf, 1);
        setenv("CNET_EMBER_HOME", home, 1);
    }

    /* SHA + ckpt store/load */
    {
        CnetEmberCkptStore ck;
        char sha[CNET_EMBER_SHA_HEX];
        char buf[256];
        size_t got = 0;
        char cdir[512];
        snprintf(cdir, sizeof cdir, "%s/ckpt", home);
        expect(cnet_ember_ckpt_open(&ck, cdir, 8) == 0, "ckpt_open");
        cnet_ember_sha1_hex("hello", 5, sha);
        expect(strlen(sha) == 40, "sha_len");
        {
            char longtx[128];
            memset(longtx, 'x', sizeof longtx - 1);
            longtx[sizeof longtx - 1] = '\0';
            memcpy(longtx, "hello world transcript pad ", 26);
            expect(cnet_ember_ckpt_store(&ck, longtx, strlen(longtx), 1,
                                         CNET_EMBER_CKPT_CONTINUED) == 0,
                   "ckpt_store");
            {
                char more[160];
                snprintf(more, sizeof more, "%s MORE", longtx);
                expect(cnet_ember_ckpt_find_prefix(&ck, more, strlen(more)) >= 0,
                       "ckpt_find_prefix");
                {
                    int idx = cnet_ember_ckpt_find_prefix(&ck, more, strlen(more));
                    expect(cnet_ember_ckpt_load(&ck, idx, buf, sizeof buf, &got) > 0,
                           "ckpt_load");
                    expect(strstr(buf, "hello world") != NULL, "ckpt_payload");
                }
            }
        }
        cnet_ember_ckpt_close(&ck);
    }

    /* Session sync extend vs rebuild */
    {
        CnetEmberSession s;
        CnetEmberCkptStore ck;
        char cdir[512];
        snprintf(cdir, sizeof cdir, "%s/ckpt2", home);
        cnet_ember_session_init(&s);
        expect(cnet_ember_ckpt_open(&ck, cdir, 8) == 0, "sess_ckpt_open");
        expect(cnet_ember_session_sync(&s, &ck, "U: hi\n", &sr) == 0, "sync_rebuild");
        expect(sr.kind == CNET_EMBER_SYNC_REBUILD, "sync_kind_rebuild");
        expect(cnet_ember_session_sync(&s, &ck, "U: hi\nA: yo\n", &sr) == 0,
               "sync_extend");
        expect(sr.kind == CNET_EMBER_SYNC_EXTEND, "sync_kind_extend");
        expect(s.tx_len > 0, "sync_tx");
        /* force compact */
        s.soft_compact_chars = 8;
        s.hard_compact_chars = 8;
        s.n_turns = 9;
        expect(cnet_ember_session_maybe_compact(&s, &ck, NULL, NULL, 1) == 1,
               "compact_force");
        expect(s.compacted_n >= 1, "compact_count");
        expect(strstr(s.summary, "not CERT") != NULL ||
                   strstr(s.tx, "not CERT") != NULL,
               "compact_not_cert");
        cnet_ember_session_free(&s);
        cnet_ember_ckpt_close(&ck);
    }

    /* Steer style card */
    {
        CnetEmberSteer st;
        char dst[2048];
        snprintf(steer_path, sizeof steer_path, "%s/steer.txt", home);
        sf = fopen(steer_path, "w");
        expect(sf != NULL, "steer_file");
        if (sf) {
            fputs("# comment\nBe terse and fail-closed.\n", sf);
            fclose(sf);
        }
        expect(cnet_ember_steer_load(&st, steer_path) == 0, "steer_load");
        expect(cnet_ember_steer_apply(&st, "hello", dst, sizeof dst) == 0,
               "steer_apply");
        expect(strstr(dst, "terse") != NULL, "steer_card_in_prompt");
        expect(strstr(dst, "never CERT") != NULL, "steer_law");
        expect(strstr(dst, "hello") != NULL, "steer_keeps_turn");
    }

    /* Ember draft via held hook — claimed_cert always 0 */
    cnet_held_model_set_hook(hook_draft);
    expect(cnet_ember_open(&e, home) == 0, "ember_open");
    expect(cnet_ember_draft(&e, "what color is sky", out, sizeof out, &dr) == 0,
           "ember_draft_rc");
    expect(dr.claimed_cert == 0, "ember_no_cert");
    expect(dr.drafted == 1, "ember_drafted");
    expect(strstr(out, "ember-draft:") != NULL, "ember_text");
    /* second turn extends session */
    expect(cnet_ember_draft(&e, "and grass", out, sizeof out, &dr) == 0,
           "ember_draft2");
    expect(e.sess.n_turns >= 2, "ember_turns");
    cnet_ember_close(&e);
    cnet_held_model_set_hook(NULL);

    /* RLM session multi-turn CERT recall */
    {
        CnetRlmPolicy pol;
        CnetRlmSession sess;
        CnetRlmResult r;
        cnet_rlm_policy_default(&pol);
        pol.core.allow_wiki = 0;
        pol.core.open_chat_enabled = 0;
        cnet_rlm_session_init(&sess);
        expect(cnet_rlm_ask_session("2 plus 3", &pol, &sess, &r) == 0,
               "rlm_sess_math");
        expect(r.final.claimed_cert == 1, "rlm_sess_cert");
        expect(sess.n >= 1, "rlm_sess_remembered");
        expect(cnet_rlm_ask_session("again", &pol, &sess, &r) == 0,
               "rlm_sess_again");
        expect(r.session_used == 1, "rlm_sess_used");
        expect(r.final.claimed_cert == 1, "rlm_sess_again_cert");
        expect(strstr(r.final.value, "5") != NULL ||
                   strstr(r.summary, "5") != NULL,
               "rlm_sess_again_value");
        /* Ember draft path via RLM opt-in + hook */
        cnet_held_model_set_hook(hook_draft);
        pol.allow_ember_draft = 1;
        expect(cnet_rlm_ask_session("write a tiny haiku about zz99", &pol, &sess,
                                    &r) == 1,
               "rlm_ember_rc");
        expect(r.final.claimed_cert == 0, "rlm_ember_no_cert");
        expect(r.ember_draft == 1 || strstr(r.final.skill, "ember") != NULL ||
                   strstr(r.summary, "ember-draft") != NULL ||
                   strstr(r.final.value, "ember-draft") != NULL,
               "rlm_ember_draft_flag");
        cnet_held_model_set_hook(NULL);
        cnet_rlm_session_clear(&sess);
    }

    /* Policy default keeps ember off */
    {
        CnetRlmPolicy d;
        cnet_rlm_policy_default(&d);
        expect(d.allow_ember_draft == 0, "rlm_ember_default_off");
    }

    if (g_fail == 0)
        printf("CNET_EMBER_PASS\n");
    else
        printf("CNET_EMBER_FAIL\n");
    printf("checks=%d fail=%d residual_never_cert=1 session_sync=1 compact=1 "
           "steer=1 rlm_session=1 ember_draft_opt_in=1 python=0 "
           "broader_claims=WITHHELD\n",
           g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
