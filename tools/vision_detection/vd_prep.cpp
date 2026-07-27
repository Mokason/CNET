/* vd_prep — VOC2007 -> class-agnostic proposals -> fixed HOG -> train-only PCA.
 *
 * Emits binary feature packs consumed by the CNET head trainer/evaluator.
 * Nothing here is learned except the PCA basis, which is fitted on TRAIN ONLY
 * and then frozen (mean + eigenvectors written once, reused verbatim).
 *
 * CPU only; cv::setNumThreads(1) per worker, parallelism across images.
 *
 * Protocol: plans/cnet_vision_object_detection_20260727.md
 */
#include "vd_io.h"
#include "vd_pack.h"
#include "vd_sha256.h"

#include <opencv2/opencv.hpp>
#include <opencv2/ximgproc/segmentation.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <thread>
#include <vector>

static const int SS_WIDTH   = 300;
static const int MAX_PROP   = 300;   /* per image, after SS ordering */
static const uint64_t SEED  = 20260727ULL;

/* Variant table. V1 is retained so its configuration stays readable and
   runnable; it does NOT reproduce commit 6ffc5f1 byte-for-byte, because the
   proposal-selection determinism repair (see protocol section 3b) changed which
   300 candidates are kept. The V1 artefact and verdict are frozen in that
   commit as the V1 record; this variant is its configuration, not its output.
   V2 changes the feature description and the holdout slice.
   Protocol: plans/cnet_vision_object_detection_v2_20260727.md */
struct Variant {
    const char *name;
    int hog_side;      /* crop is resized to hog_side x hog_side */
    int color;         /* 1 = HOG on BGR (max-magnitude channel gradient), 0 = gray */
    int hog_dim;
    int pca_dim;
    int test_offset;   /* slice into the seed-20260727 shuffle of the 4952 test IDs */
    int pca_fit_images;/* 0 = fit on every train image; else first N (memory bound) */
};
static const Variant V1 = {"v1", 32, 0,  324,  64,    0,   0};
static const Variant V2 = {"v2", 64, 1, 1764, 256, 1000, 600};
static Variant VAR = V1;

struct Box { int x, y, w, h; };
struct GT  { Box b; int difficult; };

struct Prop {
    Box b;
    int label;        /* 1 = car (IoU>=0.5 with non-difficult GT), 0 = bg, -1 = ignore */
    std::vector<float> feat;   /* PCA-projected after phase B */
};

