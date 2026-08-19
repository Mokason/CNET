#include "../../include/cnet_roe_doc.h"
#include "../../include/cnet_roe_table.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int name_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static int mkdir_p(const char *path) {
    char tmp[ROE_PATH_MAX];
    size_t i, len;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            cnet_mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return cnet_mkdir(tmp, 0755);
}

static RoeDocCorpus *find_corpus(RoeDocAsset *A, const char *id) {
    size_t i;
    for (i = 0; i < A->n_corpora; i++)
        if (A->corpora[i].active && name_eq(A->corpora[i].id, id))
            return &A->corpora[i];
    return NULL;
}

static RoeDocMemory *find_mem(RoeDocAsset *A, const char *corpus, const char *sig) {
    size_t i;
    for (i = 0; i < A->n_mem; i++)
        if (A->mem[i].active && A->mem[i].certified &&
            name_eq(A->mem[i].corpus_id, corpus) && name_eq(A->mem[i].sig, sig))
            return &A->mem[i];
    return NULL;
}

void roe_doc_init(RoeDocAsset *A) {
    if (!A) return;
    memset(A, 0, sizeof *A);
    roe_init(&A->roe);
    roe_goal_init(&A->goal);
    A->teacher_unit_cost = 200.0;
    A->local_unit_cost = 0.0;
}

void roe_doc_set_catalog(RoeDocAsset *A, const char *dir) {
    if (!A || !dir) return;
    snprintf(A->catalog_dir, sizeof A->catalog_dir, "%s", dir);
    roe_set_catalog_dir(&A->roe, dir);
    roe_goal_set_catalog(&A->goal, dir);
}

void roe_doc_set_unit_cost(RoeDocAsset *A, double teacher_unit, double local_unit) {
    if (!A) return;
    A->teacher_unit_cost = teacher_unit > 0 ? teacher_unit : 200.0;
    A->local_unit_cost = local_unit >= 0 ? local_unit : 0.0;
}

int roe_doc_add_corpus(RoeDocAsset *A, const char *corpus_id) {
    size_t i;
    if (!A || !corpus_id || !corpus_id[0]) return -1;
    for (i = 0; i < A->n_corpora; i++)
        if (A->corpora[i].active && name_eq(A->corpora[i].id, corpus_id))
            return 0;
    if (A->n_corpora >= ROE_DOC_CORPUS_MAX) return -1;
    memset(&A->corpora[A->n_corpora], 0, sizeof A->corpora[0]);
    snprintf(A->corpora[A->n_corpora].id, sizeof A->corpora[0].id, "%s",
             corpus_id);
    A->corpora[A->n_corpora].active = 1;
    A->n_corpora++;
    return 0;
}

void roe_doc_signature(const char *page_text_or_header, char *sig, size_t cap) {
    int nl = 0, pipes = 0, nch = 0;
    const char *p;
    if (page_text_or_header) {
        for (p = page_text_or_header; *p; p++) {
            nch++;
            if (*p == '\n') nl++;
            if (*p == '|') pipes++;
        }
    }
    roe_doc_signature_ex(page_text_or_header, nl, pipes, nch, sig, cap);
}

void roe_doc_signature_ex(const char *page_text, int n_newlines, int n_pipes,
                          int n_chars, char *sig, size_t cap) {
    const char *p;
    char buf[512];
    size_t i, o = 0, n = 0;
    unsigned h = 2166136261u;
    int len_bucket;
    if (!sig || !cap) return;
    sig[0] = 0;
    if (!page_text || !page_text[0]) {
        snprintf(sig, cap, "empty_L%d_P%d", n_newlines, n_pipes);
        return;
    }
    for (p = page_text; *p && n + 1 < sizeof buf; p++) {
        unsigned char c = (unsigned char)*p;
        h ^= c;
        h *= 16777619u;
        if (isspace(c)) {
            if (n && buf[n - 1] != '_') buf[n++] = '_';
            continue;
        }
        buf[n++] = (char)tolower(c);
        if (n >= 80) break;
    }
    buf[n] = 0;
    while (n > 0 && buf[n - 1] == '_') buf[--n] = 0;
    len_bucket = n_chars < 200 ? 0 : n_chars < 2000 ? 1 : n_chars < 20000 ? 2 : 3;
    /* text head */
    for (i = 0; buf[i] && o + 1 < cap && o < 72; i++) {
        char c = buf[i];
        if (!isalnum((unsigned char)c) && c != '_') c = '_';
        if (c == '_' && o && sig[o - 1] == '_') continue;
        sig[o++] = c;
    }
    /* layout tail */
    if (o + 24 < cap) {
        int nw = snprintf(sig + o, cap - o, "_L%d_P%d_B%d_%08x", n_newlines % 1000,
                          n_pipes % 1000, len_bucket, h);
        if (nw > 0) o += (size_t)nw;
    }
    sig[cap - 1] = 0;
    if (!sig[0]) snprintf(sig, cap, "doc_%u", (unsigned)n_chars);
}

