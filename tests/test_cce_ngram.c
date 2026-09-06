#include "../include/cce/cce_ngram.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(const char *name, int cond)
{
    if (!cond) {
        printf("FAIL %s\n", name);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    printf("cce_ngram %s\n", cce_ngram_version());
    expect("open reject order 0", cce_ngram_open(0, 64) == NULL);
    expect("open reject order 9", cce_ngram_open(9, 64) == NULL);

    cce_ngram *t = cce_ngram_open(3, 256);
    expect("open trigram", t != NULL);
    if (!t)
        return 1;

    /* toks: a b c a b d   ids 1 2 3 1 2 4 */
    uint32_t toks[] = {1, 2, 3, 1, 2, 4};
    expect("ingest", cce_ngram_ingest(t, toks, 6) == CCE_NGRAM_OK);

    uint32_t ab[2] = {1, 2};
    expect("count(a b → c)==1", cce_ngram_count(t, ab, 2, 3) == 1);
    expect("count(a b → d)==1", cce_ngram_count(t, ab, 2, 4) == 1);
    expect("count(a → b)==2", cce_ngram_count(t, toks, 1, 2) == 2);
    expect("unigram a==2", cce_ngram_count(t, NULL, 0, 1) == 2);

    uint32_t ot[8];
    float op[8];
    int kout = 0;
    expect("predict a b", cce_ngram_predict(t, ab, 2, ot, op, 4, &kout) == CCE_NGRAM_OK);
    expect("predict k>=2", kout >= 2);
    /* both c and d should appear */
    int saw_c = 0, saw_d = 0;
    for (int i = 0; i < kout; i++) {
        if (ot[i] == 3)
            saw_c = 1;
        if (ot[i] == 4)
            saw_d = 1;
        printf("  next=%u p=%.3f\n", ot[i], op[i]);
    }
    expect("saw c and d", saw_c && saw_d);

    /* unseen trigram context x y → backoff to unigram */
    uint32_t xy[2] = {9, 9};
    kout = 0;
    int rc = cce_ngram_predict(t, xy, 2, ot, op, 4, &kout);
    expect("backoff predict", rc == CCE_NGRAM_OK && kout > 0);

    cce_ngram_stats st;
    cce_ngram_get_stats(t, &st);
    expect("ingested 6", st.tokens_ingested == 6);
    expect("backoff>0", st.backoff > 0);
    printf("  used=%zu adds=%llu hits=%llu backoff=%llu\n", st.used,
           (unsigned long long)st.adds, (unsigned long long)st.hits,
           (unsigned long long)st.backoff);

    /* shard merge */
    cce_ngram *a = cce_ngram_open(3, 128);
    cce_ngram *b = cce_ngram_open(3, 128);
    uint32_t s1[] = {1, 2, 3};
    uint32_t s2[] = {1, 2, 4};
    cce_ngram_ingest(a, s1, 3);
    cce_ngram_ingest(b, s2, 3);
    expect("merge", cce_ngram_merge(a, b) == CCE_NGRAM_OK);
    expect("merged a b → c", cce_ngram_count(a, ab, 2, 3) == 1);
    expect("merged a b → d", cce_ngram_count(a, ab, 2, 4) == 1);

    /* not a certificate */
    expect("predict is not CERT (just counts)", 1);

    cce_ngram_close(t);
    cce_ngram_close(a);
    cce_ngram_close(b);

    if (fails) {
        printf("NGRAM_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("NGRAM_PASS\n");
    return 0;
}
