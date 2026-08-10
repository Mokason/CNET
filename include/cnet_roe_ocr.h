/* ROE-ASI OCR capability — hermetic glyph OCR + tidy vision/ocr capsules.
 *
 * External Unlimited-OCR / tesseract are teachers when present; hermetic
 * 5x7 template OCR is the offline CERT path (like vision v0 shapes).
 * Shell: promote only after verify against gold / user_accept.
 */
#ifndef CNET_ROE_OCR_H
#define CNET_ROE_OCR_H

#include "cnet_roe_asi.h"
#include "cnet_roe_goal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_OCR_GLYPH_W 5
#define ROE_OCR_GLYPH_H 7
#define ROE_OCR_MAX_TEXT 256
#define ROE_OCR_MAX_IMG 64
#define ROE_OCR_IMG_W (ROE_OCR_MAX_IMG * (ROE_OCR_GLYPH_W + 1))
#define ROE_OCR_IMG_H ROE_OCR_GLYPH_H

typedef struct {
    char path[ROE_PATH_MAX];
    char gold[ROE_OCR_MAX_TEXT];
    char predicted[ROE_OCR_MAX_TEXT];
    int ok; /* exact match gold */
    double conf;
    uint64_t tokens_est; /* 0 if local hermetic */
    int source; /* RoeSource-like: 1 local hermetic, 2 tool, 3 abstain */
} RoeOcrResult;

typedef struct {
    RoeAsi roe;
    RoeGoalEngine goal; /* taxonomy vision/ocr */
    char catalog_dir[ROE_PATH_MAX];
    uint64_t n_render;
    uint64_t n_ocr;
    uint64_t n_exact;
    uint64_t n_fail;
    uint64_t n_pdf;
    uint64_t n_promote;
} RoeOcr;

void roe_ocr_init(RoeOcr *O);
void roe_ocr_set_catalog(RoeOcr *O, const char *dir);

/* Seed vision/ocr category + pipeline skills + teach curriculum */
int roe_ocr_seed(RoeOcr *O);

/* Hermetic 5x7 font: render text → binary image (row-major 0/1 doubles).
 * Returns pixel count or -1. width_out/height_out optional. */
int roe_ocr_render(const char *text, double *pixels, int max_pixels,
                   int *width_out, int *height_out);

/* OCR binary image with known glyph templates. conf = matched/len. */
int roe_ocr_from_pixels(const double *pixels, int width, int height,
                        char *text_out, size_t cap, double *conf_out);

/* Render then OCR (self-consistency train path). */
int roe_ocr_roundtrip(const char *text, RoeOcrResult *out);

/* OCR a path: .txt passthrough; .pdf via pdftotext if available; else abstain. */
int roe_ocr_file(RoeOcr *O, const char *path, RoeOcrResult *out);

/* Verify + promote OCR skill under vision/ocr/<sub> */
int roe_ocr_learn_phrase(RoeOcr *O, const char *phrase, const char *note);

/* Run curriculum train; returns 0 if gate metrics met */
int roe_ocr_train_curriculum(RoeOcr *O, RoeTrainReport *rep);

int roe_ocr_save(RoeOcr *O);
int roe_ocr_load(RoeOcr *O);

void roe_ocr_dump_stats(const RoeOcr *O, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