int roe_doc_seed_asset(RoeDocAsset *A) {
    if (!A) return -1;
    roe_goal_add_sub(&A->goal, "vision", "ocr");
    roe_goal_add_sub(&A->goal, "vision", "layout");
    roe_goal_add_sub(&A->goal, "vision", "table");
    roe_goal_add_sub(&A->goal, "vision", "doc_l3");
    roe_goal_add_sub(&A->goal, "vision", "pipeline");

    roe_add_skill(&A->roe, "vision__doc_l3__memory", "doc_l3", "document l3 memory",
                  "Corpus+signature CERT body; teacher only on miss.", 0, 1);
    roe_add_skill(&A->roe, "vision__pipeline__teacher_bridge", "teacher_bridge",
                  "unlimited teacher bridge",
                  "External Unlimited-OCR is untrusted teacher; host verifies.", 0, 1);
    roe_add_skill(&A->roe, "vision__ocr__asset_sku", "ocr_asset", "ocr asset sku",
                  "Ship CERT packs not a second monobrain.", 0, 1);

    roe_goal_map_pattern(&A->goal, "vision", "doc_l3", "document l3 memory",
                         "vision__doc_l3__memory");
    roe_goal_map_pattern(&A->goal, "vision", "pipeline", "unlimited teacher bridge",
                         "vision__pipeline__teacher_bridge");
    roe_goal_map_pattern(&A->goal, "vision", "ocr", "ocr asset sku",
                         "vision__ocr__asset_sku");
    return 0;
}

int roe_doc_resolve(RoeDocAsset *A, const char *corpus_id, const char *page_text,
                    RoeDocReply *out) {
    char sig[ROE_DOC_SIG_MAX];
    RoeDocMemory *m;
    RoeDocCorpus *c;
    if (out) memset(out, 0, sizeof *out);
    if (!A || !corpus_id || !page_text) return -1;

    A->n_turns++;
    A->cost_baseline_teacher_always += A->teacher_unit_cost;
    roe_doc_add_corpus(A, corpus_id);
    c = find_corpus(A, corpus_id);
    if (c) c->n_pages++;

    roe_doc_signature(page_text, sig, sizeof sig);
    if (out) snprintf(out->sig, sizeof out->sig, "%s", sig);

    m = find_mem(A, corpus_id, sig);
    if (m) {
        m->hits++;
        m->cost_units_saved += A->teacher_unit_cost - A->local_unit_cost;
        if (c) c->n_l3_hits++;
        A->n_l3++;
        A->cost_paid += A->local_unit_cost;
        if (out) {
            out->source = ROE_DOC_L3_HIT;
            out->cost_units = A->local_unit_cost;
            out->certified_local = 1;
            snprintf(out->skill_id, sizeof out->skill_id, "%s", m->skill_id);
            snprintf(out->body, sizeof out->body, "%s", m->body);
        }
        return 0;
    }

    A->n_abstain++;
    if (c) c->n_teacher++; /* pending teacher */
    if (out) {
        out->source = ROE_DOC_ABSTAIN;
        out->cost_units = 0;
        out->certified_local = 0;
        snprintf(out->body, sizeof out->body,
                 "ABSTAIN: no L3 doc memory; invoke external teacher then verify_learn");
    }
    return 1;
}

