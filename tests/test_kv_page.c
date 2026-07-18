/* Async paged KV — make kv_page → KV_PAGE_PASS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/cce/cce_kv_page.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    cce_kv_pager_opts o;
    cce_kv_pager *p = NULL;
    float k[8], v[8];
    int i, t;
    char dir[] = "kv_archive_test_XXXXXX";

    printf("== CNET async paged KV (hot/warm/cold) ==\n");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    cce_kv_pager_opts_default(&o, 8, 8, 100000); /* legal 100k, not alloc */
    o.page_len = 16;
    o.n_hot = 2; /* hot window = 32 positions */
    o.archive_dir = dir;
    o.async = 1;
    o.quant_cold = 1;
    o.rehydrate = 1;

    check(cce_kv_pager_open(&p, &o) == CCE_OK && p, "pager open");
    if (!p) return 1;
    check(cce_kv_pager_hot_capacity(p) == 32, "hot capacity 32");
    check(cce_kv_pager_legal_max(p) == 100000, "legal max 100k not dense alloc");

    /* Fill first 32 positions (no slide yet). */
    {
        int ok_fill = 1;
        for (t = 0; t < 32; ++t) {
            for (i = 0; i < 8; ++i) {
                k[i] = (float)(t * 10 + i);
                v[i] = (float)(t * 10 + i + 1000);
            }
            if (cce_kv_pager_write(p, t, k, v) != 0) ok_fill = 0;
        }
        check(ok_fill, "fill hot window 32 positions");
    }
    check(cce_kv_pager_k_row(p, 0) && cce_kv_pager_k_row(p, 0)[0] == 0.f,
          "pos0 still hot at fill");
    check(cce_kv_pager_k_row(p, 31) != NULL, "pos31 hot");

    /* Writing 32 should slide page 0..15 to cold async, reuse ring. */
    for (i = 0; i < 8; ++i) {
        k[i] = 999.f;
        v[i] = 1999.f;
    }
    check(cce_kv_pager_write(p, 32, k, v) == 0, "write pos32 slides window");
    check(cce_kv_pager_window_start(p) == 16, "window_start advanced to 16");
    check(cce_kv_pager_k_row(p, 0) == NULL, "pos0 no longer hot (cold)");
    check(cce_kv_pager_k_row(p, 16) != NULL, "pos16 still hot");
    check(cce_kv_pager_k_row(p, 32) != NULL &&
              cce_kv_pager_k_row(p, 32)[0] == 999.f,
          "pos32 written into reused tail");
    check(cce_kv_pager_pages_reused(p) >= 1, "page pointer reused");

    /* Double-buffer async: flush may still be in flight */
    {
        int n = 0;
        while (cce_kv_pager_pages_flushed(p) < 1 && n < 2000) {
            usleep(1000);
            n++;
        }
    }
    cce_kv_pager_sync(p);
    check(cce_kv_pager_pages_flushed(p) >= 1, "async cold flush completed");
    check(cce_kv_pager_verify_cold(p, 0, 0) == 0, "cold page file verifies");
    check(cce_kv_pager_quant_cold(p) == 1, "quant cold enabled");

    /* Rehydrate cold page 0 (positions 0..15) */
    check(cce_kv_pager_rehydrate_pos(p, 0) == 0, "rehydrate pos0");
    {
        const float *kr = cce_kv_pager_k_row_ex(p, 0);
        check(kr != NULL && kr[0] == 0.f, "rehydrated k[0]≈0");
        check(cce_kv_pager_pages_rehydrated(p) >= 1, "rehydrate counter");
    }

    /* Clamp attention jmin into hot window */
    check(cce_kv_pager_clamp_jmin(p, 0) == 16, "clamp jmin to window");

    /* write_slice: layer-offset pack into a hot row */
    {
        float ks[2] = {7.f, 8.f}, vs[2] = {9.f, 10.f};
        const float *kr;
        check(cce_kv_pager_write_slice(p, 32, 2, ks, 2, 2, vs, 2) == 0,
              "write_slice layer offset");
        kr = cce_kv_pager_k_row(p, 32);
        check(kr && kr[2] == 7.f && kr[3] == 8.f, "write_slice k visible");
    }

    /* Fill further to force more flushes — endless stream simulation */
    for (t = 33; t < 80; ++t) {
        for (i = 0; i < 8; ++i) {
            k[i] = (float)t;
            v[i] = (float)(t + 1);
        }
        if (cce_kv_pager_write(p, t, k, v) != 0) {
            check(0, "stream write");
            break;
        }
    }
    cce_kv_pager_sync(p);
    check(cce_kv_pager_pages_flushed(p) >= 3, "multiple cold pages documented");
    check(cce_kv_pager_cur_pos(p) >= 80, "cur_pos advanced past hot capacity");
    check(cce_kv_pager_hot_capacity(p) == 32, "RAM footprint still 32 slots");

    /* Rehydrate still works after more slides (older cold pages). */
    check(cce_kv_pager_k_row_ex(p, 5) != NULL, "k_row_ex rehydrate mid-stream");
    check(cce_kv_pager_rehydrate_enabled(p) == 1, "rehydrate enabled");

    printf("    flushed=%d reused=%d rehyd=%d window_start=%d cur_pos=%d "
           "archive=%s\n",
           cce_kv_pager_pages_flushed(p), cce_kv_pager_pages_reused(p),
           cce_kv_pager_pages_rehydrated(p), cce_kv_pager_window_start(p),
           cce_kv_pager_cur_pos(p), dir);

    cce_kv_pager_close(p);
    /* clean archive dir without shell */
    {
        char path[400];
        snprintf(path, sizeof path, "%s/ledger.tsv", dir);
        unlink(path);
        for (t = 0; t < 16; ++t) {
            snprintf(path, sizeof path, "%s/page_%06d.kvc", dir, t);
            unlink(path);
        }
        rmdir(dir);
    }

    /* f32 cold path (CNET_KV_QUANT=0 style) */
    {
        cce_kv_pager_opts o2;
        cce_kv_pager *p2 = NULL;
        char dir2[] = "kv_archive_f32_XXXXXX";
        if (mkdtemp(dir2)) {
            cce_kv_pager_opts_default(&o2, 4, 4, 1000);
            o2.page_len = 16; /* min page_len is 16 */
            o2.n_hot = 2;
            o2.archive_dir = dir2;
            o2.async = 0;
            o2.quant_cold = 0;
            o2.rehydrate = 1;
            check(cce_kv_pager_open(&p2, &o2) == CCE_OK, "f32 cold open");
            if (p2) {
                float kf[4] = {1, 2, 3, 4}, vf[4] = {5, 6, 7, 8};
                int okw = 1;
                for (t = 0; t < 40; ++t) {
                    if (cce_kv_pager_write(p2, t, kf, vf) != 0) okw = 0;
                }
                cce_kv_pager_sync(p2);
                check(okw && cce_kv_pager_pages_flushed(p2) >= 1,
                      "f32 cold flush");
                check(cce_kv_pager_quant_cold(p2) == 0, "f32 quant off");
                check(cce_kv_pager_rehydrate_pos(p2, 0) == 0,
                      "f32 rehydrate");
                {
                    const float *kr = cce_kv_pager_k_row_ex(p2, 0);
                    check(kr && kr[0] == 1.f, "f32 rehyd value");
                }
                cce_kv_pager_close(p2);
            }
            {
                char path[400];
                snprintf(path, sizeof path, "%s/ledger.tsv", dir2);
                unlink(path);
                for (t = 0; t < 8; ++t) {
                    snprintf(path, sizeof path, "%s/page_%06d.kvc", dir2, t);
                    unlink(path);
                }
                rmdir(dir2);
            }
        }
    }

    printf("KV_PAGE_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
