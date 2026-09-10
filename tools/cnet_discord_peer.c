/* cnet_discord_peer — Discord <-> Marble PEER bridge (pure C).
 *
 *   cnet_discord_peer                 dry-run / skip when no token
 *   cnet_discord_peer --once "u: hi"  inject one line through cnet_peer
 *   cnet_discord_peer --live          poll allowlisted channels and reply
 *   cnet_discord_peer --selftest      offline unit checks
 *
 * Env:
 *   CNET_DISCORD_TOKEN     bot token. Secret: env only, never a pack, never logged.
 *   CNET_DISCORD_CHANNELS  comma-separated channel id allowlist (REQUIRED for --live)
 *   CNET_DISCORD_POLL_SEC  poll interval, default 5
 *   CNET_DISCORD_MAX_LOOPS stop after N polls (0 = forever); for tests
 *   CNET_DISCORD_PREFIX    only handle messages starting with this (default none)
 *
 * Transport is the Discord REST API over `curl` (no websocket stack, no extra
 * link deps). Polling is enough for a peer bridge and keeps the bridge a small
 * auditable program.
 *
 * Law:
 *   - Discord is a PEER, not a control plane: every message goes in as
 *     `PEER discord <user>: <text>` through the same front door as Hermes.
 *   - The allowlist is mandatory and fail-closed: an empty or unparseable
 *     CNET_DISCORD_CHANNELS means zero channels, never "all".
 *   - Marble replies with whatever cnetd said. A miss stays a miss; a stage
 *     draft is labelled as a draft. The bridge never upgrades an answer.
 *   - Chat text reaches argv only after shell-quoting; the token never does.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DP_URL  512
#define DP_ID   32
#define DP_TEXT 2048
#define DP_BUF  65536
#define DP_MAXCH 16

static int g_verbose;

/* ------------------------------------------------------------- utilities */
static int sh_quote(const char *in, char *out, size_t cap) {
    size_t o = 0, i;
    if (!in || cap < 3) return -1;
    out[o++] = '\'';
    for (i = 0; in[i]; i++) {
        if (in[i] == '\'') {
            if (o + 4 >= cap) return -1;
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else {
            if (o + 2 >= cap) return -1;
            out[o++] = in[i];
        }
    }
    out[o++] = '\'';
    out[o] = 0;
    return 0;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0, i;
    for (i = 0; in && in[i] && o + 8 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) { o += (size_t)snprintf(out + o, 8, "\\u%04x", c); }
            else out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

/* first "key":"value" inside `obj`, escapes decoded */
static int jstr(const char *obj, const char *key, char *out, size_t cap) {
    char pat[64];
    const char *p;
    size_t o = 0;
    out[0] = 0;
    if (!obj) return -1;
    if ((size_t)snprintf(pat, sizeof pat, "\"%s\":", key) >= sizeof pat) return -1;
    p = strstr(obj, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"' && o + 2 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': case 'r': case 't': out[o++] = ' '; break;
            case 'u':
                if (isxdigit((unsigned char)p[1])) { p += 4; out[o++] = ' '; }
                else out[o++] = 'u';
                break;
            default: out[o++] = *p; break;
            }
            p++;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    return 0;
}

static int jtrue(const char *obj, const char *key) {
    char pat[64];
    const char *p;
    if (!obj) return 0;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(obj, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
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

/* ------------------------------------------------------- channel allowlist */
/* Fail closed: only ids made of digits, and only ones actually listed. */
static int parse_channels(const char *csv, char ch[][DP_ID], int maxch) {
    int n = 0;
    const char *p = csv;
    if (!csv) return 0;
    while (*p && n < maxch) {
        size_t o = 0;
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        while (*p && *p != ',' && o + 1 < DP_ID) {
            if (!isdigit((unsigned char)*p)) { o = 0; break; }  /* reject junk id */
            ch[n][o++] = *p++;
        }
        while (*p && *p != ',') p++;   /* skip the remainder of a rejected id */
        if (o) { ch[n][o] = 0; n++; }
    }
    return n;
}

static int channel_allowed(const char *id, char ch[][DP_ID], int n) {
    int i;
    if (!id || !id[0]) return 0;
    for (i = 0; i < n; i++)
        if (!strcmp(ch[i], id)) return 1;
    return 0;
}

/* ------------------------------------------------------------- peer bridge */
/* Send one Discord message into cnetd as a PEER turn, return the ANSWER
 * (and STAGE line if the daemon produced a draft). */
static int ask_marble(const char *user, const char *text, char *out, size_t cap) {
    char line[DP_TEXT], q[DP_TEXT * 4], cmd[DP_TEXT * 5], resp[DP_BUF];
    char peer[80];
    const char *a, *st;
    size_t o = 0;
    snprintf(peer, sizeof peer, "discord_%.40s", user && user[0] ? user : "user");
    snprintf(line, sizeof line, "%.1800s", text ? text : "");
    if (sh_quote(line, q, sizeof q) != 0) return -1;
    if ((size_t)snprintf(cmd, sizeof cmd, "cnet_peer --peer %s %s 2>&1", peer, q) >= sizeof cmd)
        return -1;
    if (run(cmd, resp, sizeof resp) != 0 && !resp[0]) return -1;

    a = strstr(resp, "\nANSWER ");
    if (!a && strncmp(resp, "ANSWER ", 7) == 0) a = resp - 1;
    if (!a) return -1;
    a += 8;
    while (*a && *a != '\n' && o + 2 < cap) out[o++] = *a++;
    out[o] = 0;
    /* A stage draft is labelled, never silently merged into the answer. */
    st = strstr(resp, "\nSTAGE ");
    if (st && o + 32 < cap) {
        st += 7;
        o += (size_t)snprintf(out + o, cap - o, "\n_(stage draft, not certified)_ ");
        while (*st && *st != '\n' && o + 2 < cap) out[o++] = *st++;
        out[o] = 0;
    }
    return 0;
}

static int discord_post(const char *token, const char *chan, const char *text) {
    char esc[DP_TEXT * 2], body[DP_TEXT * 3], bq[DP_TEXT * 4];
    char url[DP_URL], cmd[DP_TEXT * 6], resp[DP_BUF];
    json_escape(text, esc, sizeof esc);
    snprintf(body, sizeof body, "{\"content\":\"%.1900s\"}", esc);
    if (sh_quote(body, bq, sizeof bq) != 0) return -1;
    snprintf(url, sizeof url, "https://discord.com/api/v10/channels/%s/messages", chan);
    /* token goes in a header via curl -H; it is never echoed */
    if ((size_t)snprintf(cmd, sizeof cmd,
                         "curl -sS --max-time 20 -X POST "
                         "-H 'Authorization: Bot %s' -H 'Content-Type: application/json' "
                         "-d %s '%s' 2>/dev/null",
                         token, bq, url) >= sizeof cmd)
        return -1;
    return run(cmd, resp, sizeof resp) == 0 ? 0 : -1;
}

static int discord_fetch(const char *token, const char *chan, const char *after,
                         char *out, size_t cap) {
    char url[DP_URL], cmd[DP_URL * 3];
    if (after && after[0])
        snprintf(url, sizeof url,
                 "https://discord.com/api/v10/channels/%s/messages?limit=10&after=%s",
                 chan, after);
    else
        snprintf(url, sizeof url,
                 "https://discord.com/api/v10/channels/%s/messages?limit=1", chan);
    if ((size_t)snprintf(cmd, sizeof cmd,
                         "curl -sS --max-time 20 -H 'Authorization: Bot %s' '%s' 2>/dev/null",
                         token, url) >= sizeof cmd)
        return -1;
    return run(cmd, out, cap);
}

/* Walk the top-level objects of a JSON array, oldest last in Discord order. */
static int for_each_object(char *arr, char **objs, int maxo) {
    int depth = 0, n = 0, instr = 0;
    char *p = arr, *start = NULL;
    for (; *p; p++) {
        if (instr) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') instr = 0;
            continue;
        }
        if (*p == '"') { instr = 1; continue; }
        if (*p == '{') { if (depth == 0) start = p; depth++; continue; }
        if (*p == '}') {
            depth--;
            if (depth == 0 && start && n < maxo) {
                p[1] = p[1];             /* keep buffer intact */
                objs[n++] = start;
                start = NULL;
            }
        }
    }
    return n;
}

static int live_loop(const char *token) {
    char ch[DP_MAXCH][DP_ID];
    char last[DP_MAXCH][DP_ID];
    const char *csv = getenv("CNET_DISCORD_CHANNELS");
    const char *prefix = getenv("CNET_DISCORD_PREFIX");
    int nch, i, loops = 0;
    int poll = getenv("CNET_DISCORD_POLL_SEC") ? atoi(getenv("CNET_DISCORD_POLL_SEC")) : 5;
    int maxl = getenv("CNET_DISCORD_MAX_LOOPS") ? atoi(getenv("CNET_DISCORD_MAX_LOOPS")) : 0;
    if (poll < 1) poll = 1;

    nch = parse_channels(csv, ch, DP_MAXCH);
    if (nch <= 0) {
        fprintf(stderr,
                "cnet_discord_peer: refusing to run live with an empty channel "
                "allowlist. Set CNET_DISCORD_CHANNELS=<id>[,<id>...]\n");
        return 2;
    }
    memset(last, 0, sizeof last);
    printf("discord_peer live: %d channel(s) allowlisted, poll=%ds\n", nch, poll);
    fflush(stdout);

    for (;;) {
        for (i = 0; i < nch; i++) {
            static char buf[DP_BUF];
            char *objs[16];
            int n, k;
            if (discord_fetch(token, ch[i], last[i], buf, sizeof buf) != 0) continue;
            if (buf[0] != '[') {
                if (g_verbose) fprintf(stderr, "discord_peer: fetch error on %s\n", ch[i]);
                continue;
            }
            n = for_each_object(buf, objs, 16);
            /* Discord returns newest first; walk backwards for chronology. */
            for (k = n - 1; k >= 0; k--) {
                char mid[DP_ID], content[DP_TEXT], user[128], reply[DP_TEXT];
                char *obj = objs[k];
                if (jstr(obj, "id", mid, sizeof mid) != 0) continue;
                snprintf(last[i], DP_ID, "%s", mid);
                if (jtrue(obj, "bot")) continue;            /* never answer ourselves */
                if (jstr(obj, "content", content, sizeof content) != 0 || !content[0])
                    continue;
                if (jstr(obj, "username", user, sizeof user) != 0)
                    snprintf(user, sizeof user, "discord");
                if (prefix && prefix[0]) {
                    if (strncmp(content, prefix, strlen(prefix)) != 0) continue;
                    memmove(content, content + strlen(prefix),
                            strlen(content + strlen(prefix)) + 1);
                }
                if (ask_marble(user, content, reply, sizeof reply) != 0) continue;
                if (reply[0]) (void)discord_post(token, ch[i], reply);
                printf("discord_peer: %s/%s -> replied\n", ch[i], user);
                fflush(stdout);
            }
        }
        loops++;
        if (maxl > 0 && loops >= maxl) break;
        sleep((unsigned)poll);
    }
    printf("DISCORD_PEER_LIVE_OK loops=%d\n", loops);
    return 0;
}

/* ---------------------------------------------------------------- selftest */
static int selftest(void) {
    char ch[DP_MAXCH][DP_ID], b[256];
    int n, fail = 0, checks = 0;
#define T(ok, m) do { checks++; printf("  %-54s %s\n", m, (ok) ? "PASS" : "FAIL"); \
                      if (!(ok)) fail++; } while (0)
    printf("=== cnet_discord_peer selftest ===\n");

    /* allowlist is mandatory and fail-closed */
    T(parse_channels(NULL, ch, DP_MAXCH) == 0, "allowlist: NULL -> zero channels");
    T(parse_channels("", ch, DP_MAXCH) == 0, "allowlist: empty -> zero channels");
    T(parse_channels("*", ch, DP_MAXCH) == 0, "allowlist: wildcard is not a channel");
    T(parse_channels("../../etc", ch, DP_MAXCH) == 0, "allowlist: path junk rejected");
    n = parse_channels("123,456", ch, DP_MAXCH);
    T(n == 2 && !strcmp(ch[0], "123") && !strcmp(ch[1], "456"), "allowlist: two ids parsed");
    T(channel_allowed("123", ch, n) && !channel_allowed("999", ch, n),
      "allowlist: only listed channels allowed");

    /* untrusted chat text must not escape into the shell */
    T(sh_quote("a'; rm -rf /", b, sizeof b) == 0 && b[0] == '\'' &&
          strstr(b, "'\\''") != NULL,
      "shell quoting neutralises embedded quote");
    json_escape("he said \"hi\"\nbye", b, sizeof b);
    T(strstr(b, "\\\"hi\\\"") != NULL && strstr(b, "\\n") != NULL, "json escaping");

    /* message field extraction */
    {
        char obj[] = "{\"id\":\"111\",\"content\":\"hello \\\"there\\\"\","
                     "\"author\":{\"username\":\"ana\",\"bot\":false}}";
        T(jstr(obj, "id", b, sizeof b) == 0 && !strcmp(b, "111"), "parse: message id");
        T(jstr(obj, "content", b, sizeof b) == 0 && strstr(b, "hello") != NULL,
          "parse: content with escapes");
        T(jstr(obj, "username", b, sizeof b) == 0 && !strcmp(b, "ana"), "parse: username");
        T(!jtrue(obj, "bot"), "parse: human author not flagged bot");
    }
    {
        char arr[] = "[{\"id\":\"2\",\"content\":\"b\"},{\"id\":\"1\",\"content\":\"a\"}]";
        char *objs[8];
        T(for_each_object(arr, objs, 8) == 2, "parse: two objects in array");
    }
    {
        char obj[] = "{\"id\":\"9\",\"author\":{\"bot\":true},\"content\":\"x\"}";
        T(jtrue(obj, "bot"), "parse: bot author detected (never self-answer)");
    }

    printf("\nchecks=%d failures=%d\n", checks, fail);
    if (fail) { printf("DISCORD_PEER_FAIL\n"); return 1; }
    printf("DISCORD_PEER_SELFTEST_PASS\n");
    return 0;
#undef T
}

static void usage(void) {
    fprintf(stderr,
            "usage: cnet_discord_peer [--dry-run|--live|--once TEXT|--selftest] [-v]\n"
            "  --once \"user: hello\"  inject one line through cnet_peer --peer discord\n"
            "  --live                 poll CNET_DISCORD_CHANNELS and reply\n"
            "Without a token: dry-run only (DISCORD_PEER_SKIP live).\n");
}

static int inject_once(const char *line) {
    char q[DP_TEXT * 4], cmd[DP_TEXT * 5];
    if (sh_quote(line, q, sizeof q) != 0) return 1;
    snprintf(cmd, sizeof cmd, "cnet_peer --peer discord %s", q);
    printf("discord_peer inject: %s\n", cmd);
    return system(cmd) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *tok = getenv("CNET_DISCORD_TOKEN");
    int dry = 1, live = 0;
    const char *once = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--live")) { live = 1; dry = 0; }
        else if (!strcmp(argv[i], "--selftest")) return selftest();
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--once") && i + 1 < argc) once = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
    }
    if (once) {
        if (inject_once(once) != 0) return 1;
        printf("DISCORD_PEER_ONCE_OK\n");
        if (!tok || !tok[0]) printf("DISCORD_PEER_SKIP live_no_token\n");
        printf("DISCORD_PEER_PASS\n");
        return 0;
    }
    if (live) {
        if (!tok || !tok[0]) {
            fprintf(stderr, "cnet_discord_peer: --live needs CNET_DISCORD_TOKEN\n");
            return 2;
        }
        return live_loop(tok);
    }
    if (dry || !tok || !tok[0]) {
        if (selftest() != 0) return 1;
        printf("DISCORD_PEER_SKIP live_no_token (set CNET_DISCORD_TOKEN + --live)\n");
        printf("Wire: Discord channel -> cnet_peer --peer discord \"user: msg\" -> cnetd\n");
        printf("DISCORD_PEER_PASS\n");
        return 0;
    }
    printf("DISCORD_PEER_PASS\n");
    return 0;
}
