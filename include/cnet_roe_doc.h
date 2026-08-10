/* ROE Document L3 + OCR asset packs — better asset than bare Unlimited.
 *
 * L3: (corpus_id + page_sig) → CERT markdown/body after verify.
 * Pack: tidy vision cat/sub export/import with ABI + fail-closed load.
 * Never second brain: teacher remains external; this is the product SKU layer.
 */
#ifndef CNET_ROE_DOC_H
#define CNET_ROE_DOC_H

#include "cnet_roe_asi.h"
#include "cnet_roe_goal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_DOC_CORPUS_MAX 64
#define ROE_DOC_MEM_MAX 256
#define ROE_DOC_SIG_MAX 160
#define ROE_DOC_NAME 64
#define ROE_DOC_BODY 4096 /* was 768 — long page / pack bodies */
#define ROE_DOC_ABI_VER 2
#define ROE_DOC_ABI_MAGIC "ROE_OCR_PACK"
#define ROE_DOC_PDF_MAX_PAGES 512 /* stream cap per book run */

typedef enum {
    ROE_DOC_L3_HIT = 1,
    ROE_DOC_LOCAL_PDF = 2,     /* pdftotext digital layer */
    ROE_DOC_LOCAL_CLASSIC = 3, /* tesseract if present */
    ROE_DOC_TEACHER = 4,       /* external VLM teacher needed */
    ROE_DOC_ABSTAIN = 5
} RoeDocSource;

typedef struct {
    char corpus_id[ROE_DOC_NAME];
    char sig[ROE_DOC_SIG_MAX];       /* header/template fingerprint */
    char skill_id[ROE_NAME_MAX];
    char body[ROE_DOC_BODY];         /* CERT markdown / text */
    char note[ROE_TEXT_MAX];
    int certified;
    int active;
    uint64_t hits;
    uint64_t verifies;
    double cost_units_saved;         /* cumulative avoided teacher units */
} RoeDocMemory;

typedef struct {
    char id[ROE_DOC_NAME];
    int active;
    uint64_t n_pages;
    uint64_t n_l3_hits;
    uint64_t n_teacher;
} RoeDocCorpus;

typedef struct {
    RoeAsi roe;
    RoeGoalEngine goal;
    RoeDocCorpus corpora[ROE_DOC_CORPUS_MAX];
    size_t n_corpora;
    RoeDocMemory mem[ROE_DOC_MEM_MAX];
    size_t n_mem;
    char catalog_dir[ROE_PATH_MAX];

    /* economics (frozen bench units: 1 teacher page = 200 units default) */
    double teacher_unit_cost;
    double local_unit_cost;
    uint64_t n_turns;
    uint64_t n_l3;
    uint64_t n_local_pdf;
    uint64_t n_local_classic;
    uint64_t n_teacher;
    uint64_t n_abstain;
    uint64_t n_verify_ok;
    uint64_t n_verify_fail;
    uint64_t n_promote;
    double cost_paid;
    double cost_baseline_teacher_always;
} RoeDocAsset;

typedef struct {
    int source; /* RoeDocSource */
    char sig[ROE_DOC_SIG_MAX];
    char body[ROE_DOC_BODY];
    char skill_id[ROE_NAME_MAX];
    double cost_units;
    int certified_local;
} RoeDocReply;

typedef struct {
    int n_pages;
    double cost_teacher_always;
    double cost_hybrid;
    double save_ratio;
    double usd_per_page_teacher; /* if unit→usd mapped */
    double usd_per_page_hybrid;
    double unit_to_usd;
    int l3_hits;
    int teacher_calls;
    int quality_match; /* 1 if hybrid body matches teacher on covered */
    char summary[320];
} RoeDocDollarBench;

typedef struct {
    int n_pages;
    int n_l3;
    int n_local_pdf;
    int n_local_classic;
    int n_teacher;
    int n_abstain;
    double teacher_rate; /* teacher/pages */
    double local_rate;   /* 1 - teacher_rate */
    double cost_save;
    int kpi_teacher_under_10pct; /* 1 if teacher_rate < 0.10 */
    char by_corpus[512];
    char summary[384];
} RoeDocCoverage;