/* ---------- deterministic helpers ---------------------------------------- */
static uint64_t fnv1a(const std::string &s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static uint64_t splitmix(uint64_t &x) {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static std::string sha256_file(const std::string &p) {
    char hex[65];
    if (vd_sha256_file(p.c_str(), hex) != 0) return "";
    return std::string(hex, 64);
}

/* ---------- VOC annotation parsing (fails loud on malformed) ------------- */
static bool tag_val(const std::string &s, const char *tag, std::string &out) {
    std::string a = std::string("<") + tag + ">", b = std::string("</") + tag + ">";
    size_t i = s.find(a); if (i == std::string::npos) return false;
    size_t j = s.find(b, i); if (j == std::string::npos) return false;
    out = s.substr(i + a.size(), j - i - a.size());
    return true;
}

/* strtol with full-consumption and range checking: atoi silently returns 0 for
   garbage, which would turn a malformed annotation into a plausible box. */
static bool parse_int(const std::string &t, long lo, long hi, long &out) {
    if (t.empty() || t.size() > 24) return false;
    errno = 0;
    char *end = NULL;
    long v = strtol(t.c_str(), &end, 10);
    if (errno == ERANGE || end == t.c_str()) return false;
    while (end && *end && isspace((unsigned char)*end)) end++;
    if (end && *end) return false;
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

static const size_t MAX_ANN_BYTES = 1u << 20;   /* 1 MiB; VOC files are ~1 KiB */

static bool parse_ann(const std::string &path, const std::string &cls,
                      std::vector<GT> &gts, int img_w, int img_h, std::string &err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "annotation_missing:" + path; return false; }
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz < 0) { err = "annotation_unreadable:" + path; return false; }
    if ((size_t)sz > MAX_ANN_BYTES) { err = "annotation_too_large:" + path; return false; }
    f.seekg(0, std::ios::beg);
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    if (s.find("<annotation>") == std::string::npos) { err = "annotation_malformed:" + path; return false; }
    size_t pos = 0;
    while ((pos = s.find("<object>", pos)) != std::string::npos) {
        size_t end = s.find("</object>", pos);
        if (end == std::string::npos) { err = "object_unterminated:" + path; return false; }
        std::string ob = s.substr(pos, end - pos);
        std::string name, dif, xs, ys, xe, ye;
        if (!tag_val(ob, "name", name)) { err = "object_no_name:" + path; return false; }
        if (name == cls) {
            if (!tag_val(ob, "xmin", xs) || !tag_val(ob, "ymin", ys) ||
                !tag_val(ob, "xmax", xe) || !tag_val(ob, "ymax", ye)) {
                err = "object_no_bndbox:" + path; return false;
            }
            GT g;
            long d = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (tag_val(ob, "difficult", dif)) {
                if (!parse_int(dif, 0, 1, d)) { err = "object_bad_difficult:" + path; return false; }
            }
            g.difficult = (int)d;
            if (!parse_int(xs, 0, VD_MAX_COORD, x1) || !parse_int(ys, 0, VD_MAX_COORD, y1) ||
                !parse_int(xe, 0, VD_MAX_COORD, x2) || !parse_int(ye, 0, VD_MAX_COORD, y2)) {
                err = "object_bad_coord:" + path; return false;
            }
            if (x2 <= x1 || y2 <= y1) { err = "object_bad_box:" + path; return false; }
            /* the box must lie inside the image that was actually decoded */
            if (x2 > img_w || y2 > img_h) { err = "object_box_outside_image:" + path; return false; }
            g.b.x = x1; g.b.y = y1; g.b.w = x2 - x1; g.b.h = y2 - y1;
            gts.push_back(g);
        }
        pos = end + 9;
    }
    return true;
}

static double iou(const Box &a, const Box &b) {
    int x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    int iw = x2 - x1, ih = y2 - y1;
    if (iw <= 0 || ih <= 0) return 0.0;
    double in = (double)iw * ih;
    return in / ((double)a.w * a.h + (double)b.w * b.h - in);
}

/* ---------- fixed HOG ----------------------------------------------------
   V1: 32x32 grayscale -> 324-D.  V2: 64x64 colour (BGR) -> 1764-D. OpenCV's
   HOG takes the per-pixel maximum-magnitude channel gradient on a 3-channel
   input, so the colour path is genuinely different data at the same dimension
   as a grayscale 64x64 descriptor (probe: logs/vision/v2_hog_probe.log). */
static cv::HOGDescriptor &hog() {
    static cv::HOGDescriptor h(cv::Size(VAR.hog_side, VAR.hog_side), cv::Size(16, 16),
                               cv::Size(8, 8), cv::Size(8, 8), 9);
    return h;
}

static bool hog_of(const cv::Mat &img, const Box &b, std::vector<float> &out) {
    cv::Rect r(b.x, b.y, b.w, b.h);
    r &= cv::Rect(0, 0, img.cols, img.rows);
    if (r.width < 8 || r.height < 8) return false;
    cv::Mat crop = img(r), src, res;
    if (VAR.color) src = crop;
    else cv::cvtColor(crop, src, cv::COLOR_BGR2GRAY);
    cv::resize(src, res, cv::Size(VAR.hog_side, VAR.hog_side));
    hog().compute(res, out);
    return (int)out.size() == VAR.hog_dim;
}

/* ---------- per-image worker --------------------------------------------- */
struct ImgOut {
    std::string id, sha;
    std::vector<Prop> props;
    std::vector<GT> gts;
    std::vector<std::vector<float>> raw;  /* phase A, PCA-fit subset only */
    int ok;
    std::string err;
};

/* Phase A: Selective Search + boxes + labels. HOG is always computed so that
   proposal filtering is identical everywhere, but the raw descriptor is only
   retained for the PCA-fit subset -- at 1764-D, keeping every train proposal
   resident would cost ~7.4 GB before the PCA solve's own copy. */
static void do_image(const std::string &root, const std::string &id,
                     const std::string &cls, bool label_props, bool keep_raw,
                     ImgOut &o) {
    cv::setNumThreads(1);
    o.id = id; o.ok = 0;
    std::string jp = root + "/JPEGImages/" + id + ".jpg";
    std::string ap = root + "/Annotations/" + id + ".xml";
    o.sha = sha256_file(jp);
    if (o.sha.empty()) { o.err = "image_missing:" + jp; return; }
    cv::Mat img = cv::imread(jp);
    if (img.empty()) { o.err = "image_corrupt:" + jp; return; }
    if (!parse_ann(ap, cls, o.gts, img.cols, img.rows, o.err)) return;

    double sc = (double)SS_WIDTH / img.cols;
    cv::Mat small;
    cv::resize(img, small, cv::Size(), sc, sc);
    auto ss = cv::ximgproc::segmentation::createSelectiveSearchSegmentation();
    ss->setBaseImage(small);
    ss->switchToSelectiveSearchFast();
    std::vector<cv::Rect> rects;
    ss->process(rects);

    /* OpenCV's selective search returns a deterministic candidate SET in a
       NONDETERMINISTIC ORDER -- it perturbs region priority with the global
       rand(), so the sequence changes run to run and with worker count.
       Measured over 30 images: set identical 30/30, sequence differs 30/30,
       and the naive "first MAX_PROP" truncation therefore selected a different
       subset on 29/30. Canonicalise the order, then subsample deterministically
       from a per-image seed, so the kept proposals depend only on the image. */
    auto canon = [](const cv::Rect &a, const cv::Rect &b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.width != b.width) return a.width < b.width;
        return a.height < b.height;
    };
    std::sort(rects.begin(), rects.end(), canon);
    if ((int)rects.size() > MAX_PROP) {
        uint64_t st = fnv1a(id) ^ SEED;
        for (size_t i = rects.size(); i > 1; i--)
            std::swap(rects[i - 1], rects[(size_t)(splitmix(st) % i)]);
        rects.resize(MAX_PROP);
        std::sort(rects.begin(), rects.end(), canon);
    }

    int n = (int)rects.size();
    for (int i = 0; i < n; i++) {
        Prop p;
        std::vector<float> f;
        p.b.x = (int)(rects[i].x / sc); p.b.y = (int)(rects[i].y / sc);
        p.b.w = (int)(rects[i].width / sc); p.b.h = (int)(rects[i].height / sc);
        if (p.b.w < 16 || p.b.h < 16) continue;
        if (!hog_of(img, p.b, f)) continue;
        p.label = 0;
        if (label_props) {
            double best = 0.0; int best_dif = 0;
            for (const GT &g : o.gts) {
                double v = iou(p.b, g.b);
                if (v > best) { best = v; best_dif = g.difficult; }
            }
            if (best >= 0.5) p.label = best_dif ? -1 : 1;
            else if (best >= 0.3) p.label = -1;      /* ambiguous zone: ignore */
        }
        o.props.push_back(p);
        if (keep_raw) o.raw.push_back(f);
    }
    o.ok = 1;
}

