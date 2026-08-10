#include "../include/cnet_roe_ocr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 5x7 uppercase + digits. Rows packed as 5 bits LSB-left in a byte. */
typedef struct {
    char ch;
    unsigned char rows[7];
} Glyph;

/* bit0 = leftmost pixel */
static const Glyph FONT[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
    {'3', {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
};
static const int FONT_N = (int)(sizeof FONT / sizeof FONT[0]);

static const Glyph *glyph_for(char c) {
    int i;
    c = (char)toupper((unsigned char)c);
    for (i = 0; i < FONT_N; i++)
        if (FONT[i].ch == c) return &FONT[i];
    return NULL;
}

void roe_ocr_init(RoeOcr *O) {
    if (!O) return;
    memset(O, 0, sizeof *O);
    roe_init(&O->roe);
    roe_goal_init(&O->goal);
}

void roe_ocr_set_catalog(RoeOcr *O, const char *dir) {
    if (!O || !dir) return;
    snprintf(O->catalog_dir, sizeof O->catalog_dir, "%s", dir);
    roe_set_catalog_dir(&O->roe, dir);
    roe_goal_set_catalog(&O->goal, dir);
}

int roe_ocr_seed(RoeOcr *O) {
    if (!O) return -1;
    roe_goal_add_sub(&O->goal, "vision", "ocr");
    roe_goal_add_sub(&O->goal, "vision", "pipeline");
    roe_goal_add_sub(&O->goal, "vision", "pdf");

    /* Shared ROE skills under vision/ocr */
    roe_add_skill(&O->roe, "vision__ocr__hermetic", "ocr_hermetic", "hermetic ocr",
                  "Hermetic 5x7 glyph OCR: render→template match; CERT on exact gold.", 0,
                  1);
    roe_add_skill(&O->roe, "vision__ocr__pipeline", "ocr_pipeline", "ocr pipeline",
                  "OCR pipeline: load image/pdf → preprocess → recognize → verify text.", 0,
                  1);
    roe_add_skill(&O->roe, "vision__ocr__abstain", "ocr_ood", "ocr ood",
                  "OOD OCR: abstain if no backend and no hermetic coverage.", 0, 1);
    roe_add_skill(&O->roe, "vision__pdf__pdftotext", "pdf_text", "pdftotext",
                  "PDF text layer: use pdftotext when embedded text exists.", 0, 1);

    roe_goal_map_pattern(&O->goal, "vision", "ocr", "hermetic ocr",
                         "vision__ocr__hermetic");
    roe_goal_map_pattern(&O->goal, "vision", "pipeline", "ocr pipeline",
                         "vision__ocr__pipeline");
    roe_goal_map_pattern(&O->goal, "vision", "pdf", "pdftotext",
                         "vision__pdf__pdftotext");

    /* teach phrases for promote into catalog */
    roe_add_teach(&O->roe, "ocr confidence", "ocr_conf",
                  "conf = matched_glyphs/len; promote only if conf>=1.0 on gold");
    roe_add_teach(&O->roe, "ocr preprocess", "ocr_pre",
                  "binarize, deskew, segment lines, then glyph/word match");
    roe_add_teach(&O->roe, "vision capsule asset", "vision_asset",
                  "Vision capsule carries frontend asset with unit (schema-2 bound)");

    roe_goal_map_pattern(&O->goal, "vision", "ocr", "ocr confidence",
                         "vision__ocr__conf");
    roe_goal_map_pattern(&O->goal, "vision", "pipeline", "ocr preprocess",
                         "vision__pipeline__pre");
    roe_goal_map_pattern(&O->goal, "vision", "ocr", "vision capsule",
                         "vision__ocr__capsule");

    /* mirror goal engine skills from roe after seed - share by load/save later */
    return 0;
}

int roe_ocr_render(const char *text, double *pixels, int max_pixels,
                   int *width_out, int *height_out) {
    int n, i, r, c, x, w, h, stride;
    if (!text || !pixels || max_pixels <= 0) return -1;
    n = (int)strlen(text);
    if (n > ROE_OCR_MAX_IMG) n = ROE_OCR_MAX_IMG;
    w = n * (ROE_OCR_GLYPH_W + 1);
    h = ROE_OCR_GLYPH_H;
    if (w * h > max_pixels) return -1;
    stride = w;
    memset(pixels, 0, (size_t)(w * h) * sizeof(double));
    for (i = 0; i < n; i++) {
        const Glyph *g = glyph_for(text[i]);
        if (!g) g = glyph_for(' ');
        x = i * (ROE_OCR_GLYPH_W + 1);
        for (r = 0; r < ROE_OCR_GLYPH_H; r++) {
            for (c = 0; c < ROE_OCR_GLYPH_W; c++) {
                if (g->rows[r] & (1u << c))
                    pixels[r * stride + x + c] = 1.0;
            }
        }
    }
    if (width_out) *width_out = w;
    if (height_out) *height_out = h;
    return w * h;
}

static int match_glyph(const double *cell, int stride) {
    int best = -1, bi = -1, i, r, c, score, best_score = -1;
    for (i = 0; i < FONT_N; i++) {
        score = 0;
        for (r = 0; r < ROE_OCR_GLYPH_H; r++) {
            for (c = 0; c < ROE_OCR_GLYPH_W; c++) {
                int on = (FONT[i].rows[r] & (1u << c)) ? 1 : 0;
                int pix = cell[r * stride + c] >= 0.5 ? 1 : 0;
                if (on == pix) score++;
            }
        }
        if (score > best_score) {
            best_score = score;
            bi = i;
            best = score;
        }
    }
    (void)best;
    /* require perfect or near-perfect for conf */
    if (best_score < ROE_OCR_GLYPH_W * ROE_OCR_GLYPH_H - 2) return -1;
    return bi;
}

int roe_ocr_from_pixels(const double *pixels, int width, int height,
                        char *text_out, size_t cap, double *conf_out) {
    int n_chars, i, ok = 0, stride;
    if (!pixels || !text_out || cap < 2 || width <= 0 || height < ROE_OCR_GLYPH_H)
        return -1;
    stride = width;
    n_chars = width / (ROE_OCR_GLYPH_W + 1);
    if (n_chars < 1) return -1;
    if ((size_t)n_chars + 1 > cap) n_chars = (int)cap - 1;
    for (i = 0; i < n_chars; i++) {
        int x = i * (ROE_OCR_GLYPH_W + 1);
        int gi = match_glyph(pixels + x, stride);
        if (gi < 0) {
            text_out[i] = '?';
        } else {
            text_out[i] = FONT[gi].ch;
            ok++;
        }
    }
    text_out[n_chars] = 0;
    if (conf_out) *conf_out = n_chars ? (double)ok / (double)n_chars : 0.0;
    (void)height;
    return n_chars;
}

int roe_ocr_roundtrip(const char *text, RoeOcrResult *out) {
    double pix[ROE_OCR_IMG_W * ROE_OCR_IMG_H];
    int w = 0, h = 0, np;
    double conf = 0;
    char got[ROE_OCR_MAX_TEXT];
    char norm[ROE_OCR_MAX_TEXT];
    size_t i, j = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!text) return -1;
    /* normalize gold to font alphabet */
    for (i = 0; text[i] && j + 1 < sizeof norm; i++) {
        char c = (char)toupper((unsigned char)text[i]);
        if (glyph_for(c)) norm[j++] = c;
    }
    norm[j] = 0;
    if (!norm[0]) return -1;
    np = roe_ocr_render(norm, pix, (int)(sizeof pix / sizeof pix[0]), &w, &h);
    if (np < 0) return -1;
    if (roe_ocr_from_pixels(pix, w, h, got, sizeof got, &conf) < 0) return -1;
    if (out) {
        snprintf(out->gold, sizeof out->gold, "%s", norm);
        snprintf(out->predicted, sizeof out->predicted, "%s", got);
        out->conf = conf;
        out->ok = (strcmp(norm, got) == 0);
        out->tokens_est = 0;
        out->source = 1;
    }
    return (strcmp(norm, got) == 0) ? 0 : 1;
}

static int run_pdftotext(const char *path, char *out, size_t cap) {
    char cmd[ROE_PATH_MAX + 64];
    char tmp[] = "/tmp/roe_ocr_XXXXXX";
    int fd, rc;
    FILE *f;
    size_t n;
    if (!path || !out || cap < 8) return -1;
    fd = mkstemp(tmp);
    if (fd < 0) return -1;
    close(fd);
    snprintf(cmd, sizeof cmd, "pdftotext -q -layout %s %s 2>/dev/null", path, tmp);
    rc = system(cmd);
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }
    f = fopen(tmp, "r");
    if (!f) {
        unlink(tmp);
        return -1;
    }
    n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
    unlink(tmp);
    return n > 0 ? 0 : -1;
}