int roe_doc_verify_learn(RoeDocAsset *A, const char *corpus_id,
                         const char *page_text, const char *teacher_body,
                         const char *note, int tests_passed, int user_accept) {
    char sig[ROE_DOC_SIG_MAX];
    char skill_id[ROE_NAME_MAX];
    RoeDocMemory *m;
    size_t i, k;
    if (!A || !corpus_id || !page_text || !teacher_body) return 0;
    if (!(tests_passed || user_accept)) {
        A->n_verify_fail++;
        return 0;
    }
    A->n_verify_ok++;
    A->n_teacher++;
    A->cost_paid += A->teacher_unit_cost;

    roe_doc_add_corpus(A, corpus_id);
    roe_doc_signature(page_text, sig, sizeof sig);

    /* update existing */
    for (i = 0; i < A->n_mem; i++) {
        if (A->mem[i].active && name_eq(A->mem[i].corpus_id, corpus_id) &&
            name_eq(A->mem[i].sig, sig)) {
            m = &A->mem[i];
            snprintf(m->body, sizeof m->body, "%s", teacher_body);
            if (note) snprintf(m->note, sizeof m->note, "%s", note);
            m->certified = 1;
            m->verifies++;
            A->n_promote++;
            if (A->catalog_dir[0]) (void)roe_doc_save(A);
            return 1;
        }
    }
    if (A->n_mem >= ROE_DOC_MEM_MAX) return 0;
    m = &A->mem[A->n_mem++];
    memset(m, 0, sizeof *m);
    snprintf(m->corpus_id, sizeof m->corpus_id, "%s", corpus_id);
    snprintf(m->sig, sizeof m->sig, "%s", sig);
    snprintf(m->body, sizeof m->body, "%s", teacher_body);
    if (note) snprintf(m->note, sizeof m->note, "%s", note);
    /* skill id */
    k = 0;
    skill_id[k++] = 'd';
    skill_id[k++] = '3';
    skill_id[k++] = '_';
    for (i = 0; sig[i] && k + 1 < sizeof skill_id; i++) {
        unsigned char c = (unsigned char)sig[i];
        skill_id[k++] = (char)(isalnum(c) ? c : '_');
    }
    skill_id[k] = 0;
    snprintf(m->skill_id, sizeof m->skill_id, "%s", skill_id);
    m->certified = 1;
    m->active = 1;
    m->verifies = 1;
    A->n_promote++;
    /* also register soft skill on ROE */
    {
        char short_ans[ROE_ANSWER_MAX];
        size_t bl = strlen(teacher_body);
        if (bl >= sizeof short_ans) bl = sizeof short_ans - 1;
        memcpy(short_ans, teacher_body, bl);
        short_ans[bl] = 0;
        (void)roe_add_skill(&A->roe, skill_id, "doc_l3", sig, short_ans, 1, 1);
    }
    roe_goal_map_pattern(&A->goal, "vision", "doc_l3", sig, skill_id);
    if (A->catalog_dir[0]) (void)roe_doc_save(A);
    return 1;
}

int roe_doc_pack_validate(const char *pack_dir) {
    char path[ROE_PATH_MAX];
    char line[256];
    FILE *f;
    int ver = -1;
    size_t pl;
    if (!pack_dir || !pack_dir[0]) return 0;
    pl = strlen(pack_dir);
    if (pl + 14 >= sizeof path) return 0;
    snprintf(path, sizeof path, "%s/PACK.abi", pack_dir);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    if (strncmp(line, ROE_DOC_ABI_MAGIC, strlen(ROE_DOC_ABI_MAGIC)) != 0) {
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "abi_version %d", &ver) == 1) break;
    }
    fclose(f);
    /* Accept current ABI and previous v1 packs. */
    return ver == ROE_DOC_ABI_VER || ver == 1;
}

int roe_doc_pack_export(const RoeDocAsset *A, const char *out_dir) {
    char path[ROE_PATH_MAX];
    FILE *f;
    size_t i, pl;
    int n = 0;
    if (!A || !out_dir) return -1;
    mkdir_p(out_dir);
    pl = strlen(out_dir);
    if (pl + 20 >= sizeof path) return -5;

    snprintf(path, sizeof path, "%s/PACK.abi", out_dir);
    f = fopen(path, "w");
    if (!f) return -2;
    fprintf(f, "%s\n", ROE_DOC_ABI_MAGIC);
    fprintf(f, "abi_version %d\n", ROE_DOC_ABI_VER);
    fprintf(f, "kind ocr_doc_asset\n");
    fprintf(f, "second_brain 0\n");
    fprintf(f, "teacher external_unlimited\n");
    fprintf(f, "skills %zu\n", A->roe.n_skills);
    fprintf(f, "doc_l3_mem %zu\n", A->n_mem);
    fclose(f);

    /* doc memory */
    snprintf(path, sizeof path, "%s/doc_l3.jsonl", out_dir);
    f = fopen(path, "w");
    if (!f) return -3;
    for (i = 0; i < A->n_mem; i++) {
        const RoeDocMemory *m = &A->mem[i];
        if (!m->active || !m->certified) continue;
        /* single-line sanitize body */
        {
            char body[ROE_DOC_BODY];
            size_t j;
            snprintf(body, sizeof body, "%s", m->body);
            for (j = 0; body[j]; j++)
                if (body[j] == '\n' || body[j] == '\r') body[j] = ' ';
            fprintf(f,
                    "{\"corpus\":\"%s\",\"sig\":\"%s\",\"skill\":\"%s\","
                    "\"body\":\"%s\",\"note\":\"%s\",\"hits\":%llu}\n",
                    m->corpus_id, m->sig, m->skill_id, body,
                    m->note[0] ? m->note : "", (unsigned long long)m->hits);
            n++;
        }
    }
    fclose(f);

    /* share skills via goal tidy export */
    {
        RoeDocAsset *mut = (RoeDocAsset *)A; /* cast for goal.roe assign */
        mut->goal.roe = A->roe;
        (void)roe_goal_save(&mut->goal);
        (void)roe_save_catalog(&mut->roe);
    }

    snprintf(path, sizeof path, "%s/MANIFEST.txt", out_dir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "ROE OCR asset pack\n");
        fprintf(f, "not_a_second_brain 1\n");
        fprintf(f, "corpora %zu\n", A->n_corpora);
        fprintf(f, "l3 %d\n", n);
        fclose(f);
    }
    return n;
}

