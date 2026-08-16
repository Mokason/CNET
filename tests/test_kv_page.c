/* Async paged KV — make kv_page → KV_PAGE_PASS */
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#include "../include/cce/cce_kv_page.h"
#include "../include/cnet_platform.h"  /* cnet_mkdir */

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

typedef struct {
    cce_kv_pager *pager;
    float k[4], v[4];
    atomic_int started;
    atomic_int done;
    int rc;
} slide_writer_ctx;

static void *slide_writer(void *opaque) {
    slide_writer_ctx *ctx = (slide_writer_ctx *)opaque;
    atomic_store(&ctx->started, 1);
    ctx->rc = cce_kv_pager_write(ctx->pager, 32, ctx->k, ctx->v);
    atomic_store(&ctx->done, 1);
    return NULL;
}

typedef struct {
    cce_kv_pager *pager;
    atomic_int done;
} sync_wait_ctx;

static void *sync_waiter(void *opaque) {
    sync_wait_ctx *ctx = (sync_wait_ctx *)opaque;
    cce_kv_pager_sync(ctx->pager);
    atomic_store(&ctx->done, 1);
    return NULL;
}

int main(void) {
    cce_kv_pager_opts o;
    cce_kv_pager *p = NULL;
    float k[8], v[8];
    int i, t;
    char dir[] = "kv_archive_test_XXXXXX";

    printf("== CNET async paged KV (hot/warm/cold) ==\n");

    {
        cce_kv_pager_opts bad;
        cce_kv_pager *bad_pager = NULL;
        cce_kv_pager_opts_default(&bad, 4, 4, 256);
        bad.page_len = INT_MAX;
        bad.n_hot = 2;
        check(cce_kv_pager_open(&bad_pager, &bad) == CCE_ERR_INVALID_ARG &&
                  bad_pager == NULL,
              "overflowing page geometry is refused before allocation");
    }

    if (!cnet_mkdtemp(dir)) {
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
    {
        cce_kv_pager_opts million;
        cce_kv_pager *pm = NULL;
        cce_kv_pager_opts_default(&million, 8, 8, CNET_CTX_LEGAL_MAX);
        million.page_len = 16;
        million.n_hot = 2;
        million.archive_dir = dir;
        million.async = 0;
        check(cce_kv_pager_open(&pm, &million) == CCE_OK && pm,
              "pager open 1M legal");
        check(pm && cce_kv_pager_legal_max(pm) == CNET_CTX_LEGAL_MAX,
              "legal max 1M not dense alloc");
        check(pm && cce_kv_pager_hot_capacity(pm) == 32, "1M hot still 32");
        if (pm) cce_kv_pager_close(pm);
    }

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
    check(cce_kv_pager_write_failures(p) == 0,
          "normal cold flush has no hidden storage failures");
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

    /* clear() starts a new sequence generation. COLD files from the previous
     * generation may remain on disk, but must never be rehydrated as current
     * KV state before that position is evicted and rewritten again. */
    check(cce_kv_pager_clear(p) == 0, "clear starts a fresh KV generation");
    check(cce_kv_pager_rehydrate_pos(p, 32) != 0,
          "clear rejects stale COLD pages from the prior generation");

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

    /* sync must include a dequeued write that is still blocked in storage I/O,
     * not only jobs that remain in the queue. A FIFO makes that state
     * deterministic: the worker dequeues page 0, then blocks opening it until
     * this test supplies a reader.
     *
     * Requires mkfifo. Windows named pipes live in a separate namespace that
     * fopen() cannot reach by path, so there is no way to build the same
     * blocking sink -- this case is SKIPPED there, and says so, rather than
     * being silently dropped from the count. */
#if CNET_HAVE_MKFIFO
    {
        cce_kv_pager_opts os;
        cce_kv_pager *ps = NULL;
        char sdir[] = "kv_sync_wait_XXXXXX";
        char page_path[400];
        pthread_t sync_thr;
        sync_wait_ctx sw;
        int sync_started = 0;

        if (cnet_mkdtemp(sdir)) {
            snprintf(page_path, sizeof page_path, "%s/page_000000.kvc", sdir);
            check(mkfifo(page_path, 0600) == 0, "sync: blocking cold sink created");
            cce_kv_pager_opts_default(&os, 4, 4, 256);
            os.page_len = 16;
            os.n_hot = 2;
            os.archive_dir = sdir;
            os.async = 1;
            os.quant_cold = 0;
            check(cce_kv_pager_open(&ps, &os) == CCE_OK, "sync: pager open");
            if (ps) {
                float kf[4] = {1, 2, 3, 4}, vf[4] = {5, 6, 7, 8};
                int spins = 0;
                int stream_ok = 1;
                for (t = 0; t < 33; ++t)
                    if (cce_kv_pager_write(ps, t, kf, vf) != 0) stream_ok = 0;
                check(stream_ok, "sync: stream writes");
                while (cce_kv_pager_queue_depth(ps) != 0 && spins++ < 2000)
                    usleep(1000);
                check(cce_kv_pager_queue_depth(ps) == 0,
                      "sync: worker dequeued blocked write");
                sw.pager = ps;
                atomic_init(&sw.done, 0);
                sync_started = pthread_create(&sync_thr, NULL, sync_waiter, &sw) == 0;
                check(sync_started, "sync: waiter thread started");
                usleep(20000);
                check(!atomic_load(&sw.done),
                      "sync: waits for dequeued in-flight write");
                {
                    FILE *drain = fopen(page_path, "rb");
                    unsigned char buf[1024];
                    check(drain != NULL, "sync: cold sink reader opens");
                    if (drain) {
                        while (fread(buf, 1, sizeof buf, drain) > 0) { }
                        fclose(drain);
                    }
                }
                if (sync_started) pthread_join(sync_thr, NULL);
                check(atomic_load(&sw.done), "sync: completes after storage I/O");
                cce_kv_pager_close(ps);
            }
            unlink(page_path);
            snprintf(page_path, sizeof page_path, "%s/ledger.tsv", sdir);
            unlink(page_path);
            rmdir(sdir);
        }
    }
#else
    printf("  %-58s SKIP (no mkfifo on this platform)\n",
           "sync: waits for dequeued in-flight write");
#endif /* CNET_HAVE_MKFIFO */

    /* A synchronous archive failure must not recycle HOT state. */
    {
        cce_kv_pager_opts of;
        cce_kv_pager *pf = NULL;
        char fdir[] = "kv_sync_fail_XXXXXX";
        char blocked_path[400];
        if (cnet_mkdtemp(fdir)) {
            snprintf(blocked_path, sizeof blocked_path,
                     "%s/page_000000.kvc", fdir);
            check(cnet_mkdir(blocked_path, 0700) == 0,
                  "storage failure: blocked page path created");
            cce_kv_pager_opts_default(&of, 4, 4, 256);
            of.page_len = 16;
            of.n_hot = 2;
            of.archive_dir = fdir;
            of.async = 0;
            of.quant_cold = 0;
            check(cce_kv_pager_open(&pf, &of) == CCE_OK,
                  "storage failure: pager open");
            if (pf) {
                float kf[4] = {1, 2, 3, 4}, vf[4] = {5, 6, 7, 8};
                int fill_ok = 1;
                for (t = 0; t < 32; ++t)
                    if (cce_kv_pager_write(pf, t, kf, vf) != 0) fill_ok = 0;
                check(fill_ok, "storage failure: initial HOT window filled");
                check(cce_kv_pager_write(pf, 32, kf, vf) != 0,
                      "storage failure: slide reports COLD write failure");
                check(cce_kv_pager_window_start(pf) == 0 &&
                          cce_kv_pager_k_row(pf, 0) != NULL,
                      "storage failure: HOT window is preserved");
                check(cce_kv_pager_write_failures(pf) == 1,
                      "storage failure: failure telemetry increments");
                cce_kv_pager_close(pf);
            }
            rmdir(blocked_path);
            snprintf(blocked_path, sizeof blocked_path, "%s/ledger.tsv", fdir);
            unlink(blocked_path);
            rmdir(fdir);
        }
    }

    /* f32 cold path (CNET_KV_QUANT=0 style) */
    {
        cce_kv_pager_opts o2;
        cce_kv_pager *p2 = NULL;
        char dir2[] = "kv_archive_f32_XXXXXX";
        if (cnet_mkdtemp(dir2)) {
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

    /* ---- Regression RED-then-GREEN tests for 2026-07-19 audit ---- */

    /* [HIGH] verify_cold must re-hash the body and compare against the
     * stored digest — not merely parse the header. A flipped body byte
     * MUST be detected. We craft a tiny quant cold page, flip one body
     * byte, and assert verify_cold rejects it. */
    {
        cce_kv_pager_opts ov;
        cce_kv_pager *pv = NULL;
        char vdir[] = "kv_audit_v_XXXXXX";
        if (cnet_mkdtemp(vdir)) {
            cce_kv_pager_opts_default(&ov, 4, 4, 256);
            ov.page_len = 16;
            ov.n_hot = 2;
            ov.archive_dir = vdir;
            ov.async = 0;          /* sync so file exists when we verify */
            ov.quant_cold = 0;      /* f32 → body bytes == floats, easy flip */
            ov.rehydrate = 1;
            check(cce_kv_pager_open(&pv, &ov) == CCE_OK, "audit: vhash open");
            if (pv) {
                int rr;
                float kf[4] = {1, 2, 3, 4}, vf[4] = {5, 6, 7, 8};
                char path[400];
                FILE *vf_f;
                long fsz;
                uint64_t dig_stored = 0;
                /* Write 33 positions so pos 32 slides page 0 to cold. */
                int okw = 1;
                for (t = 0; t < 33; ++t) {
                    if (cce_kv_pager_write(pv, t, kf, vf) != 0) okw = 0;
                }
                cce_kv_pager_sync(pv);
                check(okw && cce_kv_pager_pages_flushed(pv) >= 1,
                      "audit: vhash cold page written");
                /* Parse digest from the file header (mirrors verify_cold
                 * header parsing) so we can pass expect_digest below. */
                snprintf(path, sizeof path, "%s/page_000000.kvc", vdir);
                vf_f = fopen(path, "rb");
                check(vf_f != NULL, "audit: vhash cold file exists");
                if (vf_f) {
                    uint32_t magic, ver; int pid, lo, hi, ks, vs, quant;
                    uint64_t dig;
                    if (fread(&magic, 4, 1, vf_f) == 1 &&
                        fread(&ver, 4, 1, vf_f) == 1 &&
                        fread(&pid, 4, 1, vf_f) == 1 &&
                        fread(&lo, 4, 1, vf_f) == 1 &&
                        fread(&hi, 4, 1, vf_f) == 1 &&
                        fread(&ks, 4, 1, vf_f) == 1 &&
                        fread(&vs, 4, 1, vf_f) == 1 &&
                        fread(&quant, 4, 1, vf_f) == 1 &&
                        fread(&dig, 8, 1, vf_f) == 1) {
                        dig_stored = dig;
                    }
                    fclose(vf_f);
                }
                /* verify_cold with the correct digest must pass. */
                rr = cce_kv_pager_verify_cold(pv, 0, dig_stored);
                check(rr == 0, "audit: vhash verify_cold ok w/ digest");
                /* Now flip a body byte and assert verify_cold detects it. */
                snprintf(path, sizeof path, "%s/page_000000.kvc", vdir);
                vf_f = fopen(path, "r+b");
                check(vf_f != NULL, "audit: vhash reopen for flip");
                if (vf_f) {
                    fseek(vf_f, 0, SEEK_END);
                    fsz = ftell(vf_f);
                    /* Header for f32 (CVK2 quant=0): 4+4+4+4+4+4+4+4 +8 dig
                     * = 40 bytes; body starts at offset 40. Flip last byte. */
                    if (fsz > 40) {
                        unsigned char b;
                        fseek(vf_f, fsz - 1, SEEK_SET);
                        if (fread(&b, 1, 1, vf_f) == 1) {
                            b ^= 0x01u;
                            fseek(vf_f, fsz - 1, SEEK_SET);
                            fwrite(&b, 1, 1, vf_f);
                        }
                    }
                    fclose(vf_f);
                }
                rr = cce_kv_pager_verify_cold(pv, 0, dig_stored);
                check(rr != 0,
                      "audit: vhash verify_cold REJECTS flipped body byte");
                check(cce_kv_pager_rehydrate_pos(pv, 0) != 0,
                      "audit: rehydrate REJECTS flipped body byte");
                cce_kv_pager_close(pv);
            }
            {
                char path[400];
                snprintf(path, sizeof path, "%s/ledger.tsv", vdir);
                unlink(path);
                for (t = 0; t < 8; ++t) {
                    snprintf(path, sizeof path, "%s/page_%06d.kvc", vdir, t);
                    unlink(path);
                }
                rmdir(vdir);
            }
        }
    }

    /* [MED] Concurrent raw-row safety: an acquired row holds a read lease.
     * A writer that must slide the ring blocks until release, and the reader
     * observes stable storage throughout the overlap. */
    {
        cce_kv_pager_opts oc;
        cce_kv_pager *pc = NULL;
        char cdir[] = "kv_audit_c_XXXXXX";
        if (cnet_mkdtemp(cdir)) {
            cce_kv_pager_opts_default(&oc, 4, 4, 256);
            oc.page_len = 16;
            oc.n_hot = 2;
            oc.archive_dir = cdir;
            oc.async = 0;
            oc.quant_cold = 0;
            oc.rehydrate = 0;
            check(cce_kv_pager_open(&pc, &oc) == CCE_OK, "audit: conc open");
            if (pc) {
                slide_writer_ctx ctx;
                pthread_t writer;
                float *kr0;
                int created;
                memset(&ctx, 0, sizeof ctx);
                ctx.pager = pc;
                ctx.k[0] = 1.f; ctx.k[1] = 2.f; ctx.k[2] = 3.f; ctx.k[3] = 4.f;
                ctx.v[0] = 5.f; ctx.v[1] = 6.f; ctx.v[2] = 7.f; ctx.v[3] = 8.f;
                atomic_init(&ctx.started, 0);
                atomic_init(&ctx.done, 0);
                for (t = 0; t < 32; ++t)
                    cce_kv_pager_write(pc, t, ctx.k, ctx.v);
                kr0 = cce_kv_pager_k_row_acquire(pc, 0);
                check(kr0 != NULL && kr0[0] == 1.f,
                      "audit: acquired row0 valid");
                check(cce_kv_pager_reader_count(pc) == 1,
                      "audit: acquired row increments reader count");
                created = pthread_create(&writer, NULL, slide_writer, &ctx) == 0;
                check(created, "audit: concurrent slide writer started");
                if (created) {
                    while (!atomic_load(&ctx.started)) usleep(1000);
                    usleep(50000);
                    check(!atomic_load(&ctx.done),
                          "audit: slide blocks while row lease is held");
                    check(kr0[0] == 1.f && kr0[3] == 4.f,
                          "audit: acquired row remains stable during blocked slide");
                    cce_kv_pager_row_release(pc, kr0);
                    pthread_join(writer, NULL);
                    check(ctx.rc == 0 && atomic_load(&ctx.done),
                          "audit: slide completes after row release");
                    check(cce_kv_pager_reader_count(pc) == 0,
                          "audit: reader count returns to zero");
                    check(cce_kv_pager_k_row(pc, 0) == NULL,
                          "audit: old position cold after completed slide");
                } else if (kr0) {
                    cce_kv_pager_row_release(pc, kr0);
                }
                cce_kv_pager_close(pc);
            }
            {
                char path[400];
                snprintf(path, sizeof path, "%s/ledger.tsv", cdir);
                unlink(path);
                for (t = 0; t < 8; ++t) {
                    snprintf(path, sizeof path, "%s/page_%06d.kvc", cdir, t);
                    unlink(path);
                }
                rmdir(cdir);
            }
        }
    }

    printf("KV_PAGE_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