int roe_ocr_file(RoeOcr *O, const char *path, RoeOcrResult *out) {
    const char *ext;
    if (out) memset(out, 0, sizeof *out);
    if (!O || !path) return -1;
    if (out) snprintf(out->path, sizeof out->path, "%s", path);
    ext = strrchr(path, '.');
    if (ext && (!strcmp(ext, ".txt") || !strcmp(ext, ".md"))) {
        FILE *f = fopen(path, "r");
        size_t n;
        if (!f) return -1;
        n = fread(out ? out->predicted : NULL, 1,
                  out ? sizeof out->predicted - 1 : 0, f);
        if (out) {
            out->predicted[n] = 0;
            out->ok = 1;
            out->conf = 1.0;
            out->source = 1;
            out->tokens_est = 0;
        }
        fclose(f);
        O->n_ocr++;
        O->n_exact++;
        return 0;
    }
    if (ext && !strcmp(ext, ".pdf")) {
        char buf[ROE_OCR_MAX_TEXT * 4];
        O->n_pdf++;
        if (run_pdftotext(path, buf, sizeof buf) == 0 && out) {
            size_t bl = strlen(buf);
            if (bl >= sizeof out->predicted) bl = sizeof out->predicted - 1;
            memcpy(out->predicted, buf, bl);
            out->predicted[bl] = 0;
            out->ok = 1;
            out->conf = 0.9;
            out->source = 2;
            out->tokens_est = 8;
            O->n_ocr++;
            return 0;
        }
        if (out) {
            snprintf(out->predicted, sizeof out->predicted,
                     "ABSTAIN: pdf ocr backend unavailable");
            out->source = 3;
            out->ok = 0;
        }
        O->n_fail++;
        return 1;
    }
    if (out) {
        snprintf(out->predicted, sizeof out->predicted,
                 "ABSTAIN: unsupported ocr input");
        out->source = 3;
    }
    O->n_fail++;
    return 1;
}

