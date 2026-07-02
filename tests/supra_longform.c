/* supra_longform — skeleton-driven long-form injector for the tiny Supra model.
 *
 * The probe (tests/supra_context_probe.c) found two walls:
 *   - HARD ceiling   = block_size (384 positions) — architectural.
 *   - COHERENCE decay ~120 tokens — capability ceiling of a 4-layer/256-dim net.
 *
 * This injector breaks BOTH by sliding a sub-384 window and cutting each chunk
 * before the decay point. The injector — not the model — owns the long-range
 * structure: a caller-supplied beat list (outline) carries entities + the ending,
 * and a verbatim recent tail carries local smoothness. No learned summary.
 *
 * STEERING LESSON (from the first run): a 4-layer model only renders content that
 * sits AT THE CURSOR as an unfinished continuation. So beats are phrased as
 * INCOMPLETE OPENERS (entity-first, no terminal period) and placed LAST in the
 * context — [<TEXT>][tail][beat-opener] — so the model completes the beat.
 *
 * Two runs for contrast:
 *   B (skeleton)  — beat-steered openers; should follow the outline (Mia/Pip).
 *   A (baseline)  — pure sliding window, no beats; should drift to its priors.
 *
 * Reuses only the public API (encode/generate/decode); zero edits to the CCE core.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../include/cce/cce_safetensors.h"

#define CHUNK_MAX 96     /* buffer cap; actual tokens/hop = chunk_n (runtime arg)  */
#define OVERLAP   48     /* verbatim tail re-fed each hop for a smooth seam        */
#define MAXSTORY  6000
#define CTX_CAP   400    /* keep context well under block_size (384) headroom      */
#define EOT_ID    50256  /* <|endoftext|>                                          */

static int g_text_start_id = -1;   /* the <TEXT> control id, discovered at startup */

typedef struct { const char* text; const char* kw; } beat_t;

/* encode text WITHOUT the leading <TEXT> control token, so beats compose cleanly */
static int encode_raw(cce_supra_tokenizer* tok, const char* text, int* ids, int max) {
    int tmp[256];
    int n = cce_supra_encode_text(tok, text, tmp, 256);
    int start = (n > 0 && tmp[0] == g_text_start_id) ? 1 : 0;
    int out = 0;
    for (int i = start; i < n && out < max; i++) ids[out++] = tmp[i];
    return out;
}

static float distinct_ratio(const int* ids, int n) {
    if (n <= 0) return 0.0f;
    int distinct = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) if (ids[j] == ids[i]) { seen = 1; break; }
        if (!seen) distinct++;
    }
    return (float)distinct / (float)n;
}

/* case-insensitive substring search */
static int icontains(const char* hay, const char* needle) {
    if (!needle || !*needle) return 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int icount(const char* hay, const char* needle) {
    int c = 0; size_t nl = strlen(needle);
    if (!nl) return 0;
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) c++;
    }
    return c;
}

/* decode ids[0..n) into a freshly malloc'd string (caller frees) */
static char* decode_dup(cce_supra_a2a* a, const int* ids, int n) {
    int bufsz = (n + 8) * 12 + 64;
    char* txt = (char*)malloc((size_t)bufsz);
    if (txt) cce_supra_decode_text(a->tokenizer, ids, n, txt, bufsz);
    return txt;
}

/* Keep only the FIRST complete sentence of raw garnish, and gate its quality.
   Returns 1 (sentence written to out) if it passes; 0 to drop the garnish.
   Gate: a sentence-ender exists, >= MIN_WORDS words, and mostly letters — so a
   short, clean embellishment is kept and degraded word-salad is dropped. The
   verbatim beats carry the plot regardless, so dropping garnish is always safe. */
