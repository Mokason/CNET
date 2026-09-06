/* Residual FFI converter — complete typed ports → CERT-shaped propose.
 * Never admits. Empty SHOW refused. Floats are not a port.
 */
#include "../include/cnet_ffi_convert.h"
#include "../include/cnet_roe_gold_id.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void scopy(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    if (!s) {
        d[0] = 0;
        return;
    }
    for (i = 0; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static int mkdir_p(const char *path) {
    if (!path || !path[0]) return -1;
    if (mkdir(path, 0700) != 0) {
        /* exist is ok */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }
    return 0;
}

void cnet_ffi_init(CnetFfiConv *C) {
    if (!C) return;
    memset(C, 0, sizeof *C);
    scopy(C->reason, sizeof C->reason, "init");
}

int cnet_ffi_require(CnetFfiConv *C, const char *port) {
    if (!C || !port || !port[0]) return -1;
    C->complete = 0;
    C->converted = 0;
    if (!strcmp(port, "site")) C->need_site = 1;
    else if (!strcmp(port, "sent")) C->need_sent = 1;
    else if (!strcmp(port, "got")) C->need_got = 1;
    else if (!strcmp(port, "show")) C->need_show = 1;
    else {
        scopy(C->reason, sizeof C->reason, "unknown_port");
        return -1;
    }
    scopy(C->reason, sizeof C->reason, "require");
    return 0;
}

static int obs_ok(const CnetFfiConv *C, const CnetFfiObs *o) {
    if (!C || !o) return 0;
    if (C->need_show && !o->show[0]) return 0;
    if (C->need_site && !o->site[0]) return 0;
    if (C->need_sent && !o->sent[0]) return 0;
    if (C->need_got && !o->got[0]) return 0;
    if (C->need_show && o->sent[0] && !strstr(o->show, o->sent)) return 0;
    if (C->need_show && o->got[0] && !strstr(o->show, o->got)) return 0;
    return 1;
}

int cnet_ffi_note(CnetFfiConv *C, const CnetFfiObs *o) {
    int i;
    if (!C || !o) return -1;
    if (!o->show[0]) {
        scopy(C->reason, sizeof C->reason, "empty_show");
        return -1;
    }
    for (i = 0; i < C->n_obs; i++) {
        if (!strcmp(C->obs[i].site, o->site) && !strcmp(C->obs[i].sent, o->sent)) {
            C->obs[i] = *o;
            C->complete = 0;
            C->converted = 0;
            scopy(C->reason, sizeof C->reason, "updated_residual");
            return 0;
        }
    }
    if (C->n_obs >= CNET_FFI_MAX_OBS) {
        scopy(C->reason, sizeof C->reason, "obs_full");
        return -1;
    }
    C->obs[C->n_obs] = *o;
    C->n_obs++;
    C->complete = 0;
    C->converted = 0;
    scopy(C->reason, sizeof C->reason, "noted_residual");
    return 0;
}

int cnet_ffi_complete(CnetFfiConv *C) {
    int i;
    if (!C) return 0;
    if (C->n_obs < 1) {
        scopy(C->reason, sizeof C->reason, "no_obs");
        C->complete = 0;
        return 0;
    }
    if (!C->need_site && !C->need_sent && !C->need_got && !C->need_show) {
        scopy(C->reason, sizeof C->reason, "no_ports");
        C->complete = 0;
        return 0;
    }
    for (i = 0; i < C->n_obs; i++) {
        if (!obs_ok(C, &C->obs[i])) {
            scopy(C->reason, sizeof C->reason, "incomplete_obs");
            C->complete = 0;
            return 0;
        }
    }
    C->complete = 1;
    scopy(C->reason, sizeof C->reason, "complete");
    return 1;
}

int cnet_ffi_convert(CnetFfiConv *C, char *row, size_t cap) {
    const CnetFfiObs *o;
    if (!C || !row || cap < 8) return -1;
    row[0] = 0;
    if (!cnet_ffi_complete(C)) {
        scopy(C->reason, sizeof C->reason, "convert_incomplete");
        return -1;
    }
    o = &C->obs[C->n_obs - 1];
    if (snprintf(row, cap, "%s\t%s", o->sent, o->got) < 0 || !row[0]) {
        scopy(C->reason, sizeof C->reason, "convert_fmt");
        return -1;
    }
    C->converted = 1;
    C->admitted = 0;
    scopy(C->reason, sizeof C->reason, "converted_propose_only");
    return 0;
}

int cnet_ffi_propose(CnetFfiConv *C, const char *dir) {
    FILE *f;
    char path[512];
    char row[CNET_FFI_ROW];
    int n;
    if (!C || !dir || !dir[0]) return -1;
    if (cnet_ffi_convert(C, row, sizeof row) != 0) return -1;
    if (mkdir_p(dir) != 0) return -1;
    n = snprintf(path, sizeof path, "%s/PROPOSE.json", dir);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f,
            "{\n"
            "  \"kind\": \"ffi_convert\",\n"
            "  \"site\": \"%s\",\n"
            "  \"sent\": \"%s\",\n"
            "  \"got\": \"%s\",\n"
            "  \"row\": \"%s\",\n"
            "  \"auto_cert\": false,\n"
            "  \"status\": \"pending_verify\",\n"
            "  \"admitted\": false,\n"
            "  \"law\": \"propose_only_never_self_cert\"\n"
            "}\n",
            C->obs[C->n_obs - 1].site, C->obs[C->n_obs - 1].sent,
            C->obs[C->n_obs - 1].got, row);
    fclose(f);
    scopy(C->reason, sizeof C->reason, "proposed");
    return 0;
}