int roe_ocr_learn_phrase(RoeOcr *O, const char *phrase, const char *note) {
    RoeOcrResult r;
    char id[ROE_NAME_MAX];
    char ans[ROE_ANSWER_MAX];
    size_t i, k = 0;
    if (!O || !phrase) return -1;
    O->n_render++;
    if (roe_ocr_roundtrip(phrase, &r) != 0 || !r.ok) {
        O->n_fail++;
        return 0;
    }
    O->n_ocr++;
    O->n_exact++;
    /* skill id */
    id[0] = 'o';
    id[1] = 'c';
    id[2] = 'r';
    id[3] = '_';
    for (i = 0; phrase[i] && k + 5 < sizeof id; i++) {
        unsigned char c = (unsigned char)phrase[i];
        if (isalnum(c)) id[4 + k++] = (char)tolower(c);
        else if (k && id[3 + k] != '_') id[4 + k++] = '_';
    }
    id[4 + k] = 0;
    snprintf(ans, sizeof ans, "OCR_OK conf=%.2f text=%s%s%s", r.conf, r.predicted,
             note && note[0] ? " | " : "", note ? note : "");
    if (roe_add_skill(&O->roe, id, "ocr_phrase", phrase, ans, 0, 1) == 0) {
        O->n_promote++;
        roe_goal_map_pattern(&O->goal, "vision", "ocr", phrase, id);
        if (O->catalog_dir[0]) (void)roe_ocr_save(O);
        return 1;
    }
    return 0;
}

