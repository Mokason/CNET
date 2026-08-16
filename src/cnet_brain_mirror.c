#include "cnet_brain_mirror.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

static char g_override[CNET_BRAIN_MIRROR_PATH];

void cnet_brain_mirror_set_dir(const char *dir) {
    if (dir == NULL || dir[0] == '\0') {
        g_override[0] = '\0';
        return;
    }
    snprintf(g_override, sizeof g_override, "%s", dir);
}

static int mkdirs_p(const char *path) {
    char tmp[CNET_BRAIN_MIRROR_PATH];
    size_t len, i;
    if (path == NULL || path[0] == '\0') return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len == 0 || len >= sizeof tmp) return -1;
    for (i = 1; i < len; ++i) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int cnet_brain_mirror_dir(char *buf, size_t cap) {
    const char *e;
    if (buf == NULL || cap < 8) return -1;
    if (g_override[0]) {
        snprintf(buf, cap, "%s", g_override);
        return 0;
    }
    e = getenv("CNET_BRAIN_MIRROR_DIR");
    if (e && e[0]) {
        snprintf(buf, cap, "%s", e);
        return 0;
    }
    e = getenv("HERMES_HOME");
    if (e && e[0]) {
        snprintf(buf, cap, "%s/brain_mirror", e);
        return 0;
    }
    e = getenv("HOME");
    if (e && e[0]) {
        snprintf(buf, cap, "%s/.hermes/brain_mirror", e);
        return 0;
    }
    snprintf(buf, cap, "/tmp/cnet_brain_mirror");
    return 0;
}

static void json_esc(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    if (dst == NULL || cap == 0) return;
    dst[0] = '\0';
    if (src == NULL) return;
    for (; *src && o + 2 < cap; ++src) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 3 >= cap) break;
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c < 32) {
            continue;
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

int cnet_brain_mirror_core(const CnetHemiResult *r) {
    char dir[CNET_BRAIN_MIRROR_PATH];
    char path[CNET_BRAIN_MIRROR_PATH + 32];
    char sk[160], val[320], sp[1600];
    FILE *f;
    time_t now;

    if (r == NULL) return -1;
    /* Only CERT plane certified binds. Open chat never mirrors. */
    if (r->plane == CNET_CORE_PLANE_OPEN_CHAT || r->open_chat) return 1;
    if (r->hemi != CNET_HEMI_CORE || !r->bound || !r->claimed_cert) return 1;
    if (r->source == CNET_HEMI_SRC_HELD_LLM) return 1;

    if (cnet_brain_mirror_dir(dir, sizeof dir) != 0) return -1;
    if (mkdirs_p(dir) != 0) return -1;
    snprintf(path, sizeof path, "%s/core_admit.jsonl", dir);

    json_esc(r->skill, sk, sizeof sk);
    json_esc(r->value, val, sizeof val);
    json_esc(r->spoken, sp, sizeof sp);
    now = time(NULL);

    f = fopen(path, "a");
    if (f == NULL) return -1;
    fprintf(f,
            "{\"ts\":%lld,\"hemi\":\"CORE\",\"source\":\"%s\",\"skill\":\"%s\","
            "\"value\":\"%s\",\"spoken\":\"%s\",\"claimed_cert\":1,"
            "\"may_voice\":%d,\"brain_action\":\"admit_candidate\","
            "\"law\":\"mirror_not_self_cert\"}\n",
            (long long)now, cnet_hemi_source_name(r->source), sk, val, sp,
            r->may_voice ? 1 : 0);
    fclose(f);

    /* Sidecar cand for Brain inbox consumers */
    {
        char inbox[CNET_BRAIN_MIRROR_PATH + 16];
        char cand[CNET_BRAIN_MIRROR_PATH + 128];
        char skill_tag[48];
        size_t i;
        FILE *cf;
        snprintf(inbox, sizeof inbox, "%s/inbox", dir);
        if (mkdirs_p(inbox) == 0) {
            snprintf(skill_tag, sizeof skill_tag, "%.40s", sk[0] ? sk : "core");
            for (i = 0; skill_tag[i]; ++i)
                if (skill_tag[i] == '/' || skill_tag[i] == ' ' ||
                    skill_tag[i] == '"')
                    skill_tag[i] = '_';
            if ((size_t)snprintf(cand, sizeof cand, "%s/cand_%lld_%s.json", inbox,
                                 (long long)now, skill_tag) >= sizeof cand)
                return 0; /* jsonl already written; skip oversized cand path */
            cf = fopen(cand, "w");
            if (cf) {
                fprintf(cf,
                        "{\"hemi\":\"CORE\",\"skill\":\"%s\",\"value\":\"%s\","
                        "\"spoken\":\"%s\",\"claimed_cert\":1,"
                        "\"admit\":\"pending_brain_verify\"}\n",
                        sk, val, sp);
                fclose(cf);
            }
        }
    }
    return 0;
}
