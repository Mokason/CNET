/* Hermetic stemmer bench. Marker: CNET_VSA_STEM_BENCH_PASS
 * Families that must meet, collisions that must not happen, and inputs the
 * stemmer must leave alone. The sweep gate decides whether stemming helps;
 * this gate decides whether it does what its comment says. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cnet_vsa_text.h"

static int fails = 0;
static void st(const char *in, char *out) { snprintf(out, 64, "%s", in); cnet_vsa_text_stem(out); }
static void expect(const char *in, const char *want) {
    char o[64]; st(in, o);
    int ok = strcmp(o, want) == 0;
    printf("  %-14s -> %-12s %s\n", in, o, ok ? "" : "[EXPECTED " );
    if (!ok) { printf("%s]\n", want); fails++; }
}
static void meet(const char *a, const char *b) {
    char x[64], y[64]; st(a, x); st(b, y);
    int ok = strcmp(x, y) == 0;
    printf("  %-14s ~ %-14s -> %s / %s %s\n", a, b, x, y, ok ? "" : "[MUST MEET]");
    if (!ok) fails++;
}
static void differ(const char *a, const char *b) {
    char x[64], y[64]; st(a, x); st(b, y);
    int ok = strcmp(x, y) != 0;
    printf("  %-14s != %-13s -> %s / %s %s\n", a, b, x, y, ok ? "" : "[MUST DIFFER]");
    if (!ok) fails++;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Stemmer Bench (Porter 1a/1b/1c/2-adverbial/5)\n");
    printf("=================================================================\n\n");

    printf("[1/4] Inflection families meet\n");
    meet("coating", "coatings");   meet("coating", "coated");
    meet("encode", "encoded");     meet("encode", "encoding");   meet("encodes", "encoding");
    meet("cache", "cached");       meet("cache", "caching");
    meet("route", "routed");       meet("route", "routing");
    meet("store", "stored");       meet("stored", "storing");
    meet("compile", "compiled");   meet("time", "timed");        meet("time", "timing");
    meet("rate", "rated");         meet("rate", "rates");
    meet("apply", "applied");      meet("apply", "applying");    meet("applies", "applied");
    meet("family", "families");    meet("study", "studies");
    meet("thermal", "thermally");  meet("careful", "carefully");
    meet("running", "runs");       meet("add", "added");         meet("add", "adding");
    meet("process", "processes");  meet("process", "processing");
    meet("embedded", "embedding"); meet("agree", "agreed");      meet("feed", "feeds");
    meet("stuff", "stuffed");      meet("install", "installed");
    meet("possible", "possibly");  meet("nice", "nicely");

    printf("\n[2/4] Unrelated words stay apart\n");
    differ("apply", "app");   differ("reply", "rep");   differ("early", "ear");
    differ("added", "ad");    differ("rate", "rat");    differ("site", "sit");
    differ("bus", "bu");      differ("status", "statu");

    printf("\n[3/4] Left alone\n");
    expect("gpu", "gpu"); expect("ss", "ss"); expect("bus", "bus");
    expect("v2", "v2"); expect("x86_64", "x86_64"); expect("feed", "feed"); expect("speed", "speed");
    expect("add", "add"); expect("egg", "egg"); expect("staff", "staff");

    printf("\n[4/4] Buffer discipline: output never longer than input\n");
    const char *long_words[] = { "internationalization", "characteristically", "overwhelmingly", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaay" };
    for (size_t i = 0; i < 4; ++i) {
        char o[64]; st(long_words[i], o);
        if (strlen(o) > strlen(long_words[i]) || strlen(o) == 0) { printf("  length violation on %s\n", long_words[i]); fails++; }
    }
    printf("  ok\n");

    if (fails) {
        printf("\n CNET_VSA_STEM_BENCH_FAIL: %d checks failed\n", fails);
        return 1;
    }
    printf("\n=================================================================\n");
    printf(" CNET_VSA_STEM_BENCH_PASS\n");
    printf("=================================================================\n");
    return 0;
}