#define MIN_WORDS 3
static int clean_first_sentence(const char* raw, char* out, int outsz) {
    int i = 0;
    while (raw[i] == ' ' || raw[i] == '\n' || raw[i] == '\r' || raw[i] == '\t') i++;
    int end = -1;
    for (int k = i; raw[k]; k++) { char c = raw[k]; if (c=='.'||c=='!'||c=='?') { end = k; break; } }
    if (end < 0) return 0;                       /* no sentence boundary -> drop */
    int len = end - i + 1;
    if (len <= 0 || len >= outsz) return 0;
    memcpy(out, raw + i, (size_t)len); out[len] = 0;

    int words = 0, alpha = 0, nonspace = 0, inword = 0;
    for (int k = 0; out[k]; k++) {
        char c = out[k];
        if (c == ' ') inword = 0;
        else { nonspace++; if (!inword) { words++; inword = 1; } if (isalpha((unsigned char)c)) alpha++; }
    }
    if (words < MIN_WORDS) return 0;
    if (nonspace == 0 || (float)alpha / (float)nonspace < 0.6f) return 0;
    return 1;
}

/* ---------------- Hybrid: injector owns the prose, model garnishes ----------------
   The output is the verbatim outline (so the piece PROVABLY follows the plan and
   ends at the closer beat). Between beats the model adds one short embellishment
   sentence, generated from the clean, complete beat — i.e. used only where it works
   (~the first sentence of continuation). Garnish that fails the quality gate is
   simply dropped, so the artifact is always clean and complete. */
static void run_hybrid(cce_supra_a2a* a, const beat_t* beats, int nbeats,
                       int garnish_n, float temp, int topk) {
    const int DOCSZ = 1 << 16;
    char* doc = (char*)malloc((size_t)DOCSZ); int dl = 0; doc[0] = 0;
    int kept = 0, dropped = 0;

    /* On-topic priming: an incomplete opener at the cursor is the ONE regime this
       model handles, so each garnish is seeded with a connector that refers back to
       what just happened ("It was...", "Soon...", "Then..."). */
    const char* primers[] = { "It", "Soon", "Then" };

    printf("\n========== Hybrid: injector writes verbatim, model garnishes (garnish<=%d tok) ==========\n",
           garnish_n);
    for (int b = 0; b < nbeats; b++) {
        dl += snprintf(doc + dl, DOCSZ - dl, "%s ", beats[b].text);   /* verbatim beat */
        if (b == nbeats - 1) {                                        /* keep the ending clean */
            printf("  beat %2d/%d  (closer — no garnish)\n", b + 1, nbeats);
            break;
        }

        const char* primer = primers[b % 3];
        char opener[1024];                                           /* beat + " " + primer @ cursor */
        snprintf(opener, sizeof(opener), "%s %s", beats[b].text, primer);

        int ctx[CTX_CAP + 32]; int nc = 0;
        if (g_text_start_id >= 0) ctx[nc++] = g_text_start_id;
        int braw[300]; int nb = encode_raw(a->tokenizer, opener, braw, 300);
        for (int i = 0; i < nb && nc < CTX_CAP; i++) ctx[nc++] = braw[i];

        int gen[CHUNK_MAX];
        int ng = cce_supra_generate_text(a->model, ctx, nc, gen, garnish_n, temp, topk);
        if (ng > 0 && gen[ng - 1] == EOT_ID) ng--;
        char* cont = decode_dup(a, gen, ng);                         /* continuation after primer */
        char candidate[512], gclean[256];
        snprintf(candidate, sizeof(candidate), "%s%s", primer, cont ? cont : "");
        int ok = clean_first_sentence(candidate, gclean, sizeof(gclean));
        if (ok) { dl += snprintf(doc + dl, DOCSZ - dl, "%s ", gclean); kept++;
                  printf("  beat %2d/%d  +garnish: \"%s\"\n", b + 1, nbeats, gclean); }
        else    { dropped++; printf("  beat %2d/%d  (garnish dropped)\n", b + 1, nbeats); }
        free(cont);
    }

    int words = 0, inw = 0; for (int k = 0; doc[k]; k++) { if (doc[k]==' '||doc[k]=='\n') inw=0; else if(!inw){words++;inw=1;} }
    printf("  verdict: %d/%d beats verbatim (outline followed 100%%, ends at closer beat) | "
           "garnish kept %d / dropped %d | ~%d words\n", nbeats, nbeats, kept, dropped, words);
    printf("  --- FINISHED PIECE ---\n%s\n  --- END ---\n", doc);
    free(doc);
}