/* Phase B: recompute HOG for the boxes phase A already fixed, then project
   through the frozen train-only PCA. Boxes are reused verbatim, so the data the
   basis was fitted on and the data it is applied to line up by construction. */
static void project_image(const std::string &root, const std::string &id,
                          const cv::PCA &pca, ImgOut &o) {
    cv::setNumThreads(1);
    cv::Mat img;
    if (!o.raw.empty() && o.raw.size() != o.props.size()) { o.ok = 0; o.err = "raw_prop_mismatch:" + id; return; }
    if (o.raw.empty() && !o.props.empty()) {
        img = cv::imread(root + "/JPEGImages/" + id + ".jpg");
        if (img.empty()) { o.ok = 0; o.err = "image_corrupt:" + id; return; }
    }
    for (size_t i = 0; i < o.props.size(); i++) {
        std::vector<float> f;
        if (!o.raw.empty()) f = o.raw[i];
        else if (!hog_of(img, o.props[i].b, f)) { o.ok = 0; o.err = "hog_replay_failed:" + id; return; }
        cv::Mat in(1, VAR.hog_dim, CV_32F, f.data()), pr;
        pca.project(in, pr);
        o.props[i].feat.assign(pr.ptr<float>(0), pr.ptr<float>(0) + VAR.pca_dim);
    }
    std::vector<std::vector<float>>().swap(o.raw);   /* release phase-A memory */
}

