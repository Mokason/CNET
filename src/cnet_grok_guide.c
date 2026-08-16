/* Grok guide during evolve — residual advisory only. Never CERT. */
#include "cnet_grok_guide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static const char *api_key(void) {
    const char *k = getenv("XAI_API_KEY");
    if (k && k[0]) return k;
    k = getenv("XAI_KEY");
    if (k && k[0]) return k;
    k = getenv("GROK_API_KEY");
    if (k && k[0]) return k;
    return NULL;
}

static void json_escape_file(FILE *f, const char *in) {
    while (*in) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c == '\n') {
            fputs("\\n", f);
        } else if (c >= 32) {
            fputc(c, f);
        }
    }
}

/* Ask Grok via curl CLI; wall clock enforced by alarm-ish wait with timeout. */
static int grok_ask(const char *question, int seconds_limit, char *answer,
                    size_t acap, int *timed_out) {
    const char *key = api_key();
    const char *model = getenv("CNET_GROK_MODEL");
    char body_path[] = "/tmp/cnet_grok_body_XXXXXX";
    char out_path[] = "/tmp/cnet_grok_out_XXXXXX";
    char hdr_path[] = "/tmp/cnet_grok_hdr_XXXXXX";
    int bfd, ofd, hfd;
    FILE *bf;
    double t0;
    FILE *of;
    char *json = NULL;
    long sz = 0;
    if (timed_out) *timed_out = 0;
    if (!key) return -2;
    if (!model || !model[0]) model = "grok-3";
    if (seconds_limit < 30) seconds_limit = 30;
    if (seconds_limit > 1800) seconds_limit = 1800;
    bfd = mkstemp(body_path);
    ofd = mkstemp(out_path);
    hfd = mkstemp(hdr_path);
    if (bfd < 0 || ofd < 0 || hfd < 0) return -1;
    close(ofd);
    close(hfd);
    bf = fdopen(bfd, "w");
    if (!bf) return -1;
    fprintf(bf, "{\"model\":\"%s\",\"temperature\":0.2,\"max_tokens\":1200,", model);
    fprintf(bf, "\"messages\":[{\"role\":\"system\",\"content\":\"");
    fprintf(bf,
            "You are a brief technical guide for CNET CORE evolve. "
            "Suggest structured next steps only. Never claim CERT. "
            "Prefer teach TAG n m or pairs= tables when relevant. "
            "Keep under 400 words.");
    fprintf(bf, "\"},{\"role\":\"user\",\"content\":\"");
    json_escape_file(bf, question);
    fprintf(bf, "\"}]}");
    fclose(bf);
    {
        FILE *hf = fopen(hdr_path, "w");
        if (hf) {
            fprintf(hf, "Content-Type: application/json\nAuthorization: Bearer %s\n",
                    key);
            fclose(hf);
        }
    }
    t0 = now_s();
    {
        char cmd[2048];
        int ht = seconds_limit > 120 ? 120 : seconds_limit;
        /* key via env avoids shell-metachar issues in Authorization header */
        setenv("CNET_GROK_TMP_KEY", key, 1);
        snprintf(cmd, sizeof cmd,
                 "curl -sS --max-time %d "
                 "-H 'Content-Type: application/json' "
                 "-H \"Authorization: Bearer ${CNET_GROK_TMP_KEY}\" "
                 "-d @%s https://api.x.ai/v1/chat/completions > %s 2>/dev/null",
                 ht, body_path, out_path);
        { int rc_sys = system(cmd); (void)rc_sys; }
        unsetenv("CNET_GROK_TMP_KEY");
    }

    if (now_s() - t0 >= (double)seconds_limit) {
        if (timed_out) *timed_out = 1;
        unlink(body_path);
        unlink(out_path);
        unlink(hdr_path);
        return -3;
    }
    of = fopen(out_path, "r");
    if (!of) {
        unlink(body_path);
        unlink(out_path);
        unlink(hdr_path);
        return -4;
    }
    fseek(of, 0, SEEK_END);
    sz = ftell(of);
    fseek(of, 0, SEEK_SET);
    if (sz <= 0 || sz > 2000000) {
        fclose(of);
        unlink(body_path);
        unlink(out_path);
        unlink(hdr_path);
        return -4;
    }
    json = (char *)malloc((size_t)sz + 1);
    if (!json) {
        fclose(of);
        return -1;
    }
    if (fread(json, 1, (size_t)sz, of) != (size_t)sz) {
        free(json);
        fclose(of);
        return -4;
    }
    json[sz] = 0;
    fclose(of);
    unlink(body_path);
    unlink(out_path);
    unlink(hdr_path);
    {
        const char *p = strstr(json, "\"content\":");
        if (!p) {
            free(json);
            return -5;
        }
        p = strchr(p + 10, '"');
        if (!p) {
            free(json);
            return -5;
        }
        p++;
        {
            size_t o = 0;
            while (*p && o + 1 < acap) {
                if (*p == '\\' && p[1]) {
                    p++;
                    if (*p == 'n')
                        answer[o++] = '\n';
                    else if (*p == '"' || *p == '\\')
                        answer[o++] = *p;
                    else
                        answer[o++] = *p;
                    p++;
                    continue;
                }
                if (*p == '"') break;
                answer[o++] = *p++;
            }
            answer[o] = 0;
        }
    }
    free(json);
    return answer[0] ? 0 : -5;
}