int cnet_ffi_write_gold(CnetFfiConv *C, const char *packs, char *path_out,
                        size_t cap) {
    char q[96], ans[128], gdir[ROE_PATHMAX], packsbuf[ROE_PATHMAX];
    char gpath[ROE_PATHMAX];
    const CnetFfiObs *o;
    FILE *f;
    size_t i;
    if (!C) return -1;
    if (!cnet_ffi_complete(C)) {
        scopy(C->reason, sizeof C->reason, "gold_incomplete");
        return -1;
    }
    o = &C->obs[C->n_obs - 1];
    if (!o->sent[0] || !o->got[0]) {
        scopy(C->reason, sizeof C->reason, "gold_empty");
        return -1;
    }
    if (!strncmp(o->got, "ABSTAIN", 7) || !strcmp(o->got, "-")) {
        scopy(C->reason, sizeof C->reason, "gold_refusal");
        return -1;
    }
    snprintf(q, sizeof q, "past tense of %s", o->sent);
    snprintf(ans, sizeof ans, "%s -> %s.", o->sent, o->got);
    if (!packs || !packs[0]) {
        roe_packs_root(packsbuf, sizeof packsbuf, ".");
        packs = packsbuf;
    }
    {
        size_t pl = strlen(packs);
        char h[17];
        if (pl + 6 >= sizeof gdir) {
            scopy(C->reason, sizeof C->reason, "gold_path");
            return -1;
        }
        memcpy(gdir, packs, pl);
        memcpy(gdir + pl, "/gold", 6);
        if (mkdir(gdir, 0755) != 0 && !roe_is_dir(gdir)) {
            if (mkdir(packs, 0755) != 0 && !roe_is_dir(packs)) {
                scopy(C->reason, sizeof C->reason, "gold_mkdir");
                return -1;
            }
            if (mkdir(gdir, 0755) != 0 && !roe_is_dir(gdir)) {
                scopy(C->reason, sizeof C->reason, "gold_mkdir");
                return -1;
            }
        }
        roe_q_hash16(q, h);
        if (pl + 6 + 16 + 4 >= sizeof gpath) {
            scopy(C->reason, sizeof C->reason, "gold_path");
            return -1;
        }
        memcpy(gpath, gdir, pl + 5);
        gpath[pl + 5] = '/';
        memcpy(gpath + pl + 6, h, 16);
        memcpy(gpath + pl + 22, ".txt", 5);
    }
    f = fopen(gpath, "w");
    if (!f) {
        scopy(C->reason, sizeof C->reason, "gold_write");
        return -1;
    }
    fputs(ans, f);
    fputc('\n', f);
    fclose(f);
    if (path_out && cap) {
        for (i = 0; gpath[i] && i + 1 < cap; i++) path_out[i] = gpath[i];
        path_out[i] = 0;
    }
    C->admitted = 0;
    scopy(C->reason, sizeof C->reason, "gold_pending");
    return 0;
}