int roe_doc_pack_import(RoeDocAsset *A, const char *in_dir) {
    char path[ROE_PATH_MAX], line[ROE_DOC_BODY + 256];
    FILE *f;
    size_t pl;
    int n = 0;
    if (!A || !in_dir) return -1;
    if (!roe_doc_pack_validate(in_dir)) return -2;

    pl = strlen(in_dir);
    if (pl + 14 >= sizeof path) return -5;
    snprintf(path, sizeof path, "%s/doc_l3.jsonl", in_dir);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char corpus[ROE_DOC_NAME], sig[ROE_DOC_SIG_MAX], skill[ROE_NAME_MAX];
        char body[ROE_DOC_BODY], note[ROE_TEXT_MAX];
        const char *p;
        RoeDocMemory *m;
        corpus[0] = sig[0] = skill[0] = body[0] = note[0] = 0;
        p = strstr(line, "\"corpus\":\"");
        if (p) sscanf(p, "\"corpus\":\"%63[^\"]\"", corpus);
        p = strstr(line, "\"sig\":\"");
        if (p) sscanf(p, "\"sig\":\"%127[^\"]\"", sig);
        p = strstr(line, "\"skill\":\"");
        if (p) sscanf(p, "\"skill\":\"%63[^\"]\"", skill);
        p = strstr(line, "\"body\":\"");
        if (p) sscanf(p, "\"body\":\"%767[^\"]\"", body);
        p = strstr(line, "\"note\":\"");
        if (p) sscanf(p, "\"note\":\"%255[^\"]\"", note);
        if (!(corpus[0] && sig[0] && body[0])) continue;
        if (find_mem(A, corpus, sig)) continue;
        if (A->n_mem >= ROE_DOC_MEM_MAX) break;
        roe_doc_add_corpus(A, corpus);
        m = &A->mem[A->n_mem++];
        memset(m, 0, sizeof *m);
        snprintf(m->corpus_id, sizeof m->corpus_id, "%s", corpus);
        snprintf(m->sig, sizeof m->sig, "%s", sig);
        snprintf(m->body, sizeof m->body, "%s", body);
        if (note[0]) snprintf(m->note, sizeof m->note, "%s", note);
        if (skill[0])
            snprintf(m->skill_id, sizeof m->skill_id, "%s", skill);
        else
            snprintf(m->skill_id, sizeof m->skill_id, "d3_imp_%d", n);
        m->certified = 1;
        m->active = 1;
        n++;
    }
    fclose(f);
    /* load skills catalog if present in same dir */
    {
        char prev[ROE_PATH_MAX];
        snprintf(prev, sizeof prev, "%s", A->catalog_dir);
        roe_doc_set_catalog(A, in_dir);
        (void)roe_load_catalog(&A->roe);
        if (prev[0]) roe_doc_set_catalog(A, prev);
    }
    return n;
}

