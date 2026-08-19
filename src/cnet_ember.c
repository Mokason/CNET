#include "cnet_ember.h"

#include "cnet_held_model.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void cnet_ember_default_paths(char *ckpt_dir, size_t ckpt_cap, char *steer_path,
                              size_t steer_cap) {
    const char *home = getenv("CNET_EMBER_HOME");
    const char *xd = getenv("XDG_STATE_HOME");
    char base[512];
    if (home && home[0])
        snprintf(base, sizeof base, "%s", home);
    else if (xd && xd[0])
        snprintf(base, sizeof base, "%s/cnet-ember", xd);
    else {
        const char *h = getenv("HOME");
        snprintf(base, sizeof base, "%s/.local/state/cnet-ember", h ? h : "/tmp");
    }
    if (ckpt_dir && ckpt_cap)
        snprintf(ckpt_dir, ckpt_cap, "%s/ckpt", base);
    if (steer_path && steer_cap)
        snprintf(steer_path, steer_cap, "%s/steer.txt", base);
}

static int mkdir_p(const char *path) {
    char tmp[512];
    size_t len;
    size_t i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len == 0) return -1;
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            (void)mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

int cnet_ember_open(CnetEmber *e, const char *home_dir) {
    char ckpt[512], steer[512], home[512];
    const char *sp;
    if (!e) return -1;
    memset(e, 0, sizeof *e);
    cnet_ember_session_init(&e->sess);
    cnet_ember_steer_init(&e->steer);
    if (home_dir && home_dir[0])
        snprintf(home, sizeof home, "%s", home_dir);
    else {
        cnet_ember_default_paths(ckpt, sizeof ckpt, steer, sizeof steer);
        /* derive home from ckpt parent */
        snprintf(home, sizeof home, "%s", ckpt);
        {
            char *slash = strrchr(home, '/');
            if (slash) *slash = '\0';
        }
    }
    snprintf(ckpt, sizeof ckpt, "%.400s/ckpt", home);
    snprintf(steer, sizeof steer, "%.400s/steer.txt", home);
    mkdir_p(home);
    mkdir_p(ckpt);
    if (cnet_ember_ckpt_open(&e->ckpt, ckpt, 64) != 0) return -1;
    sp = getenv("CNET_EMBER_STEER");
    if (sp && sp[0])
        (void)cnet_ember_steer_load(&e->steer, sp);
    else
        (void)cnet_ember_steer_load(&e->steer, steer);
    e->open = 1;
    return 0;
}

void cnet_ember_close(CnetEmber *e) {
    if (!e) return;
    if (e->open && e->sess.tx_len > 0)
        (void)cnet_ember_session_checkpoint(&e->sess, &e->ckpt,
                                            CNET_EMBER_CKPT_SHUTDOWN);
    cnet_ember_session_free(&e->sess);
    cnet_ember_ckpt_close(&e->ckpt);
    cnet_ember_steer_clear(&e->steer);
    memset(e, 0, sizeof *e);
}

int cnet_ember_note_miss(const char *reason, const char *q) {
    const char *p = getenv("CNET_MISS_LOG");
    FILE *f;
    char qesc[96];
    size_t i, j = 0;
    if (!p || !p[0]) return 1;
    f = fopen(p, "a");
    if (!f) return -1;
    qesc[0] = '\0';
    if (q) {
        for (i = 0; q[i] && j + 1 < sizeof qesc; ++i) {
            char c = q[i];
            if (c == '"' || c == '\\' || c == '\n' || c == '\r') c = ' ';
            qesc[j++] = c;
        }
        qesc[j] = '\0';
    }
    fprintf(f,
            "{\"via\":\"cnet_ember\",\"reason\":\"%s\",\"q\":\"%s\","
            "\"claimed_cert\":0,\"auto_cert\":false}\n",
            reason ? reason : "miss", qesc);
    fclose(f);
    return 0;
}

int cnet_ember_draft(CnetEmber *e, const char *user_turn, char *out, size_t cap,
                     CnetEmberDraftReport *rep) {
    CnetEmberSyncResult sr;
    CnetEmberDraftReport local;
    char steered[CNET_EMBER_TURN_MAX + CNET_EMBER_STEER_CARD + 128];
    char prompt[CNET_EMBER_TX_MAX];
    int rc;
    if (rep) memset(rep, 0, sizeof *rep);
    else rep = &local;
    rep->claimed_cert = 0;
    if (out && cap) out[0] = '\0';
    if (!e || !e->open || !user_turn || !out || cap == 0) return -1;

    /* Build full prompt = transcript + new user line for sync */
    if (e->sess.tx_len > 0)
        snprintf(prompt, sizeof prompt, "%sU: %s\n", e->sess.tx, user_turn);
    else
        snprintf(prompt, sizeof prompt, "U: %s\n", user_turn);

    if (cnet_ember_session_sync(&e->sess, &e->ckpt, prompt, &sr) < 0) return -1;
    rep->from_ckpt = (sr.kind == CNET_EMBER_SYNC_LOAD_CKPT);

    rc = cnet_ember_session_maybe_compact(&e->sess, &e->ckpt, NULL, NULL, 0);
    if (rc < 0) return -1;
    if (rc > 0) {
        rep->compacted = 1;
        snprintf(rep->reason, sizeof rep->reason, "compacted");
    }

    if (cnet_ember_steer_apply(&e->steer, user_turn, steered, sizeof steered) != 0)
        snprintf(steered, sizeof steered, "%s", user_turn);
    rep->steered = e->steer.loaded ? 1 : 0;

    /* Residual draft via held_model — never CERT */
    if (cnet_held_model_ask(steered, out, cap) != 0 || out[0] == '\0') {
        snprintf(rep->reason, sizeof rep->reason, "held_abstain");
        (void)cnet_ember_note_miss("held_abstain", user_turn);
        return 1;
    }
    rep->drafted = 1;
    rep->claimed_cert = 0;
    (void)cnet_ember_session_append_pair(&e->sess, user_turn, out);
    (void)cnet_ember_session_checkpoint(&e->sess, &e->ckpt,
                                        CNET_EMBER_CKPT_CONTINUED);
    snprintf(rep->reason, sizeof rep->reason, "draft_ok");
    return 0;
}
