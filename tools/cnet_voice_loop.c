/* cnet_voice_loop — CERT/peer ask then optional TTS (delivery only).
 * Prefer: cnet_peer → parse ANSWER → edge-tts if available.
 * No Python required if CNET_SPEAK_CMD is a binary; else tries edge-tts once.
 *
 * usage: cnet_voice_loop [--peer NAME] [--no-play] "query"
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run_peer(const char *peer, const char *q, char *out, size_t nout) {
    char cmd[1024];
    FILE *fp;
    size_t n = 0;
    if (peer && peer[0])
        snprintf(cmd, sizeof cmd, "cnet_peer --peer '%s' '%s' 2>/dev/null", peer, q);
    else
        snprintf(cmd, sizeof cmd, "cnet_peer '%s' 2>/dev/null", q);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while (n + 1 < nout) {
        size_t k = fread(out + n, 1, nout - 1 - n, fp);
        if (!k) break;
        n += k;
    }
    out[n] = 0;
    pclose(fp);
    return 0;
}

static void extract_answer(const char *blob, char *ans, size_t nans) {
    const char *p = strstr(blob, "\nANSWER ");
    size_t i = 0;
    ans[0] = 0;
    if (!p) p = strstr(blob, "ANSWER ");
    if (!p) return;
    p = strchr(p, ' ');
    if (!p) return;
    p++;
    while (*p && *p != '\n' && i + 1 < nans) ans[i++] = *p++;
    ans[i] = 0;
}

static int may_voice_line(const char *blob) {
    const char *p = strstr(blob, "MAY_VOICE ");
    if (!p) return 0;
    return strstr(p, "MAY_VOICE 1") != NULL;
}

int main(int argc, char **argv) {
    const char *peer = NULL;
    const char *q = NULL;
    int play = 1;
    char blob[16384];
    char ans[2048];
    char speak[2400];
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--peer") && i + 1 < argc) peer = argv[++i];
        else if (!strcmp(argv[i], "--no-play")) play = 0;
        else if (argv[i][0] != '-') q = argv[i];
    }
    if (!q) {
        fprintf(stderr, "usage: cnet_voice_loop [--peer NAME] [--no-play] \"query\"\n");
        return 2;
    }
    if (run_peer(peer, q, blob, sizeof blob) != 0) {
        fprintf(stderr, "peer failed\n");
        return 1;
    }
    fputs(blob, stdout);
    extract_answer(blob, ans, sizeof ans);
    if (!ans[0]) {
        fprintf(stderr, "VOICE_LOOP_NO_ANSWER\n");
        return 1;
    }
    if (!may_voice_line(blob)) {
        printf("VOICE_LOOP_SKIP_NO_MAY_VOICE\n");
        printf("VOICE_LOOP_PASS\n");
        return 0;
    }
    if (!play) {
        printf("VOICE_LOOP_PASS text_only\n");
        return 0;
    }
    {
        const char *sc = getenv("CNET_SPEAK_CMD");
        if (sc && sc[0]) {
            snprintf(speak, sizeof speak, "%s %s", sc, ans);
            /* unsafe if ans has quotes — use env file */
            {
                FILE *tf = fopen("/tmp/cnet_voice_line.txt", "w");
                if (tf) {
                    fputs(ans, tf);
                    fclose(tf);
                    snprintf(speak, sizeof speak, "%s /tmp/cnet_voice_line.txt", sc);
                    if (system(speak) == 0) {
                        printf("VOICE_LOOP_PASS\n");
                        return 0;
                    }
                }
            }
        }
        /* fallback edge-tts if present */
        {
            FILE *tf = fopen("/tmp/cnet_voice_line.txt", "w");
            if (tf) {
                fputs(ans, tf);
                fclose(tf);
            }
            if (system("command -v edge-tts >/dev/null 2>&1") == 0) {
                if (system("edge-tts --file /tmp/cnet_voice_line.txt --write-media /tmp/cnet_voice.mp3 "
                           "&& (command -v mpv >/dev/null && mpv --no-terminal /tmp/cnet_voice.mp3 "
                           "|| command -v ffplay >/dev/null && ffplay -nodisp -autoexit /tmp/cnet_voice.mp3 "
                           "|| true)") == 0) {
                    printf("VOICE_LOOP_PASS\n");
                    return 0;
                }
            }
            /* still pass: text path works; TTS optional */
            printf("VOICE_LOOP_PASS no_tts_backend\n");
            return 0;
        }
    }
}