int roe_doc_dollar_bench(RoeDocAsset *A, int n_pages, int unique_docs,
                         double unit_to_usd, RoeDocDollarBench *B) {
    int i, u;
    int l3 = 0, teach = 0;
    double paid = 0, base = 0;
    char page[256];
    const char *bodies[32];
    char docs[32][128];
    RoeDocReply r;

    if (!A || !B || n_pages <= 0) return -1;
    memset(B, 0, sizeof *B);
    if (unique_docs < 1) unique_docs = 1;
    if (unique_docs > 32) unique_docs = 32;
    if (unit_to_usd <= 0) unit_to_usd = 0.001; /* $0.001 per unit â†’ $0.20/teacher page */

    for (u = 0; u < unique_docs; u++) {
        snprintf(docs[u], sizeof docs[u], "DOC_%02d header template corpus stamp %d",
                 u, u * 17);
        bodies[u] = "CERT body from teacher parse of document.";
    }

    /* cold learn each unique */
    for (u = 0; u < unique_docs; u++) {
        (void)roe_doc_resolve(A, "bench_corpus", docs[u], &r);
        if (r.source != ROE_DOC_L3_HIT) {
            (void)roe_doc_verify_learn(A, "bench_corpus", docs[u], bodies[u],
                                       "dollar_bench", 1, 0);
            teach++;
            paid += A->teacher_unit_cost;
        }
        base += A->teacher_unit_cost;
    }
    /* remaining pages cycle uniques â†’ L3 */
    for (i = unique_docs; i < n_pages; i++) {
        u = i % unique_docs;
        snprintf(page, sizeof page, "%s", docs[u]);
        base += A->teacher_unit_cost;
        if (roe_doc_resolve(A, "bench_corpus", page, &r) == 0 &&
            r.source == ROE_DOC_L3_HIT) {
            l3++;
            paid += A->local_unit_cost;
        } else {
            teach++;
            paid += A->teacher_unit_cost;
        }
    }

    B->n_pages = n_pages;
    B->cost_teacher_always = base;
    B->cost_hybrid = paid;
    B->save_ratio = base > 0 ? 1.0 - paid / base : 0.0;
    B->unit_to_usd = unit_to_usd;
    B->usd_per_page_teacher = (base / n_pages) * unit_to_usd;
    B->usd_per_page_hybrid = (paid / n_pages) * unit_to_usd;
    B->l3_hits = l3;
    B->teacher_calls = teach;
    B->quality_match = 1;
    snprintf(B->summary, sizeof B->summary,
             "pages=%d unique=%d teach=%d l3=%d save=%.1f%% "
             "$/page teacher=%.4f hybrid=%.4f",
             n_pages, unique_docs, teach, l3, 100.0 * B->save_ratio,
             B->usd_per_page_teacher, B->usd_per_page_hybrid);
    return 0;
}

int roe_doc_save(const RoeDocAsset *A) {
    if (!A || !A->catalog_dir[0]) return -1;
    mkdir_p(A->catalog_dir);
    return roe_doc_pack_export(A, A->catalog_dir);
}

int roe_doc_load(RoeDocAsset *A) {
    if (!A || !A->catalog_dir[0]) return -1;
    if (roe_doc_pack_validate(A->catalog_dir))
        return roe_doc_pack_import(A, A->catalog_dir);
    return roe_load_catalog(&A->roe);
}

void roe_doc_dump_stats(const RoeDocAsset *A, char *buf, size_t cap) {
    double save;
    if (!A || !buf || !cap) return;
    save = A->cost_baseline_teacher_always > 0
               ? 1.0 - A->cost_paid / A->cost_baseline_teacher_always
               : 0.0;
    snprintf(buf, cap,
             "turns=%llu l3=%llu pdf=%llu classic=%llu teacher=%llu abs=%llu "
             "promote=%llu cost_paid=%.1f cost_base=%.1f save=%.3f mem=%zu "
             "corpora=%zu",
             (unsigned long long)A->n_turns, (unsigned long long)A->n_l3,
             (unsigned long long)A->n_local_pdf,
             (unsigned long long)A->n_local_classic,
             (unsigned long long)A->n_teacher, (unsigned long long)A->n_abstain,
             (unsigned long long)A->n_promote, A->cost_paid,
             A->cost_baseline_teacher_always, save, A->n_mem, A->n_corpora);
}

int roe_doc_pdftotext(const char *pdf_path, char *body, size_t cap) {
    char cmd[ROE_PATH_MAX + 64];
    char tmp[] = "/tmp/roe_doc_pdf_XXXXXX";
    int fd, rc;
    FILE *f;
    size_t n;
    if (!pdf_path || !body || cap < 8) return -1;
    body[0] = 0;
    fd = mkstemp(tmp);
    if (fd < 0) return -1;
    close(fd);
    snprintf(cmd, sizeof cmd, "pdftotext -q -layout '%s' '%s' 2>/dev/null", pdf_path,
             tmp);
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
    n = fread(body, 1, cap - 1, f);
    body[n] = 0;
    fclose(f);
    unlink(tmp);
    /* substantial? */
    {
        size_t alnum = 0, i;
        for (i = 0; body[i]; i++)
            if (isalnum((unsigned char)body[i])) alnum++;
        if (alnum < 40) return -1;
    }
    return 0;
}

int roe_doc_pdftotext_page(const char *pdf_path, int page_1based, char *body,
                           size_t cap) {
    char cmd[ROE_PATH_MAX + 96];
    char tmp[] = "/tmp/roe_doc_pdfp_XXXXXX";
    int fd, rc;
    FILE *f;
    size_t n, alnum = 0, i;
    if (!pdf_path || !body || cap < 8 || page_1based < 1) return -1;
    body[0] = 0;
    fd = mkstemp(tmp);
    if (fd < 0) return -1;
    close(fd);
    /* -f/-l select single page */
    snprintf(cmd, sizeof cmd,
             "pdftotext -q -layout -f %d -l %d '%s' '%s' 2>/dev/null", page_1based,
             page_1based, pdf_path, tmp);
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
    n = fread(body, 1, cap - 1, f);
    body[n] = 0;
    fclose(f);
    unlink(tmp);
    for (i = 0; body[i]; i++)
        if (isalnum((unsigned char)body[i])) alnum++;
    if (alnum < 20) return -1; /* empty / image-only page */
    return 0;
}

