#include "../include/cnet_serve_decode.h"
#include <stdio.h>
#include <string.h>

int cnet_serve_decode_picks(const int *picks, size_t n_picks,
                            const char *const *labels, size_t n_labels,
                            char *out, size_t out_cap) {
    size_t i, used = 0;
    if (!picks || !out || out_cap < 2 || n_picks == 0) return -1;
    out[0] = 0;
    for (i = 0; i < n_picks; i++) {
        char piece[128];
        int idx = picks[i];
        int n;
        if (labels && (size_t)idx < n_labels && labels[idx] && labels[idx][0])
            n = snprintf(piece, sizeof piece, "%s%s", i ? ", " : "", labels[idx]);
        else
            n = snprintf(piece, sizeof piece, "%s%d", i ? "," : "", idx);
        if (n < 0) return -1;
        if (used + (size_t)n + 1 >= out_cap) break;
        memcpy(out + used, piece, (size_t)n + 1);
        used += (size_t)n;
    }
    return (int)used;
}

int cnet_serve_decode_json(const int *picks, size_t n_picks,
                           const char *const *labels, size_t n_labels,
                           char *out, size_t out_cap) {
    char text[256];
    char parr[128];
    size_t i, u = 0;
    int tn;
    if (!out || out_cap < 8 || !picks) return -1;
    parr[0] = 0;
    for (i = 0; i < n_picks && u + 12 < sizeof parr; i++) {
        int n = snprintf(parr + u, sizeof parr - u, "%s%d", i ? "," : "", picks[i]);
        if (n < 0) break;
        u += (size_t)n;
    }
    tn = cnet_serve_decode_picks(picks, n_picks, labels, n_labels, text, sizeof text);
    if (tn < 0) text[0] = 0;
    return snprintf(out, out_cap, "{\"picks\":[%s],\"text\":\"%s\"}", parr, text);
}