void roe_doc_init(RoeDocAsset *A);
void roe_doc_set_catalog(RoeDocAsset *A, const char *dir);
void roe_doc_set_unit_cost(RoeDocAsset *A, double teacher_unit, double local_unit);

int roe_doc_add_corpus(RoeDocAsset *A, const char *corpus_id);

/* Fingerprint page text/header for L3 key (stable-ish). */
void roe_doc_signature(const char *page_text_or_header, char *sig, size_t cap);
/* Stronger sig: text + layout features (newlines, pipes, len bucket). */
void roe_doc_signature_ex(const char *page_text, int n_newlines, int n_pipes,
                          int n_chars, char *sig, size_t cap);

/* Resolve: L3 local first, else miss (caller may invoke teacher). */
int roe_doc_resolve(RoeDocAsset *A, const char *corpus_id, const char *page_text,
                    RoeDocReply *out);

/* Local-first router:
 *   1) if path is PDF → pdftotext (digital)
 *   2) L3 memory on extracted/hint text
 *   3) classic OCR if available and image/pdf raster
 *   4) else ABSTAIN (teacher)
 * auto_cert_local=1 promotes digital/classic extracts under verify(local_ok).
 */
int roe_doc_route(RoeDocAsset *A, const char *corpus_id, const char *path,
                  const char *page_text_hint, int auto_cert_local,
                  RoeDocReply *out);

/* pdftotext helper; returns 0 if got substantial text into body. */
int roe_doc_pdftotext(const char *pdf_path, char *body, size_t cap);

/* Page-chunked PDF: extract one page (1-based) via pdftotext -f -l. */
int roe_doc_pdftotext_page(const char *pdf_path, int page_1based, char *body,
                           size_t cap);

/* PDF page count via pdfinfo; -1 on failure. */
int roe_doc_pdf_page_count(const char *pdf_path);

/*
 * Stream a PDF page-by-page (digital text layer).
 * For each page: L3 → local pdftotext page → optional teacher_sim CERT.
 * Fills out_pages/out_l3/out_local/out_teacher. Caps at ROE_DOC_PDF_MAX_PAGES
 * or max_pages if >0 and smaller.
 * Returns 0 on success (even if some pages abstain).
 */
int roe_doc_pdf_stream(RoeDocAsset *A, const char *corpus_id, const char *pdf_path,
                       int max_pages, int auto_cert_local, int teacher_sim,
                       int *out_pages, int *out_l3, int *out_local,
                       int *out_teacher);

/* After external teacher returns body: verify+promote L3.
 * tests_passed or user_accept required. */
int roe_doc_verify_learn(RoeDocAsset *A, const char *corpus_id,
                         const char *page_text, const char *teacher_body,
                         const char *note, int tests_passed, int user_accept);

/* Seed vision taxonomy + pipeline skills into asset. */
int roe_doc_seed_asset(RoeDocAsset *A);

/* ---- Pack ABI: export/import fail-closed ---- */
int roe_doc_pack_export(const RoeDocAsset *A, const char *out_dir);
int roe_doc_pack_import(RoeDocAsset *A, const char *in_dir);
/* Returns 1 if pack dir has valid ABI manifest. */
int roe_doc_pack_validate(const char *pack_dir);

/* $/page freeze bench: simulate N pages with repeat rate. */
int roe_doc_dollar_bench(RoeDocAsset *A, int n_pages, int unique_docs,
                         double unit_to_usd, RoeDocDollarBench *B);

/* Coverage / gardener KPI snapshot. */
void roe_doc_coverage(const RoeDocAsset *A, RoeDocCoverage *C);

/* Batch distill: ingest dir of .txt/.pdf into corpus L3 via local paths;
 * teacher_sim=1 uses filename as synthetic teacher only when local fails
 * (for offline factory sim without GPU). */
int roe_doc_batch_distill(RoeDocAsset *A, const char *corpus_id,
                          const char *dir_path, int teacher_sim, int *n_ok,
                          int *n_teacher);

int roe_doc_save(const RoeDocAsset *A);
int roe_doc_load(RoeDocAsset *A);
void roe_doc_dump_stats(const RoeDocAsset *A, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