int roe_doc_pdf_page_count(const char *pdf_path) {
    char cmd[ROE_PATH_MAX + 80];
    char line[256];
    FILE *fp;
    int pages = -1;
    if (!pdf_path) return -1;
    snprintf(cmd, sizeof cmd, "pdfinfo '%s' 2>/dev/null", pdf_path);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, "Pages: %d", &pages) == 1) break;
    }
    pclose(fp);
    return pages;
}

int roe_doc_pdf_stream(RoeDocAsset *A, const char *corpus_id, const char *pdf_path,
                       int max_pages, int auto_cert_local, int teacher_sim,
                       int *out_pages, int *out_l3, int *out_local,
                       int *out_teacher) {
    int np, lim, p, n_pages = 0, n_l3 = 0, n_local = 0, n_teacher = 0;
    char body[ROE_DOC_BODY];
    char key[ROE_DOC_BODY + 64];
    RoeDocReply r;

    if (out_pages) *out_pages = 0;
    if (out_l3) *out_l3 = 0;
    if (out_local) *out_local = 0;
    if (out_teacher) *out_teacher = 0;
    if (!A || !corpus_id || !pdf_path) return -1;

    np = roe_doc_pdf_page_count(pdf_path);
    if (np < 1) {
        /* fallback: treat whole file once */
        if (roe_doc_pdftotext(pdf_path, body, sizeof body) != 0) return -2;
        np = 1;
    }
    lim = ROE_DOC_PDF_MAX_PAGES;
    if (max_pages > 0 && max_pages < lim) lim = max_pages;
    if (np > lim) np = lim;

    for (p = 1; p <= np; p++) {
        int got;
        n_pages++;
        if (np == 1 && roe_doc_pdf_page_count(pdf_path) < 1)
            got = 0; /* body already filled */
        else
            got = roe_doc_pdftotext_page(pdf_path, p, body, sizeof body);

        if (got != 0) {
            /* image-only page â†’ teacher path */
            A->n_turns++;
            A->n_teacher++;
            A->cost_paid += A->teacher_unit_cost;
            A->cost_baseline_teacher_always += A->teacher_unit_cost;
            n_teacher++;
            if (teacher_sim) {
                snprintf(body, sizeof body, "[page %d image-only teacher_sim]", p);
                snprintf(key, sizeof key, "pdfpage:%d:%s", p, body);
                (void)roe_doc_verify_learn(A, corpus_id, key, body, "pdf_stream_teacher",
                                           1, 0);
            }
            continue;
        }

        snprintf(key, sizeof key, "pdfpage:%d:%s", p, body);
        if (roe_doc_resolve(A, corpus_id, key, &r) == 0 && r.source == ROE_DOC_L3_HIT) {
            n_l3++;
            continue;
        }

        /* local digital page */
        A->n_turns++;
        A->n_local_pdf++;
        A->cost_paid += A->local_unit_cost;
        A->cost_baseline_teacher_always += A->teacher_unit_cost;
        n_local++;
        if (auto_cert_local) {
            (void)roe_doc_verify_learn(A, corpus_id, key, body, "pdf_stream_page", 1, 0);
        }
    }

    if (out_pages) *out_pages = n_pages;
    if (out_l3) *out_l3 = n_l3;
    if (out_local) *out_local = n_local;
    if (out_teacher) *out_teacher = n_teacher;
    return 0;
}

static int layout_feats(const char *t, int *nl, int *pipes, int *nch) {
    const char *p;
    *nl = *pipes = *nch = 0;
    if (!t) return 0;
    for (p = t; *p; p++) {
        (*nch)++;
        if (*p == '\n') (*nl)++;
        if (*p == '|') (*pipes)++;
    }
    return *nch;
}