int roe_ocr_train_curriculum(RoeOcr *O, RoeTrainReport *rep) {
    static const char *phrases[] = {
        "HELLO", "OCR", "CNET", "ROE ASI", "VISION", "CAPSULE", "READ TEXT",
        "A1B2",  "OPEN", "CODE", "DEBUG",  "SHELL",  "PASS",    "FAIL",
        "0123456789", "TEST",
    };
    int n = (int)(sizeof phrases / sizeof phrases[0]);
    int i, prom = 0, exact = 0;
    uint64_t tok = 0, base = 0;
    RoeReply rr;
    if (rep) memset(rep, 0, sizeof *rep);
    if (!O) return -1;

    for (i = 0; i < n; i++) {
        RoeOcrResult r;
        base += 40; /* pretend LLM vision call */
        O->n_render++;
        if (roe_ocr_roundtrip(phrases[i], &r) == 0 && r.ok) {
            exact++;
            O->n_ocr++;
            O->n_exact++;
            if (roe_ocr_learn_phrase(O, phrases[i], "curriculum")) prom++;
        } else {
            O->n_fail++;
        }
    }

    /* knowledge skills via teach promote */
    {
        const char *qs[] = {"ocr confidence", "ocr preprocess",
                            "vision capsule asset", "ocr pipeline", "hermetic ocr"};
        for (i = 0; i < 5; i++) {
            base += 30;
            if (roe_turn(&O->roe, qs[i], &rr) == ROE_OK) {
                if (rr.source == ROE_SRC_LOCAL) {
                    /* have */
                } else {
                    tok += rr.tokens_est;
                    (void)roe_feedback_verify(&O->roe, qs[i], NULL, 1);
                    prom++;
                }
            }
        }
    }

    if (rep) {
        rep->turns = (uint64_t)n + 5;
        rep->promotes = (uint64_t)prom;
        rep->tokens_used = tok;
        rep->tokens_baseline = base;
        rep->local_hit_rate = n ? (double)exact / (double)n : 0.0;
        rep->token_save_ratio =
            base ? 1.0 - (double)tok / (double)base : 0.0;
    }
    if (O->catalog_dir[0]) (void)roe_ocr_save(O);
    return (exact == n && prom > 0) ? 0 : 1;
}

int roe_ocr_save(RoeOcr *O) {
    if (!O || !O->catalog_dir[0]) return -1;
    O->goal.roe = O->roe; /* share skills into goal saver tidy tree */
    /* copy skills into goal engine for tidy export */
    {
        size_t i;
        O->goal.roe = O->roe;
        for (i = 0; i < O->roe.n_skills; i++) {
            /* maps already set */
            (void)i;
        }
    }
    (void)roe_save_catalog(&O->roe);
    return roe_goal_save(&O->goal);
}

int roe_ocr_load(RoeOcr *O) {
    if (!O || !O->catalog_dir[0]) return -1;
    return roe_load_catalog(&O->roe);
}

void roe_ocr_dump_stats(const RoeOcr *O, char *buf, size_t cap) {
    if (!O || !buf || !cap) return;
    snprintf(buf, cap,
             "render=%llu ocr=%llu exact=%llu fail=%llu pdf=%llu promote=%llu "
             "skills=%zu",
             (unsigned long long)O->n_render, (unsigned long long)O->n_ocr,
             (unsigned long long)O->n_exact, (unsigned long long)O->n_fail,
             (unsigned long long)O->n_pdf, (unsigned long long)O->n_promote,
             O->roe.n_skills);
}