/* ---------- pack IO ------------------------------------------------------ */
struct PackHdr { char magic[8]; int32_t dim, n_img; int64_t n_prop; };

/* Image IDs of an existing pack, via the shared validating reader: a file the
   bench would refuse must not be usable as disjointness evidence here. */
static std::vector<std::string> read_pack_ids(const std::string &path, int &rc) {
    std::vector<std::string> out;
    char **ids = NULL;
    size_t n = 0;
    rc = vd_pack_read_ids(path.c_str(), &ids, &n);
    if (rc != VD_PACK_OK) return out;
    for (size_t i = 0; i < n; i++) out.push_back(std::string(ids[i]));
    vd_pack_free_ids(ids, n);
    return out;
}

static void write_pack(const std::string &path, const std::vector<ImgOut> &imgs, int dim) {
    std::ofstream f(path, std::ios::binary);
    PackHdr h; memcpy(h.magic, "VDPACK1", 8);
    h.dim = dim; h.n_img = (int32_t)imgs.size(); h.n_prop = 0;
    for (auto &im : imgs) h.n_prop += (int64_t)im.props.size();
    f.write((char*)&h, sizeof h);
    for (size_t i = 0; i < imgs.size(); i++) {
        const ImgOut &im = imgs[i];
        int32_t np = (int32_t)im.props.size(), ng = (int32_t)im.gts.size();
        int32_t idlen = (int32_t)im.id.size();
        f.write((char*)&idlen, 4); f.write(im.id.data(), idlen);
        f.write((char*)&np, 4); f.write((char*)&ng, 4);
        for (const GT &g : im.gts) {
            int32_t v[5] = {g.b.x, g.b.y, g.b.w, g.b.h, g.difficult};
            f.write((char*)v, sizeof v);
        }
        for (const Prop &p : im.props) {
            int32_t v[5] = {p.b.x, p.b.y, p.b.w, p.b.h, p.label};
            f.write((char*)v, sizeof v);
            f.write((char*)p.feat.data(), sizeof(float) * dim);
        }
    }
}