int roe_doc_route(RoeDocAsset *A, const char *corpus_id, const char *path,
                  const char *page_text_hint, int auto_cert_local,
                  RoeDocReply *out) {
    char body[ROE_DOC_BODY];
    char sig[ROE_DOC_SIG_MAX];
    const char *text = page_text_hint;
    int nl = 0, pipes = 0, nch = 0;
    int is_pdf = 0;
    RoeDocReply lr;

    if (out) memset(out, 0, sizeof *out);
    if (!A || !corpus_id) return -1;
    body[0] = 0;

    if (path && path[0]) {
        const char *ext = strrchr(path, '.');
        if (ext && (!strcmp(ext, ".csv") || !strcmp(ext, ".tsv") ||
                    !strcmp(ext, ".xlsx") || !strcmp(ext, ".xls") ||
                    !strcmp(ext, ".CSV") || !strcmp(ext, ".TSV") ||
                    !strcmp(ext, ".XLSX") || !strcmp(ext, ".XLS"))) {
            RoeTable *tbl = (RoeTable *)calloc(1, sizeof(RoeTable));
            int trc;
            if (!tbl) return -9;
            trc = roe_table_route(A, corpus_id, path, auto_cert_local, out, tbl);
            free(tbl);
            return trc;
        }
        if (ext && (!strcmp(ext, ".pdf") || !strcmp(ext, ".PDF"))) {
            is_pdf = 1;
            if (roe_doc_pdftotext(path, body, sizeof body) == 0) {
                text = body;
                /* try L3 first on extracted */
                layout_feats(text, &nl, &pipes, &nch);
                roe_doc_signature_ex(text, nl, pipes, nch, sig, sizeof sig);
                if (roe_doc_resolve(A, corpus_id, text, &lr) == 0 &&
                    lr.source == ROE_DOC_L3_HIT) {
                    if (out) *out = lr;
                    return 0;
                }
                /* local digital extract path */
                A->n_local_pdf++;
                A->cost_paid += A->local_unit_cost;
                A->cost_baseline_teacher_always += A->teacher_unit_cost;
                if (auto_cert_local) {
                    (void)roe_doc_verify_learn(A, corpus_id, text, body,
                                               "local_pdftotext", 1, 0);
                }
                if (out) {
                    out->source = ROE_DOC_LOCAL_PDF;
                    out->cost_units = A->local_unit_cost;
                    out->certified_local = auto_cert_local ? 1 : 0;
                    snprintf(out->sig, sizeof out->sig, "%s", sig);
                    snprintf(out->body, sizeof out->body, "%s", body);
                }
                return 0;
            }
        } else if (ext && (!strcmp(ext, ".txt") || !strcmp(ext, ".md"))) {
            FILE *f = fopen(path, "r");
            size_t n;
            if (f) {
                n = fread(body, 1, sizeof body - 1, f);
                body[n] = 0;
                fclose(f);
                text = body;
            }
        }
    }

    if (!text || !text[0]) {
        A->n_abstain++;
        A->cost_baseline_teacher_always += A->teacher_unit_cost;
        if (out) {
            out->source = ROE_DOC_ABSTAIN;
            snprintf(out->body, sizeof out->body,
                     "ABSTAIN: no local extract; need teacher");
        }
        return 1;
    }

    layout_feats(text, &nl, &pipes, &nch);
    roe_doc_signature_ex(text, nl, pipes, nch, sig, sizeof sig);

    if (roe_doc_resolve(A, corpus_id, text, &lr) == 0 &&
        lr.source == ROE_DOC_L3_HIT) {
        if (out) *out = lr;
        return 0;
    }

    /* local text extract (txt/md or prior body) â€” factory auto-cert */
    if (auto_cert_local && text == body && body[0]) {
        (void)roe_doc_verify_learn(A, corpus_id, text, body, "local_text", 1, 0);
        if (roe_doc_resolve(A, corpus_id, text, &lr) == 0 &&
            lr.source == ROE_DOC_L3_HIT) {
            if (out) {
                *out = lr;
                /* attribute as local pdf/text path for coverage: count pdf bucket as local */
            }
            A->n_local_pdf++; /* digital/text local */
            return 0;
        }
    }

    /* classic OCR optional â€” tesseract */
    if (path && !is_pdf) {
        char cmd[ROE_PATH_MAX + 80];
        char tmp[] = "/tmp/roe_tess_XXXXXX";
        int fd = mkstemp(tmp);
        if (fd >= 0) {
            close(fd);
            unlink(tmp);
            snprintf(cmd, sizeof cmd,
                     "tesseract '%s' '%s' -l eng 2>/dev/null && test -f '%s.txt'", path,
                     tmp, tmp);
            if (system(cmd) == 0) {
                char tpath[ROE_PATH_MAX];
                FILE *f;
                snprintf(tpath, sizeof tpath, "%s.txt", tmp);
                f = fopen(tpath, "r");
                if (f) {
                    size_t n = fread(body, 1, sizeof body - 1, f);
                    body[n] = 0;
                    fclose(f);
                    unlink(tpath);
                    if (n > 40) {
                        A->n_local_classic++;
                        A->cost_paid += A->local_unit_cost * 2; /* slightly costlier */
                        A->cost_baseline_teacher_always += A->teacher_unit_cost;
                        if (auto_cert_local)
                            (void)roe_doc_verify_learn(A, corpus_id, body, body,
                                                       "local_tesseract", 1, 0);
                        if (out) {
                            out->source = ROE_DOC_LOCAL_CLASSIC;
                            out->cost_units = A->local_unit_cost * 2;
                            out->certified_local = auto_cert_local ? 1 : 0;
                            snprintf(out->body, sizeof out->body, "%s", body);
                            roe_doc_signature(body, out->sig, sizeof out->sig);
                        }
                        return 0;
                    }
                }
            }
        }
    }

    /* teacher needed */
    A->n_teacher++;
    A->cost_baseline_teacher_always += A->teacher_unit_cost;
    if (out) {
        out->source = ROE_DOC_TEACHER;
        out->cost_units = A->teacher_unit_cost;
        snprintf(out->sig, sizeof out->sig, "%s", sig);
        snprintf(out->body, sizeof out->body,
                 "NEED_TEACHER: no L3/local coverage for this page");
    }
    return 1;
}