/* ---------------- Approach B: skeleton-driven ---------------- */
static int run_skeleton(cce_supra_a2a* a, const beat_t* beats, int nbeats, int chunk_n, float temp, int topk) {
    cce_supra_decomposed* m = a->model;
    int* story = (int*)malloc(sizeof(int) * MAXSTORY);
    int ns = 0, ended = 0, kw_hits = 0;
    float dr_sum = 0.0f; int dr_n = 0;

    printf("\n========== Approach B: skeleton-driven (%d beats, chunk=%d) ==========\n", nbeats, chunk_n);
    for (int b = 0; b < nbeats && !ended; b++) {
        int ctx[CTX_CAP + 32]; int nc = 0;
        if (g_text_start_id >= 0) ctx[nc++] = g_text_start_id;      /* doc in <TEXT> mode  */
        int tstart = ns - OVERLAP; if (tstart < 0) tstart = 0;      /* recent tail FIRST   */
        for (int i = tstart; i < ns && nc < CTX_CAP; i++) ctx[nc++] = story[i];
        int braw[256];                                              /* beat opener AT CURSOR */
        int nb = encode_raw(a->tokenizer, beats[b].text, braw, 256);
        for (int i = 0; i < nb && nc < CTX_CAP; i++) ctx[nc++] = braw[i];

        int gen[CHUNK_MAX];
        int ng = cce_supra_generate_text(m, ctx, nc, gen, chunk_n, temp, topk);
        int keep = ng;
        if (ng > 0 && gen[ng - 1] == EOT_ID) { keep = ng - 1; ended = 1; }
        for (int i = 0; i < keep && ns < MAXSTORY; i++) story[ns++] = gen[i];

        char* chunk_txt = decode_dup(a, gen, keep);
        int hit = chunk_txt ? icontains(chunk_txt, beats[b].kw) : 0;
        kw_hits += hit;
        float dr = distinct_ratio(gen, keep); dr_sum += dr; dr_n++;
        printf("  beat %2d/%d  ctx=%3d +%2d tok  distinct=%.2f  kw[\"%s\"]=%s%s\n",
               b + 1, nbeats, nc, keep, dr, beats[b].kw, hit ? "HIT " : "miss",
               ended ? "  [EOT]" : "");
        free(chunk_txt);
    }

    char* full = decode_dup(a, story, ns);
    printf("  verdict: %d tok | %s | avg distinct=%.2f | beat-kw hits=%d/%d | Mia=%d Pip=%d | %s\n",
           ns, ns > m->block_size ? "PAST 384 wall" : "under wall",
           dr_n ? dr_sum / dr_n : 0.0f, kw_hits, dr_n,
           full ? icount(full, "Mia") : 0, full ? icount(full, "Pip") : 0,
           ended ? "stopped at closing beat" : "rendered all beats");
    printf("  --- STORY (%d tok) ---\n%s\n  --- END ---\n", ns, full ? full : "");
    free(full); free(story);
    return ns;
}