int main(int argc, char **argv) {
    std::string root, cls = "car", outdir = "data/vision_cache", v1cache;
    int n_test = 1000, workers = 8;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--root" && i + 1 < argc) root = argv[++i];
        else if (a == "--class" && i + 1 < argc) cls = argv[++i];
        else if (a == "--out" && i + 1 < argc) outdir = argv[++i];
        else if (a == "--ntest" && i + 1 < argc) n_test = atoi(argv[++i]);
        else if (a == "--workers" && i + 1 < argc) workers = atoi(argv[++i]);
        else if (a == "--v1cache" && i + 1 < argc) v1cache = argv[++i];
        else if (a == "--variant" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "v1") VAR = V1;
            else if (v == "v2") VAR = V2;
            else { fprintf(stderr, "VD_PREP_FAIL unknown_variant:%s\n", v.c_str()); return 2; }
        }
    }
    if (root.empty()) { fprintf(stderr, "VD_PREP_FAIL need --root\n"); return 2; }
    fprintf(stderr, "vd_prep: CPU-only (ROCR/HIP/CUDA_VISIBLE_DEVICES empty), workers=%d\n", workers);
    fprintf(stderr, "variant=%s hog=%dx%d %s raw_dim=%d pca_dim=%d test_slice=[%d,%d)\n",
            VAR.name, VAR.hog_side, VAR.hog_side, VAR.color ? "colour" : "gray",
            VAR.hog_dim, VAR.pca_dim, VAR.test_offset, VAR.test_offset + n_test);

    auto read_ids = [&](const std::string &p) {
        std::vector<std::string> v; std::ifstream f(p); std::string s;
        while (f >> s) v.push_back(s);
        return v;
    };
    std::vector<std::string> trainval = read_ids(root + "/ImageSets/Main/trainval.txt");
    std::vector<std::string> testall  = read_ids(root + "/ImageSets/Main/test.txt");
    if (trainval.size() != 5011 || testall.size() != 4952) {
        fprintf(stderr, "VD_PREP_FAIL unexpected split sizes %zu/%zu\n", trainval.size(), testall.size());
        return 2;
    }

    /* Train/val split keyed on the image CONTENT hash, not the image ID.
       VOC2007 trainval contains 3 exact-duplicate image pairs under different
       IDs (008037<->009623, 000338<->007284, 000949<->005042), so an ID-based
       split leaks identical pixels across train and val. Hashing the content
       puts every copy in the same split by construction. Measured separately:
       0 duplicates inside test and 0 trainval<->test duplicates, so the
       official train/test separation is clean at content level. */
    std::vector<std::string> tr, va, te;
    {
        fprintf(stderr, "hashing trainval for content-addressed split...\n");
        std::vector<std::string> shas(trainval.size());
        std::vector<std::thread> th;
        std::atomic<size_t> next(0);
        for (int w = 0; w < workers; w++)
            th.emplace_back([&]() {
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= trainval.size()) break;
                    shas[i] = sha256_file(root + "/JPEGImages/" + trainval[i] + ".jpg");
                }
            });
        for (auto &t : th) t.join();
        for (size_t i = 0; i < trainval.size(); i++) {
            if (shas[i].empty()) {
                fprintf(stderr, "VD_PREP_FAIL image_missing:%s\n", trainval[i].c_str());
                return 4;
            }
            ((fnv1a(shas[i]) % 5 == 0) ? va : tr).push_back(trainval[i]);
        }
    }
    {
        std::vector<std::string> shuf = testall;
        uint64_t st = SEED;
        for (size_t i = shuf.size(); i > 1; i--) {
            size_t j = (size_t)(splitmix(st) % i);
            std::swap(shuf[i - 1], shuf[j]);
        }
        /* One fixed permutation of the official 4952. V1 spent [0,1000); V2 takes
           [1000,2000) from the 3952 V1 never saw; [2000,4952) stays unspent. */
        size_t lo = (size_t)VAR.test_offset, hi = lo + (size_t)n_test;
        if (hi > shuf.size()) { fprintf(stderr, "VD_PREP_FAIL test_slice_overflow\n"); return 2; }
        te.assign(shuf.begin() + lo, shuf.begin() + hi);
    }
    fprintf(stderr, "split train=%zu val=%zu test=%zu\n", tr.size(), va.size(), te.size());

    /* leakage: image IDs must be disjoint */
    {
        std::set<std::string> a(tr.begin(), tr.end()), b(va.begin(), va.end()), c(te.begin(), te.end());
        for (auto &x : b) if (a.count(x)) { fprintf(stderr, "VD_PREP_FAIL id_leak_train_val:%s\n", x.c_str()); return 3; }
        for (auto &x : c) if (a.count(x) || b.count(x)) { fprintf(stderr, "VD_PREP_FAIL id_leak_test:%s\n", x.c_str()); return 3; }
    }

    /* V1's holdout is spent. If a previous variant's cache is on disk, the new
       holdout must not intersect the images it actually scored -- checked by ID
       here (fail fast) and by content hash below, once the SHAs exist. */
    std::vector<std::string> v1_ids;
    size_t v1_id_checked = 0;
    std::string prev_pack_sha;
    if (VAR.test_offset > 0 && v1cache.empty()) {
        fprintf(stderr, "VD_PREP_FAIL variant_%s_requires_--v1cache\n", VAR.name);
        return 3;
    }
    if (!v1cache.empty()) {
        int rc = 0;
        std::string ppath = v1cache + "/test.pack";
        v1_ids = read_pack_ids(ppath, rc);
        if (rc != VD_PACK_OK) {
            fprintf(stderr, "VD_PREP_FAIL v1cache_invalid:%s (%s)\n",
                    ppath.c_str(), vd_pack_strerror(rc));
            return 3;
        }
        /* Partial evidence is not evidence: the spent holdout must be exactly
           the size the protocol says it is, with no duplicate IDs. */
        std::set<std::string> prev(v1_ids.begin(), v1_ids.end());
        if (v1_ids.size() != (size_t)n_test || prev.size() != (size_t)n_test) {
            fprintf(stderr, "VD_PREP_FAIL v1_evidence_incomplete ids=%zu unique=%zu expected=%d\n",
                    v1_ids.size(), prev.size(), n_test);
            return 3;
        }
        for (auto &x : te)
            if (prev.count(x)) { fprintf(stderr, "VD_PREP_FAIL v1_v2_test_id_leak:%s\n", x.c_str()); return 3; }
        v1_id_checked = v1_ids.size();
        prev_pack_sha = sha256_file(ppath);
        if (prev_pack_sha.empty()) { fprintf(stderr, "VD_PREP_FAIL v1cache_unhashable\n"); return 3; }
        fprintf(stderr, "disjoint: v2 holdout vs %zu scored v1 ids -> 0 id overlap\n", v1_id_checked);
    }

    /* first N train images carry the PCA-fit sample (see protocol §3) */
    std::set<std::string> fit_ids;
    {
        size_t nfit = VAR.pca_fit_images > 0 ? std::min((size_t)VAR.pca_fit_images, tr.size()) : tr.size();
        for (size_t i = 0; i < nfit; i++) fit_ids.insert(tr[i]);
    }

    auto run_split = [&](const std::vector<std::string> &ids, bool label_props,
                         std::vector<ImgOut> &out, bool raw_for_fit) {
        out.resize(ids.size());
        std::vector<std::thread> th;
        std::atomic<size_t> next(0);
        for (int w = 0; w < workers; w++)
            th.emplace_back([&]() {
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= ids.size()) break;
                    do_image(root, ids[i], cls, label_props,
                             raw_for_fit && fit_ids.count(ids[i]) != 0, out[i]);
                }
            });
        for (auto &t : th) t.join();
        for (auto &o : out)
            if (!o.ok) { fprintf(stderr, "VD_PREP_FAIL %s\n", o.err.c_str()); exit(4); }
    };

    std::vector<ImgOut> Otr, Ova, Ote;
    fprintf(stderr, "proposals: train...\n");  run_split(tr, true,  Otr, true);
    fprintf(stderr, "proposals: val...\n");    run_split(va, true,  Ova, false);
    fprintf(stderr, "proposals: test...\n");   run_split(te, false, Ote, false);

    /* content-level V1 vs V2 holdout check: a duplicate image under a different
       ID must not slip past the ID check above. */
    size_t v1_sha_checked = 0;
    if (!v1_ids.empty()) {
        std::set<std::string> prev;
        std::vector<std::string> vs(v1_ids.size());
        std::vector<std::thread> th;
        std::atomic<size_t> next(0);
        for (int w = 0; w < workers; w++)
            th.emplace_back([&]() {
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= v1_ids.size()) break;
                    vs[i] = sha256_file(root + "/JPEGImages/" + v1_ids[i] + ".jpg");
                }
            });
        for (auto &t : th) t.join();
        for (size_t q = 0; q < vs.size(); q++) {
            if (vs[q].empty()) {
                /* a silently dropped hash would weaken the content check to
                   "the files we happened to be able to read" */
                fprintf(stderr, "VD_PREP_FAIL v1_content_hash_missing:%s\n", v1_ids[q].c_str());
                return 3;
            }
            prev.insert(vs[q]);
        }
        if (prev.size() != v1_ids.size()) {
            fprintf(stderr, "VD_PREP_FAIL v1_content_hash_not_unique unique=%zu of %zu\n",
                    prev.size(), v1_ids.size());
            return 3;
        }
        for (auto &o : Ote)
            if (prev.count(o.sha)) {
                fprintf(stderr, "VD_PREP_FAIL v1_v2_test_content_leak:%s\n", o.id.c_str());
                return 3;
            }
        v1_sha_checked = prev.size();
        fprintf(stderr, "disjoint: v2 holdout vs %zu scored v1 content hashes -> 0 overlap\n", v1_sha_checked);
    }

    /* leakage: content hashes must be disjoint across splits */
    {
        std::map<std::string, std::string> seen;
        auto add = [&](std::vector<ImgOut> &v, const char *tag) {
            for (auto &o : v) {
                auto it = seen.find(o.sha);
                if (it != seen.end() && it->second != tag) {
                    fprintf(stderr, "VD_PREP_FAIL content_hash_leak %s in %s and %s\n",
                            o.id.c_str(), it->second.c_str(), tag);
                    exit(3);
                }
                seen[o.sha] = tag;
            }
        };
        add(Otr, "train"); add(Ova, "val"); add(Ote, "test");
    }

    /* PCA fitted on TRAIN ONLY, on the images reserved for it in the protocol */
    size_t nfit_rows = 0, nfit_img = 0;
    for (auto &o : Otr) if (!o.raw.empty()) { nfit_rows += o.raw.size(); nfit_img++; }
    if (nfit_rows < (size_t)VAR.pca_dim * 4) {
        fprintf(stderr, "VD_PREP_FAIL pca_fit_rows_too_few=%zu\n", nfit_rows); return 5;
    }
    fprintf(stderr, "pca: fitting %d comps on %zu train rows from %zu train images...\n",
            VAR.pca_dim, nfit_rows, nfit_img);
    cv::PCA pca;
    {
        std::vector<float> flat;
        flat.reserve(nfit_rows * (size_t)VAR.hog_dim);
        for (auto &o : Otr)
            for (auto &r : o.raw) flat.insert(flat.end(), r.begin(), r.end());
        cv::Mat fitM((int)nfit_rows, VAR.hog_dim, CV_32F, flat.data());
        pca = cv::PCA(fitM, cv::Mat(), cv::PCA::DATA_AS_ROW, VAR.pca_dim);
    }

    /* phase B: replay HOG on the fixed boxes and project through the frozen basis */
    auto project_split = [&](const std::vector<std::string> &ids, std::vector<ImgOut> &v,
                             const char *tag) {
        fprintf(stderr, "project: %s...\n", tag);
        std::vector<std::thread> th;
        std::atomic<size_t> next(0);
        for (int w = 0; w < workers; w++)
            th.emplace_back([&]() {
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= v.size()) break;
                    project_image(root, ids[i], pca, v[i]);
                }
            });
        for (auto &t : th) t.join();
        for (auto &o : v)
            if (!o.ok) { fprintf(stderr, "VD_PREP_FAIL %s\n", o.err.c_str()); exit(4); }
    };
    project_split(tr, Otr, "train"); project_split(va, Ova, "val"); project_split(te, Ote, "test");

    if (vd_mkdir_p(outdir.c_str()) != 0) {
        fprintf(stderr, "VD_PREP_FAIL mkdir_refused:%s\n", outdir.c_str());
        return 5;
    }
    write_pack(outdir + "/train.pack", Otr, VAR.pca_dim);
    write_pack(outdir + "/val.pack",   Ova, VAR.pca_dim);
    write_pack(outdir + "/test.pack",  Ote, VAR.pca_dim);
    {
        std::ofstream f(outdir + "/pca.bin", std::ios::binary);
        int32_t d1 = VAR.hog_dim, d2 = VAR.pca_dim;
        f.write((char*)&d1, 4); f.write((char*)&d2, 4);
        cv::Mat mean = pca.mean.reshape(1, 1), ev = pca.eigenvectors;
        f.write((char*)mean.ptr<float>(0), sizeof(float) * VAR.hog_dim);
        for (int i = 0; i < VAR.pca_dim; i++) f.write((char*)ev.ptr<float>(i), sizeof(float) * VAR.hog_dim);
    }
    /* Manifest: the single binding between a cache directory and the run that
       produced it. It carries the SHA-256 of every artefact the bench will
       score, so a bench invocation cannot pair packs from different preps, nor
       score a pack that was edited after extraction. Published last, and
       atomically, so a manifest that exists is a manifest whose artefacts are
       already on disk and final. */
    {
        char sh_tr[65], sh_va[65], sh_te[65], sh_pca[65];
        std::string ptr = outdir + "/train.pack", pva = outdir + "/val.pack";
        std::string pte = outdir + "/test.pack", ppc = outdir + "/pca.bin";
        if (vd_sha256_file(ptr.c_str(), sh_tr) != 0 || vd_sha256_file(pva.c_str(), sh_va) != 0 ||
            vd_sha256_file(pte.c_str(), sh_te) != 0 || vd_sha256_file(ppc.c_str(), sh_pca) != 0) {
            fprintf(stderr, "VD_PREP_FAIL manifest_hash_failed\n");
            return 6;
        }
        size_t np_tr = 0, np_va = 0, np_te = 0;
        for (auto &o : Otr) np_tr += o.props.size();
        for (auto &o : Ova) np_va += o.props.size();
        for (auto &o : Ote) np_te += o.props.size();

        std::ostringstream m;
        m << "manifest_version 1\n"
          << "variant "        << VAR.name       << "\n"
          << "class "          << cls            << "\n"
          << "seed "           << SEED           << "\n"
          << "hog_side "       << VAR.hog_side   << "\n"
          << "color "          << VAR.color      << "\n"
          << "hog_dim "        << VAR.hog_dim    << "\n"
          << "pca_dim "        << VAR.pca_dim    << "\n"
          << "test_offset "    << VAR.test_offset<< "\n"
          << "test_count "     << te.size()      << "\n"
          << "train_img "      << Otr.size()     << "\n"
          << "val_img "        << Ova.size()     << "\n"
          << "test_img "       << Ote.size()     << "\n"
          << "train_prop "     << np_tr          << "\n"
          << "val_prop "       << np_va          << "\n"
          << "test_prop "      << np_te          << "\n"
          << "pca_fit_images " << nfit_img       << "\n"
          << "pca_fit_rows "   << nfit_rows      << "\n"
          << "split_key sha256_content_hash_trainval\n"
          << "trainval_id_overlap 0\n"
          << "trainval_content_overlap 0\n"
          << "prev_test_ids_checked "   << v1_id_checked  << "\n"
          << "prev_test_sha_checked "   << v1_sha_checked << "\n"
          << "prev_test_id_overlap 0\n"
          << "prev_test_content_overlap 0\n"
          << "sha256_prev_test_pack " << (prev_pack_sha.empty() ? "none" : prev_pack_sha) << "\n"
          << "sha256_train_pack " << sh_tr  << "\n"
          << "sha256_val_pack "   << sh_va  << "\n"
          << "sha256_test_pack "  << sh_te  << "\n"
          << "sha256_pca_bin "    << sh_pca << "\n";
        std::string body = m.str();
        std::string mpath = outdir + "/manifest.txt";
        if (vd_publish_file(mpath.c_str(), body.data(), body.size()) != 0) {
            fprintf(stderr, "VD_PREP_FAIL manifest_publish_failed:%s\n", mpath.c_str());
            return 6;
        }
        fprintf(stderr, "manifest published: %s\n", mpath.c_str());
    }

    size_t pt = 0, pv = 0, pe = 0, gt_tr = 0, gt_te = 0, pos_tr = 0;
    for (auto &o : Otr) { pt += o.props.size(); for (auto &g : o.gts) if (!g.difficult) gt_tr++;
                          for (auto &p : o.props) if (p.label == 1) pos_tr++; }
    for (auto &o : Ova) pv += o.props.size();
    for (auto &o : Ote) { pe += o.props.size(); for (auto &g : o.gts) if (!g.difficult) gt_te++; }
    printf("VD_PREP_OK variant=%s class=%s dim=%d train_img=%zu val_img=%zu test_img=%zu "
           "train_prop=%zu val_prop=%zu test_prop=%zu train_pos_prop=%zu "
           "train_gt=%zu test_gt=%zu\n",
           VAR.name, cls.c_str(), VAR.pca_dim, Otr.size(), Ova.size(), Ote.size(),
           pt, pv, pe, pos_tr, gt_tr, gt_te);
    return 0;
}
