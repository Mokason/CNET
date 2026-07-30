/* Fresh-runtime specialist runner.
 *
 * Imports ONE capsule and detects cars in full JPEG images using nothing else:
 * no vision cache, no PCA file, no training artefact, no source-tree state. The
 * proposal rule, descriptor, projection basis, class map, thresholds and gate
 * constants all come out of the capsule's bound frontend asset; the weights and
 * typed contract come out of its payload; the coverage reference rows come out
 * of its coverage section.
 *
 * If this runs, the specialist is transferable. If it needs anything outside the
 * package, it is not.
 */
#include <opencv2/opencv.hpp>
#include <opencv2/ximgproc/segmentation.hpp>

/* The protocol this runtime implements. An asset naming a different one is
   refused rather than reinterpreted -- identity comes from the binary, never
   from the artifact describing itself. */
#define VD_RUNNER_PROTOCOL "cnet_vision_v2_20260727"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "vd_coverage.h"
#include "vd_eval.h"
#include "vd_frontend.h"
#include "vd_pack.h"
#include "../../include/base.h"
#include "../../include/cnet_capsule.h"
#include "../../include/hybrid_ai.h"
#include "../../include/nn.h"
#include "../../include/contract/contract.h"
}

static uint64_t fnv1a(const std::string &s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ULL; }
    return h;
}
static uint64_t splitmix(uint64_t &x) {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int main(int argc, char **argv) {
    const char *capdir = NULL, *vocroot = NULL, *idsfile = NULL, *gtpack = NULL;
    int use_gate = 0, limit = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--capsule" && i + 1 < argc) capdir = argv[++i];
        else if (a == "--voc-root" && i + 1 < argc) vocroot = argv[++i];
        else if (a == "--ids" && i + 1 < argc) idsfile = argv[++i];
        else if (a == "--gt-pack" && i + 1 < argc) gtpack = argv[++i];
        else if (a == "--gate") use_gate = 1;
        else if (a == "--limit" && i + 1 < argc) limit = atoi(argv[++i]);
    }
    if (!capdir) { printf("VD_RUNNER_FAIL need_capsule\n"); return 2; }
    printf("vd_runner: CPU-only (ROCR/HIP/CUDA_VISIBLE_DEVICES empty)\n");

    /* ---- import the specialist; nothing else is consulted ---------------- */
    CnetBase base; HybridAi cov; CnetCapsuleReport rep;
    void *asset = NULL; size_t alen = 0; unsigned aschema = 0;
    cnb_init(&base);
    memset(&cov, 0, sizeof cov);
    if (cnet_capsule_import_asset(&base, &cov, capdir, &asset, &alen, &aschema, &rep) != 0) {
        printf("VD_RUNNER_REFUSED reason=%s\n", rep.reject_reason);
        return 3;
    }
    /* Validate EVERY field before any of them is used: the header used to be
       trusted after a magic/schema check, and pca_dim then drove a loop into a
       fixed double[512] while hog_dim*pca_dim was multiplied unchecked and cast
       to OpenCV int. vd_frontend_validate is a standalone C unit precisely so
       tests/test_vd_frontend_parse.c can mutate it under ASan/UBSan. */
    VdFrontendHdr h;
    size_t nfloat = 0;
    if (const char *bad = vd_frontend_validate(asset, alen, &h, &nfloat)) {
        printf("VD_RUNNER_REFUSED reason=frontend_%s\n", bad);
        return 3;
    }
    if (const char *bad = vd_frontend_check_protocol(&h, VD_RUNNER_PROTOCOL)) {
        printf("VD_RUNNER_REFUSED reason=frontend_%s\n", bad);
        return 3;
    }
    /* Only now is h.extractor known to be a terminated string. */
    printf("imported unit=%s schema=%u asset=%zu coverage_rows=%zu extractor=%s\n",
           rep.unit, rep.schema, alen, rep.coverage_rows, h.extractor);

    /* ---- rebuild the frontend from the asset ----------------------------- */
    const float *fl = (const float *)((const unsigned char *)asset + sizeof h);
    cv::PCA pca;
    {
        cv::Mat mean(1, (int)h.hog_dim, CV_32F), ev((int)h.pca_dim, (int)h.hog_dim, CV_32F);
        memcpy(mean.ptr<float>(0), fl, sizeof(float) * h.hog_dim);
        for (unsigned r = 0; r < h.pca_dim; r++)
            memcpy(ev.ptr<float>((int)r), fl + h.hog_dim + (size_t)r * h.hog_dim,
                   sizeof(float) * h.hog_dim);
        pca.mean = mean; pca.eigenvectors = ev;
    }
    cv::HOGDescriptor hog(cv::Size((int)h.hog_side, (int)h.hog_side), cv::Size(16,16),
                          cv::Size(8,8), cv::Size(8,8), 9);

    /* ---- head + gate ------------------------------------------------------ */
    BinaryTransformNetwork btn; Contract ct;
    memset(&btn, 0, sizeof btn); memset(&ct, 0, sizeof ct);
    if (cnb_get_unit(&base, rep.unit, &btn, &ct) != 0) { printf("VD_RUNNER_FAIL get_unit\n"); return 3; }
    /* The frontend must describe the head that arrived with it. Without this a
       checksum-valid capsule could project into one width and feed a head
       certified on another -- a confident function of the wrong input. */
    if (const char *bad = vd_frontend_check_contract(&h, btn.input_count,
                                                     btn.output_count)) {
        printf("VD_RUNNER_REFUSED reason=%s\n", bad);
        return 3;
    }
    VdCoverage gate; memset(&gate, 0, sizeof gate);
    if (use_gate) {
        const HybridCoverage *hc = NULL;
        for (size_t i = 0; i < HYBRID_COVERAGE_MAX; i++)
            if (cov.coverage[i].n_rows && strcmp(cov.coverage[i].unit, rep.unit) == 0) {
                hc = &cov.coverage[i]; break;
            }
        if (!hc) { printf("VD_RUNNER_FAIL no_coverage_rows\n"); return 3; }
        if (vd_cov_init(&gate, hc->rows, hc->n_rows, hc->in_dim, h.gate_k, h.gate_tau) != 0) {
            printf("VD_RUNNER_FAIL gate_init\n"); return 3;
        }
        printf("gate: rows=%zu k=%u tau=%.9f\n", hc->n_rows, h.gate_k, h.gate_tau);
    }

    if (!vocroot || !idsfile) { printf("VD_RUNNER_IMPORT_OK\n"); return 0; }

    /* ---- inference from JPEG --------------------------------------------- */
    std::vector<std::string> ids;
    { std::ifstream f(idsfile); std::string t; while (f >> t) ids.push_back(t); }
    if (limit > 0 && (int)ids.size() > limit) ids.resize(limit);

    VdPack gp; memset(&gp, 0, sizeof gp);
    if (gtpack && vd_pack_load(gtpack, &gp) != VD_PACK_OK) { printf("VD_RUNNER_FAIL gt_pack\n"); return 3; }

    std::vector<VdImage> imgs(ids.size());
    std::vector<std::vector<VdDet> > keep(ids.size());
    size_t total_prop = 0, admitted = 0, ndet = 0;
    for (size_t i = 0; i < ids.size(); i++) {
        cv::setNumThreads(1);
        cv::Mat img = cv::imread(std::string(vocroot) + "/JPEGImages/" + ids[i] + ".jpg");
        if (img.empty()) { printf("VD_RUNNER_FAIL image:%s\n", ids[i].c_str()); return 4; }
        double sc = (double)h.ss_width / img.cols;
        cv::Mat small; cv::resize(img, small, cv::Size(), sc, sc);
        auto ss = cv::ximgproc::segmentation::createSelectiveSearchSegmentation();
        ss->setBaseImage(small); ss->switchToSelectiveSearchFast();
        std::vector<cv::Rect> rects; ss->process(rects);
        auto canon = [](const cv::Rect &a, const cv::Rect &b) {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            if (a.width != b.width) return a.width < b.width;
            return a.height < b.height; };
        std::sort(rects.begin(), rects.end(), canon);
        if ((int)rects.size() > (int)h.max_prop) {
            uint64_t st = fnv1a(ids[i]) ^ 20260727ULL;
            for (size_t q = rects.size(); q > 1; q--)
                std::swap(rects[q-1], rects[(size_t)(splitmix(st) % q)]);
            rects.resize(h.max_prop);
            std::sort(rects.begin(), rects.end(), canon);
        }
        std::vector<VdDet> raw;
        for (size_t r = 0; r < rects.size(); r++) {
            int bx = (int)(rects[r].x / sc), by = (int)(rects[r].y / sc);
            int bw = (int)(rects[r].width / sc), bh = (int)(rects[r].height / sc);
            if (bw < (int)h.min_side || bh < (int)h.min_side) continue;
            cv::Rect rr(bx, by, bw, bh);
            rr &= cv::Rect(0, 0, img.cols, img.rows);
            if (rr.width < 8 || rr.height < 8) continue;
            cv::Mat crop = img(rr), res;
            cv::resize(crop, res, cv::Size((int)h.hog_side, (int)h.hog_side));
            std::vector<float> desc;
            hog.compute(res, desc);
            if (desc.size() != h.hog_dim) continue;
            cv::Mat in(1, (int)h.hog_dim, CV_32F, desc.data()), prj;
            pca.project(in, prj);
            double x[VD_FRONTEND_MAX_PCA_DIM];  /* pca_dim is validated <= this */
            for (unsigned d = 0; d < h.pca_dim; d++) x[d] = prj.ptr<float>(0)[d];
            total_prop++;
            if (use_gate && !vd_cov_admit(&gate, x)) continue;   /* abstain */
            admitted++;
            const double *o = btn_forward(&btn, x);
            if (!o) continue;
            double a = o[0], b = o[1], m = a > b ? a : b;
            double ea = exp(a - m), eb = exp(b - m), s = eb / (ea + eb);
            VdDet d0; d0.box.x = rr.x; d0.box.y = rr.y; d0.box.w = rr.width; d0.box.h = rr.height;
            d0.score = s; d0.cls = 0;
            raw.push_back(d0);
        }
        std::vector<int> kidx(raw.size() ? raw.size() : 1);
        size_t nk = raw.size() ? vd_nms(raw.data(), raw.size(),
                                        h.nms_iou_x100 / 100.0, kidx.data()) : 0;
        if (nk == VD_NMS_FAIL) { printf("VD_RUNNER_FAIL nms\n"); return 4; }
        for (size_t q = 0; q < nk; q++) keep[i].push_back(raw[kidx[q]]);
        ndet += nk;
        imgs[i].dets = keep[i].data(); imgs[i].n_det = nk;
        imgs[i].gts = NULL; imgs[i].gt_difficult = NULL; imgs[i].n_gt = 0;
        if (gtpack) {
            for (size_t g = 0; g < gp.n; g++)
                if (ids[i] == gp.imgs[g].id) {
                    imgs[i].gts = gp.imgs[g].gts;
                    imgs[i].gt_difficult = gp.imgs[g].gt_dif;
                    imgs[i].n_gt = gp.imgs[g].n_gt;
                    break;
                }
        }
    }
    printf("VD_RUNNER_INFER images=%zu proposals=%zu admitted=%zu detections=%zu gate=%d\n",
           ids.size(), total_prop, admitted, ndet, use_gate);
    if (gtpack) {
        double ap = vd_ap50(imgs.data(), imgs.size(), h.match_iou_x100 / 100.0);
        double pr = 0, rc = 0;
        vd_pr_at(imgs.data(), imgs.size(), h.match_iou_x100 / 100.0,
                 h.score_thr_x100 / 100.0, &pr, &rc);
        printf("VD_RUNNER_AP ap50=%.6f precision=%.6f recall=%.6f thr=%.2f\n",
               ap, pr, rc, h.score_thr_x100 / 100.0);
    }
    /* stable per-detection dump so a replay can be compared exactly */
    for (size_t i = 0; i < ids.size(); i++)
        for (size_t q = 0; q < keep[i].size(); q++)
            printf("DET %s %d %d %d %d %.17g\n", ids[i].c_str(), keep[i][q].box.x,
                   keep[i][q].box.y, keep[i][q].box.w, keep[i][q].box.h, keep[i][q].score);
    printf("VD_RUNNER_DONE\n");
    if (use_gate) vd_cov_free(&gate);
    free(asset);
    btn_free(&btn); contract_free(&ct);
    vd_pack_free(&gp);
    hybrid_ai_free(&cov);
    cnb_free(&base);
    return 0;
}