/* ---------------- Approach A: pure sliding window (baseline) ---------------- */
static int run_baseline(cce_supra_a2a* a, const char* seed, int nhops, int chunk_n, float temp, int topk) {
    cce_supra_decomposed* m = a->model;
    int* story = (int*)malloc(sizeof(int) * MAXSTORY);
    int ns = 0;
    const int CARRY = 160;
    float dr_sum = 0.0f; int dr_n = 0;

    printf("\n========== Approach A: pure sliding window, no beats (chunk=%d) ==========\n", chunk_n);
    int s0[256]; int n0 = cce_supra_encode_text(a->tokenizer, seed, s0, 256);
    for (int h = 0; h < nhops; h++) {
        int ctx[CTX_CAP + 32]; int nc = 0;
        if (h == 0) {
            for (int i = 0; i < n0 && nc < CTX_CAP; i++) ctx[nc++] = s0[i];
        } else {
            if (g_text_start_id >= 0) ctx[nc++] = g_text_start_id;
            int tstart = ns - CARRY; if (tstart < 0) tstart = 0;
            for (int i = tstart; i < ns && nc < CTX_CAP; i++) ctx[nc++] = story[i];
        }
        int gen[CHUNK_MAX];
        int ng = cce_supra_generate_text(m, ctx, nc, gen, chunk_n, temp, topk);
        int keep = ng; if (ng > 0 && gen[ng - 1] == EOT_ID) keep = ng - 1;
        for (int i = 0; i < keep && ns < MAXSTORY; i++) story[ns++] = gen[i];
        float dr = distinct_ratio(gen, keep); dr_sum += dr; dr_n++;
        printf("  hop %2d/%d  ctx=%3d +%2d tok  distinct=%.2f\n", h + 1, nhops, nc, keep, dr);
    }
    char* full = decode_dup(a, story, ns);
    printf("  verdict: %d tok | %s | avg distinct=%.2f | Mia=%d Pip=%d | no learned ending\n",
           ns, ns > m->block_size ? "PAST 384 wall" : "under wall",
           dr_n ? dr_sum / dr_n : 0.0f,
           full ? icount(full, "Mia") : 0, full ? icount(full, "Pip") : 0);
    printf("  --- STORY (%d tok) ---\n%s\n  --- END ---\n", ns, full ? full : "");
    free(full); free(story);
    return ns;
}

int main(int argc, char** argv) {
    /* usage: supra_longform [mode] [n]
       mode = "hybrid" (default) | "skeleton"
       n    = garnish tokens (hybrid) or chunk tokens (skeleton); default 16 */
    const char* mode = (argc > 1) ? argv[1] : "hybrid";
    int chunk_n = (argc > 2) ? atoi(argv[2]) : 16;
    if (chunk_n < 1) chunk_n = 1;
    if (chunk_n > CHUNK_MAX) chunk_n = CHUNK_MAX;

    cce_supra_a2a* a = NULL;
    if (cce_supra_a2a_load(&a, "supra_cache") != CCE_OK || !a || !a->model) {
        printf("Could not load Supra model from ./supra_cache (model.safetensors missing?)\n");
        return 1;
    }
    int sent[4]; int nsent = cce_supra_encode_text(a->tokenizer, "", sent, 4);
    if (nsent >= 1) g_text_start_id = sent[0];

    printf("=== Supra long-form injector (mode=%s) ===\n", mode);
    printf("  block_size=%d  n=%d  OVERLAP=%d  <TEXT>id=%d\n",
           a->model->block_size, chunk_n, OVERLAP, g_text_start_id);

    /* For HYBRID, beats are COMPLETE sentences — they ARE the output, so they carry
       the plot + entities + ending verbatim. (For the SKELETON experiment they're
       used only as steering context; the .kw field is the steering-adherence probe.) */
    beat_t beats[] = {
        {"Once upon a time, a little girl named Mia found a tiny blue egg in the garden.", "egg"},
        {"Mia carefully carried the blue egg inside and showed it to her mother in the kitchen.", "Mia"},
        {"That night, the blue egg began to crack, and a small bird with shiny feathers hopped out.", "bird"},
        {"Mia named the little bird Pip and fed him soft bread crumbs every morning.", "Pip"},
        {"As the days passed, Pip grew bigger and stronger, and he longed to fly through the open window.", "Pip"},
        {"Mia felt sad, but she opened the window wide so that Pip could soar into the bright sky.", "window"},
        {"Pip flew in a happy circle, sang a sweet song, and landed gently back on Mia's shoulder.", "Pip"},
        {"And from that day on, Mia and Pip were the very best of friends, forever. The end.", "friends"},
    };
    int nbeats = (int)(sizeof(beats) / sizeof(beats[0]));

    if (strcmp(mode, "skeleton") == 0) {           /* the steering experiment (for the record) */
        run_skeleton(a, beats, nbeats, chunk_n, 0.7f, 40);
        run_baseline(a, beats[0].text, nbeats, chunk_n, 0.7f, 40);
    } else {                                        /* default: the chosen hybrid path */
        run_hybrid(a, beats, nbeats, chunk_n, 0.7f, 40);
    }

    cce_supra_a2a_free(a);
    return 0;
}
