/* cnet_listen — STT stub (delivery only). 
 * v1: reads a text file or stdin as "transcript" and forwards to cnet_peer.
 * Real whisper/cpp hook: set CNET_STT_CMD to a binary that prints transcript.
 *
 * usage: cnet_listen [--peer NAME] [--file wav_or_txt] 
 *        echo "hello" | cnet_listen
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *peer = "user";
    const char *file = NULL;
    char buf[4096];
    char cmd[4400];
    size_t n = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--peer") && i + 1 < argc) peer = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
    }
    if (file) {
        const char *stt = getenv("CNET_STT_CMD");
        if (stt && stt[0]) {
            snprintf(cmd, sizeof cmd, "%s '%s'", stt, file);
            FILE *fp = popen(cmd, "r");
            if (!fp) return 1;
            n = fread(buf, 1, sizeof buf - 1, fp);
            buf[n] = 0;
            pclose(fp);
        } else {
            FILE *f = fopen(file, "r");
            if (!f) {
                fprintf(stderr, "cnet_listen: no file/STT (set CNET_STT_CMD for wav)\n");
                printf("LISTEN_SKIP_NO_STT\nLISTEN_PASS\n");
                return 0;
            }
            n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
        }
    } else {
        n = fread(buf, 1, sizeof buf - 1, stdin);
        buf[n] = 0;
    }
    /* trim */
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (!n) {
        printf("LISTEN_EMPTY\nLISTEN_PASS\n");
        return 0;
    }
    printf("TRANSCRIPT %s\n", buf);
    snprintf(cmd, sizeof cmd, "cnet_peer --peer '%s' '%s'", peer, buf);
    if (system(cmd) != 0) return 1;
    printf("LISTEN_PASS\n");
    return 0;
}