int cnet_ffi_admit(CnetFfiConv *C) {
    if (C) {
        C->admitted = 0;
        scopy(C->reason, sizeof C->reason, "admit_refused");
    }
    return -1;
}

void cnet_ffi_init_ports(CnetFfiConv *C) {
    cnet_ffi_init(C);
    (void)cnet_ffi_require(C, "site");
    (void)cnet_ffi_require(C, "sent");
    (void)cnet_ffi_require(C, "got");
    (void)cnet_ffi_require(C, "show");
}

static int ffi_tok(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (!out || cap < 2) return -1;
    out[0] = 0;
    if (!in) return -1;
    while (*in && o + 1 < cap) {
        unsigned char c = (unsigned char)*in++;
        if (isalnum(c) || c == '.' || c == '_' || c == '-')
            out[o++] = (char)c;
        else
            break;
    }
    out[o] = 0;
    return out[0] ? 0 : -1;
}

static const char *skip_ws(const char *s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

int cnet_ffi_parse(const char *q, CnetFfiCmd *cmd, CnetFfiObs *o) {
    const char *p;
    char verb[16];
    int npos = 0;
    if (cmd) *cmd = CNET_FFI_CMD_NONE;
    if (o) memset(o, 0, sizeof *o);
    if (!q) return 0;
    p = skip_ws(q);
    if (strncmp(p, "ffi", 3) != 0) return 0;
    p += 3;
    if (*p && *p != ' ' && *p != '\t') return 0;
    p = skip_ws(p);
    verb[0] = 0;
    {
        size_t i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && i + 1 < sizeof verb) {
            verb[i] = (char)tolower((unsigned char)p[i]);
            i++;
        }
        verb[i] = 0;
        p += i;
    }
    if (!strcmp(verb, "note")) {
        if (cmd) *cmd = CNET_FFI_CMD_NOTE;
    } else if (!strcmp(verb, "status")) {
        if (cmd) *cmd = CNET_FFI_CMD_STATUS;
        return 1;
    } else if (!strcmp(verb, "propose")) {
        if (cmd) *cmd = CNET_FFI_CMD_PROPOSE;
        return 1;
    } else if (!strcmp(verb, "reset")) {
        if (cmd) *cmd = CNET_FFI_CMD_RESET;
        return 1;
    } else if (!strcmp(verb, "gold")) {
        if (cmd) *cmd = CNET_FFI_CMD_GOLD;
        return 1;
    } else {
        return 0;
    }
    if (!o) return 1;
    p = skip_ws(p);
    while (p && *p) {
        const char *eq = NULL;
        const char *t = p;
        while (*t && *t != ' ' && *t != '\t' && *t != '=') t++;
        if (*t == '=') eq = t;
        if (eq) {
            char key[8], val[CNET_FFI_SITE];
            size_t klen = (size_t)(eq - p);
            if (klen >= sizeof key) klen = sizeof key - 1;
            memcpy(key, p, klen);
            key[klen] = 0;
            p = eq + 1;
            if (ffi_tok(p, val, sizeof val) != 0) break;
            if (!strcmp(key, "site")) scopy(o->site, sizeof o->site, val);
            else if (!strcmp(key, "sent")) scopy(o->sent, sizeof o->sent, val);
            else if (!strcmp(key, "got")) scopy(o->got, sizeof o->got, val);
            while (*p && *p != ' ' && *p != '\t') p++;
        } else {
            char val[CNET_FFI_SITE];
            if (ffi_tok(p, val, sizeof val) != 0) break;
            if (npos == 0) scopy(o->site, sizeof o->site, val);
            else if (npos == 1) scopy(o->sent, sizeof o->sent, val);
            else if (npos == 2) scopy(o->got, sizeof o->got, val);
            npos++;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        p = skip_ws(p);
    }
    if (o->site[0] || o->sent[0]) {
        snprintf(o->show, sizeof o->show, "FFI site=%s sent=%s got=%s%s",
                 o->site[0] ? o->site : "-", o->sent[0] ? o->sent : "-",
                 o->got[0] ? o->got : "-", o->got[0] ? "" : " GAP");
    }
    return 1;
}

int cnet_ffi_status_line(const CnetFfiConv *C, char *out, size_t cap) {
    if (!C || !out || cap < 8) return -1;
    snprintf(out, cap,
             "ffi n_obs=%d complete=%d converted=%d admitted=0 reason=%s",
             C->n_obs, C->complete, C->converted,
             C->reason[0] ? C->reason : "-");
    return 0;
}

int cnet_ffi_selftest(void) {
    CnetFfiConv C;
    CnetFfiObs o;
    char row[CNET_FFI_ROW];
    int fail = 0;
    memset(row, 0, sizeof row);

#define Tst(ok, msg)                                                           \
    do {                                                                       \
        printf("  %-56s %s\n", msg, (ok) ? "PASS" : "FAIL");                   \
        if (!(ok)) fail++;                                                     \
    } while (0)

    printf("=== residual FFI converter (complete→propose, never admit) ===\n");
    cnet_ffi_init(&C);
    Tst(cnet_ffi_require(&C, "site") == 0 && cnet_ffi_require(&C, "sent") == 0 &&
            cnet_ffi_require(&C, "got") == 0 && cnet_ffi_require(&C, "show") == 0,
        "require four ports");
    Tst(cnet_ffi_complete(&C) == 0, "empty is incomplete");
    Tst(cnet_ffi_convert(&C, row, sizeof row) != 0, "incomplete convert refused");
    Tst(cnet_ffi_admit(&C) != 0 && C.admitted == 0, "admit always refused");

    memset(&o, 0, sizeof o);
    snprintf(o.site, sizeof o.site, "host.embed.0");
    snprintf(o.sent, sizeof o.sent, "go");
    snprintf(o.show, sizeof o.show, "FFI site=host.embed.0 sent=go GAP");
    Tst(cnet_ffi_note(&C, &o) == 0, "note partial residual");
    Tst(cnet_ffi_complete(&C) == 0, "missing got stays incomplete");
    Tst(cnet_ffi_convert(&C, row, sizeof row) != 0, "partial convert refused");

    snprintf(o.got, sizeof o.got, "went");
    snprintf(o.show, sizeof o.show, "FFI site=host.embed.0 sent=go got=went");
    Tst(cnet_ffi_note(&C, &o) == 0, "note complete residual");
    Tst(cnet_ffi_complete(&C) == 1, "all ports filled → complete");
    Tst(cnet_ffi_convert(&C, row, sizeof row) == 0, "complete convert ok");
    Tst(strstr(row, "go") && strstr(row, "went"), "row has lemma and form");
    Tst(strstr(row, "\t") != NULL, "row is discrete TSV");
    Tst(C.converted == 1 && C.admitted == 0, "converted is not admitted");
    Tst(cnet_ffi_admit(&C) != 0, "still cannot admit after convert");

    {
        char dir[] = "/tmp/cnet_ffi_propose_test";
        FILE *f;
        char buf[512];
        Tst(cnet_ffi_propose(&C, dir) == 0, "propose pending dir");
        f = fopen("/tmp/cnet_ffi_propose_test/PROPOSE.json", "r");
        Tst(f != NULL, "PROPOSE.json written");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            Tst(strstr(buf, "auto_cert") && strstr(buf, "false"),
                "auto_cert false");
            Tst(strstr(buf, "pending_verify") != NULL, "status pending_verify");
            Tst(strstr(buf, "admitted") == NULL || strstr(buf, "\"admitted\": false"),
                "propose does not claim admit");
        }
    }

    {
        CnetFfiObs bad;
        memset(&bad, 0, sizeof bad);
        snprintf(bad.site, sizeof bad.site, "host.embed.0");
        snprintf(bad.sent, sizeof bad.sent, "see");
        snprintf(bad.got, sizeof bad.got, "saw");
        cnet_ffi_init(&C);
        (void)cnet_ffi_require(&C, "site");
        (void)cnet_ffi_require(&C, "sent");
        (void)cnet_ffi_require(&C, "got");
        (void)cnet_ffi_require(&C, "show");
        Tst(cnet_ffi_note(&C, &bad) != 0, "empty SHOW note refused");
    }

    {
        CnetFfiCmd cmd = CNET_FFI_CMD_NONE;
        CnetFfiObs po;
        Tst(cnet_ffi_parse("who are you", &cmd, &po) == 0 &&
                cmd == CNET_FFI_CMD_NONE,
            "who are you is not ffi");
        Tst(cnet_ffi_parse("ffi status", &cmd, &po) == 1 &&
                cmd == CNET_FFI_CMD_STATUS,
            "ffi status");
        Tst(cnet_ffi_parse("ffi propose", &cmd, &po) == 1 &&
                cmd == CNET_FFI_CMD_PROPOSE,
            "ffi propose");
        Tst(cnet_ffi_parse("ffi reset", &cmd, &po) == 1 && cmd == CNET_FFI_CMD_RESET,
            "ffi reset");
        Tst(cnet_ffi_parse("ffi note site=host.embed.0 sent=go got=went", &cmd,
                           &po) == 1 &&
                cmd == CNET_FFI_CMD_NOTE && !strcmp(po.sent, "go") &&
                !strcmp(po.got, "went") && strstr(po.show, "went"),
            "ffi note kv");
        Tst(cnet_ffi_parse("ffi note host.embed.0 go went", &cmd, &po) == 1 &&
                !strcmp(po.site, "host.embed.0") && !strcmp(po.sent, "go") &&
                !strcmp(po.got, "went"),
            "ffi note positional");
        cnet_ffi_init_ports(&C);
        Tst(cnet_ffi_parse("ffi note site=host.embed.0 sent=go got=went", &cmd,
                           &po) == 1 &&
                cnet_ffi_note(&C, &po) == 0 && cnet_ffi_complete(&C) == 1,
            "parse+note completes");
        Tst(cnet_ffi_parse("ffi gold", &cmd, &po) == 1 && cmd == CNET_FFI_CMD_GOLD,
            "ffi gold parse");
        {
            char gp[512], expect[512], h[17];
            Tst(cnet_ffi_write_gold(&C, "/tmp/cnet_ffi_gold_packs", gp,
                                    sizeof gp) == 0,
                "write gold from complete");
            roe_q_hash16("past tense of go", h);
            snprintf(expect, sizeof expect,
                     "/tmp/cnet_ffi_gold_packs/gold/%s.txt", h);
            Tst(!strcmp(gp, expect), "gold path uses roe hash");
            {
                FILE *gf = fopen(gp, "r");
                char buf[128];
                size_t n = 0;
                Tst(gf != NULL, "gold file exists");
                if (gf) {
                    n = fread(buf, 1, sizeof buf - 1, gf);
                    buf[n] = 0;
                    fclose(gf);
                    Tst(strstr(buf, "go -> went") != NULL, "gold text lemma form");
                }
            }
            Tst(C.admitted == 0, "gold write is not admit");
        }
    }

#undef Tst
    printf("\nfailures=%d\n", fail);
    if (fail) {
        printf("FFI_CONVERT_FAIL\n");
        return 1;
    }
    printf("FFI_CONVERT_PASS\n");
    return 0;
}