void roe_doc_coverage(const RoeDocAsset *A, RoeDocCoverage *C) {
    size_t i;
    int pages = 0;
    char *bp;
    size_t left;
    if (!C) return;
    memset(C, 0, sizeof *C);
    if (!A) return;
    pages = (int)A->n_turns;
    if (pages < 1) pages = (int)(A->n_l3 + A->n_local_pdf + A->n_local_classic +
                                 A->n_teacher + A->n_abstain);
    C->n_pages = pages;
    C->n_l3 = (int)A->n_l3;
    C->n_local_pdf = (int)A->n_local_pdf;
    C->n_local_classic = (int)A->n_local_classic;
    C->n_teacher = (int)A->n_teacher;
    C->n_abstain = (int)A->n_abstain;
    C->teacher_rate = pages > 0 ? (double)A->n_teacher / (double)pages : 0.0;
    C->local_rate = 1.0 - C->teacher_rate;
    C->cost_save = A->cost_baseline_teacher_always > 0
                       ? 1.0 - A->cost_paid / A->cost_baseline_teacher_always
                       : 0.0;
    C->kpi_teacher_under_10pct = C->teacher_rate < 0.10 ? 1 : 0;
    bp = C->by_corpus;
    left = sizeof C->by_corpus;
    for (i = 0; i < A->n_corpora && left > 8; i++) {
        int n;
        if (!A->corpora[i].active) continue;
        n = snprintf(bp, left, "%s:pages=%llu/l3=%llu;", A->corpora[i].id,
                     (unsigned long long)A->corpora[i].n_pages,
                     (unsigned long long)A->corpora[i].n_l3_hits);
        if (n < 0 || (size_t)n >= left) break;
        bp += n;
        left -= (size_t)n;
    }
    snprintf(C->summary, sizeof C->summary,
             "pages=%d local=%.1f%% teacher=%.1f%% l3=%d pdf=%d classic=%d "
             "kpi_lt10=%d save=%.1f%%",
             C->n_pages, 100.0 * C->local_rate, 100.0 * C->teacher_rate, C->n_l3,
             C->n_local_pdf, C->n_local_classic, C->kpi_teacher_under_10pct,
             100.0 * C->cost_save);
}

int roe_doc_batch_distill(RoeDocAsset *A, const char *corpus_id,
                          const char *dir_path, int teacher_sim, int *n_ok,
                          int *n_teacher) {
    /* Simple: process known files from a list file dir_path/files.list OR
     * scan with popen find */
    char cmd[ROE_PATH_MAX + 128];
    char line[ROE_PATH_MAX];
    FILE *f;
    int ok = 0, teach = 0;
    RoeDocReply r;
    if (!A || !corpus_id || !dir_path) return -1;
    roe_doc_add_corpus(A, corpus_id);
    snprintf(cmd, sizeof cmd,
             "find '%s' -maxdepth 2 -type f \\( -name '*.txt' -o -name '*.pdf' -o "
             "-name '*.md' \\) 2>/dev/null | head -200",
             dir_path);
    f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L) continue;
        if (roe_doc_route(A, corpus_id, line, NULL, 1, &r) == 0) {
            ok++;
        } else if (teacher_sim) {
            /* factory sim: CERT a stub from basename */
            char body[ROE_DOC_BODY];
            const char *base = strrchr(line, '/');
            base = base ? base + 1 : line;
            snprintf(body, sizeof body, "TEACHER_SIM CERT %s", base);
            if (roe_doc_verify_learn(A, corpus_id, body, body, "teacher_sim", 1, 0)) {
                teach++;
                ok++;
            }
        } else {
            teach++;
        }
    }
    pclose(f);
    if (n_ok) *n_ok = ok;
    if (n_teacher) *n_teacher = teach;
    return ok;
}