int cnet_grok_guide_run(const char *bricks_dir, int max_q, int seconds_per_q,
                        CnetGrokGuideReport *rep) {
    char qpath[768], lpath[768], line[1024];
    FILE *qf, *lf;
    int n = 0;
    if (!rep) return -1;
    memset(rep, 0, sizeof *rep);
    if (!bricks_dir || !bricks_dir[0]) return -1;
    {
        const char *en = getenv("CNET_GROK_GUIDE");
        if (!en || en[0] != '1') {
            rep->enabled = 0;
            return 1;
        }
    }
    rep->enabled = 1;
    if (max_q <= 0) {
        const char *mq = getenv("CNET_GROK_GUIDE_MAX_Q");
        max_q = mq && mq[0] ? atoi(mq) : 3;
    }
    if (max_q > 8) max_q = 8;
    if (seconds_per_q <= 0) {
        const char *s = getenv("CNET_GROK_GUIDE_SECONDS");
        seconds_per_q = s && s[0] ? atoi(s) : 1800;
    }
    if (seconds_per_q > 1800) seconds_per_q = 1800; /* 30 min hard cap */
    rep->seconds_limit = (double)seconds_per_q;
    snprintf(qpath, sizeof qpath, "%s/guide_questions.txt", bricks_dir);
    snprintf(lpath, sizeof lpath, "%s/guide_log.md", bricks_dir);
    snprintf(rep->log_path, sizeof rep->log_path, "%.500s", lpath);
    qf = fopen(qpath, "r");
    if (!qf) return 1;
    lf = fopen(lpath, "a");
    if (!lf) {
        fclose(qf);
        return -1;
    }
    fprintf(lf, "\n## Guide session %ld (max %ds/q, Grok residual — not CERT)\n\n",
            (long)time(NULL), seconds_per_q);
    while (fgets(line, sizeof line, qf) && n < max_q) {
        char *nl = strchr(line, '\n');
        char answer[8000];
        int to = 0, rc;
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        rep->asked++;
        n++;
        fprintf(lf, "### Q%d\n\n%s\n\n", n, line);
        rc = grok_ask(line, seconds_per_q, answer, sizeof answer, &to);
        if (to) {
            rep->timed_out++;
            fprintf(lf, "_timed out after %ds_\n\n", seconds_per_q);
        } else if (rc != 0) {
            rep->errors++;
            fprintf(lf, "_guide error rc=%d (no CERT impact)_\n\n", rc);
        } else {
            rep->answered++;
            fprintf(lf, "**Guide (Grok, non-CERT):**\n\n%s\n\n", answer);
        }
        fflush(lf);
    }
    fclose(qf);
    fclose(lf);
    qf = fopen(qpath, "w");
    if (qf) {
        fprintf(qf, "# processed — add new guide questions next time\n");
        fclose(qf);
    }
    return 0;
}
