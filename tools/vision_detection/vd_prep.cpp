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
#include <thread>
#include <vector>

static const int SS_WIDTH   = 300;
static const int MAX_PROP   = 300;   /* per image, after SS ordering */
static const uint64_t SEED  = 20260727ULL;

/* Variant table. V1 is frozen evidence (commit 6ffc5f1) and must keep reproducing
   byte-for-byte; V2 changes exactly the feature description and the holdout slice.
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

/* SHA-256 (small, for content-hash leakage assertions) */
struct Sha256 {
    uint32_t s[8]; uint64_t len; uint8_t buf[64]; size_t n;
    static uint32_t rr(uint32_t x, int c){return (x>>c)|(x<<(32-c));}
    Sha256(){reset();}
    void reset(){ static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(s,iv,sizeof s); len=0; n=0; }
    void block(const uint8_t*p){
        static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for(int i=0;i<16;i++) w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
        for(int i=16;i<64;i++){uint32_t a=rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3),b=rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+a+w[i-7]+b;}
        uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
        for(int i=0;i<64;i++){uint32_t S1=rr(e,6)^rr(e,11)^rr(e,25),ch=(e&f)^((~e)&g),t1=h+S1+ch+k[i]+w[i],S0=rr(a,2)^rr(a,13)^rr(a,22),mj=(a&b)^(a&c)^(b&c),t2=S0+mj;h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
    }
    void update(const uint8_t*p,size_t l){ len+=l; while(l){ size_t t=std::min(l,64-n); memcpy(buf+n,p,t); n+=t; p+=t; l-=t; if(n==64){block(buf);n=0;} } }
    std::string hex(){ uint64_t bl=len*8; uint8_t pad=0x80; update(&pad,1); uint8_t z=0; while(n!=56) update(&z,1);
        uint8_t e[8]; for(int i=0;i<8;i++) e[i]=(uint8_t)(bl>>(56-i*8)); update(e,8);
        char o[65]; for(int i=0;i<8;i++) sprintf(o+i*8,"%08x",s[i]); return std::string(o,64); }
};

static std::string sha256_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    Sha256 h; std::vector<uint8_t> b(65536);
    while (f) { f.read((char*)b.data(), (std::streamsize)b.size()); std::streamsize g=f.gcount(); if(g>0) h.update(b.data(),(size_t)g); }
    return h.hex();
}

/* ---------- VOC annotation parsing (fails loud on malformed) ------------- */
static bool tag_val(const std::string &s, const char *tag, std::string &out) {
    std::string a = std::string("<") + tag + ">", b = std::string("</") + tag + ">";
    size_t i = s.find(a); if (i == std::string::npos) return false;
    size_t j = s.find(b, i); if (j == std::string::npos) return false;
    out = s.substr(i + a.size(), j - i - a.size());
    return true;
}

static bool parse_ann(const std::string &path, const std::string &cls,
                      std::vector<GT> &gts, std::string &err) {
    std::ifstream f(path);
    if (!f) { err = "annotation_missing:" + path; return false; }
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
            GT g; g.difficult = tag_val(ob, "difficult", dif) ? atoi(dif.c_str()) : 0;
            int x1 = atoi(xs.c_str()), y1 = atoi(ys.c_str());
            int x2 = atoi(xe.c_str()), y2 = atoi(ye.c_str());
            if (x2 <= x1 || y2 <= y1) { err = "object_bad_box:" + path; return false; }
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
    if (!parse_ann(ap, cls, o.gts, o.err)) return;

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

/* Read just the image IDs out of an existing pack. Used to assert the V2 holdout
   is disjoint from the images V1 actually scored -- checked against the real
   artefact on disk, not against a re-derivation of the same shuffle. */
static std::vector<std::string> read_pack_ids(const std::string &path) {
    std::vector<std::string> ids;
    std::ifstream f(path, std::ios::binary);
    if (!f) return ids;
    PackHdr h;
    f.read((char*)&h, sizeof h);
    if (memcmp(h.magic, "VDPACK1", 7) != 0) return ids;
    for (int i = 0; i < h.n_img; i++) {
        int32_t idlen = 0, np = 0, ng = 0;
        f.read((char*)&idlen, 4);
        if (!f || idlen <= 0 || idlen > 30) return ids;
        std::string id(idlen, '\0');
        f.read(&id[0], idlen);
        f.read((char*)&np, 4); f.read((char*)&ng, 4);
        if (!f) return ids;
        f.seekg((std::streamoff)ng * 20 + (std::streamoff)np * (20 + 4LL * h.dim), std::ios::cur);
        ids.push_back(id);
    }
    return ids;
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
    if (!v1cache.empty()) {
        v1_ids = read_pack_ids(v1cache + "/test.pack");
        if (v1_ids.empty()) { fprintf(stderr, "VD_PREP_FAIL v1cache_unreadable:%s\n", v1cache.c_str()); return 3; }
        std::set<std::string> prev(v1_ids.begin(), v1_ids.end());
        for (auto &x : te)
            if (prev.count(x)) { fprintf(stderr, "VD_PREP_FAIL v1_v2_test_id_leak:%s\n", x.c_str()); return 3; }
        v1_id_checked = v1_ids.size();
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
        for (auto &s : vs) if (!s.empty()) prev.insert(s);
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

    if (system(("mkdir -p " + outdir).c_str()) != 0) { fprintf(stderr, "VD_PREP_FAIL mkdir\n"); return 5; }
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
    {   /* provenance the bench reads back into its JSON */
        std::ofstream f(outdir + "/meta.txt");
        f << "variant " << VAR.name << "\n"
          << "hog_side " << VAR.hog_side << "\ncolor " << VAR.color
          << "\nhog_dim " << VAR.hog_dim << "\npca_dim " << VAR.pca_dim
          << "\ntest_offset " << VAR.test_offset << "\ntest_count " << te.size()
          << "\npca_fit_images " << nfit_img << "\npca_fit_rows " << nfit_rows
          << "\nprev_test_ids_checked " << v1_id_checked
          << "\nprev_test_sha_checked " << v1_sha_checked << "\n";
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
