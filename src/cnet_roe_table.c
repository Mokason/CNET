#include "../include/cnet_roe_table.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void cell_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (src[i] && i + 1 < cap) {
        char c = src[i];
        if (c == '\n' || c == '\r' || c == '|') c = ' ';
        dst[i++] = c;
    }
    dst[i] = 0;
}

static const char *guess_type(const char *s) {
    int i, digits = 0, dots = 0, alpha = 0;
    if (!s || !s[0]) return "empty";
    for (i = 0; s[i]; i++) {
        if (isdigit((unsigned char)s[i])) digits++;
        else if (s[i] == '.' || s[i] == ',') dots++;
        else if (isalpha((unsigned char)s[i])) alpha++;
    }
    if (alpha > 0) return "text";
    if (digits > 0 && dots <= 1) return dots ? "float" : "int";
    return "text";
}

void roe_table_finalize(RoeTable *T) {
    int r, c;
    size_t o = 0;
    char hdr[ROE_DOC_SIG_MAX];
    if (!T || !T->ok) return;
    /* markdown */
    T->markdown[0] = 0;
    if (T->n_cols < 1 || T->n_rows < 1) return;
    /* header line */
    o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, "|");
    for (c = 0; c < T->n_cols && o + 8 < sizeof T->markdown; c++) {
        const char *h = T->headers[c][0] ? T->headers[c] : T->cells[0][c];
        o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, " %s |", h);
    }
    o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, "\n|");
    for (c = 0; c < T->n_cols && o + 6 < sizeof T->markdown; c++)
        o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, " --- |");
    o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, "\n");
    for (r = 1; r < T->n_rows && o + 8 < sizeof T->markdown; r++) {
        o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, "|");
        for (c = 0; c < T->n_cols && o + 8 < sizeof T->markdown; c++)
            o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, " %s |",
                                  T->cells[r][c]);
        o += (size_t)snprintf(T->markdown + o, sizeof T->markdown - o, "\n");
    }
    /* schema */
    T->schema[0] = 0;
    o = 0;
    for (c = 0; c < T->n_cols && o + 40 < sizeof T->schema; c++) {
        const char *name = T->headers[c][0] ? T->headers[c] : "?";
        const char *ty = guess_type(T->n_rows > 1 ? T->cells[1][c] : "");
        o += (size_t)snprintf(T->schema + o, sizeof T->schema - o, "%s:%s%s", name, ty,
                              c + 1 < T->n_cols ? "," : "");
    }
    /* signature from header row */
    hdr[0] = 0;
    o = 0;
    for (c = 0; c < T->n_cols && o + 20 < sizeof hdr; c++) {
        const char *h = T->headers[c][0] ? T->headers[c] : T->cells[0][c];
        o += (size_t)snprintf(hdr + o, sizeof hdr - o, "%s|", h);
    }
    roe_doc_signature_ex(hdr, T->n_rows, T->n_cols, (int)strlen(hdr), T->sig,
                         sizeof T->sig);
}

int roe_table_parse_text(const char *text, char sep, RoeTable *T) {
    char line[2048];
    const char *p, *q;
    int r = 0, c, auto_sep = 0;
    if (!text || !T) return -1;
    memset(T, 0, sizeof *T);
    if (!sep) {
        auto_sep = 1;
        /* prefer tab if present in first line */
        p = text;
        while (*p && *p != '\n') {
            if (*p == '\t') {
                sep = '\t';
                break;
            }
            if (*p == ',' && sep != '\t') sep = ',';
            p++;
        }
        if (!sep) sep = ',';
    }
    (void)auto_sep;
    p = text;
    while (*p && r < ROE_TBL_MAX_ROWS) {
        size_t li = 0;
        while (*p && *p != '\n' && *p != '\r' && li + 1 < sizeof line)
            line[li++] = *p++;
        line[li] = 0;
        while (*p == '\n' || *p == '\r') p++;
        if (!line[0]) continue;
        c = 0;
        q = line;
        while (*q && c < ROE_TBL_MAX_COLS) {
            char cell[ROE_TBL_CELL];
            size_t ci = 0;
            if (*q == '"') {
                q++;
                while (*q && ci + 1 < sizeof cell) {
                    if (*q == '"' && q[1] == '"') {
                        cell[ci++] = '"';
                        q += 2;
                        continue;
                    }
                    if (*q == '"') {
                        q++;
                        break;
                    }
                    cell[ci++] = *q++;
                }
            } else {
                while (*q && *q != sep && ci + 1 < sizeof cell) cell[ci++] = *q++;
            }
            cell[ci] = 0;
            /* trim */
            while (ci && (cell[ci - 1] == ' ' || cell[ci - 1] == '\t'))
                cell[--ci] = 0;
            cell_copy(T->cells[r][c], sizeof T->cells[r][c], cell);
            if (*q == sep) q++;
            c++;
        }
        if (c > T->n_cols) T->n_cols = c;
        if (r == 0) {
            int cc;
            for (cc = 0; cc < c; cc++)
                cell_copy(T->headers[cc], sizeof T->headers[cc], T->cells[0][cc]);
        }
        r++;
    }
    T->n_rows = r;
    if (T->n_rows < 1 || T->n_cols < 1) return -1;
    T->ok = 1;
    roe_table_finalize(T);
    return 0;
}

int roe_table_load_path(const char *path, RoeTable *T) {
    const char *ext;
    char buf[65536];
    if (!path || !T) return -1;
    memset(T, 0, sizeof *T);
    ext = strrchr(path, '.');
    if (!ext) return -1;
    if (!strcmp(ext, ".csv") || !strcmp(ext, ".tsv") || !strcmp(ext, ".CSV") ||
        !strcmp(ext, ".TSV")) {
        FILE *f = fopen(path, "r");
        size_t n;
        if (!f) return -1;
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        return roe_table_parse_text(buf, !strcmp(ext, ".tsv") || !strcmp(ext, ".TSV")
                                             ? '\t'
                                             : ',',
                                    T);
    }
    if (!strcmp(ext, ".xlsx") || !strcmp(ext, ".xls") || !strcmp(ext, ".XLSX") ||
        !strcmp(ext, ".XLS")) {
        char cmd[ROE_PATH_MAX + 128];
        FILE *f;
        size_t n;
        /* prefer repo script */
        snprintf(cmd, sizeof cmd,
                 "python3 tools/roe_table_extract.py '%s' 2>/dev/null", path);
        f = popen(cmd, "r");
        if (!f) return -1;
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        pclose(f);
        if (n < 2) return -1;
        return roe_table_parse_text(buf, '\t', T);
    }
    return -1;
}

int roe_table_route(RoeDocAsset *A, const char *corpus_id, const char *path,
                    int auto_cert_local, RoeDocReply *out, RoeTable *T_out) {
    RoeTable T;
    RoeDocReply r;
    char key[ROE_DOC_BODY];
    if (out) memset(out, 0, sizeof *out);
    if (!A || !corpus_id || !path) return -1;
    if (roe_table_load_path(path, &T) != 0) {
        if (out) {
            out->source = ROE_DOC_TEACHER;
            snprintf(out->body, sizeof out->body,
                     "NEED_TEACHER: table extract failed for %s", path);
        }
        A->n_teacher++;
        return 1;
    }
    if (T_out) *T_out = T;

    /* L3 key: header signature as page text */
    snprintf(key, sizeof key, "TABLE %s\n%s", T.sig, T.schema);
    if (roe_doc_resolve(A, corpus_id, key, &r) == 0 && r.source == ROE_DOC_L3_HIT) {
        if (out) *out = r;
        return 0;
    }

    /* local table CERT */
    A->n_local_pdf++; /* count as local digital extract */
    A->cost_paid += A->local_unit_cost;
    A->cost_baseline_teacher_always += A->teacher_unit_cost;
    if (auto_cert_local) {
        char body[ROE_DOC_BODY];
        size_t ml = strlen(T.markdown);
        size_t sl = strlen(T.schema);
        const char *pfx = "\n\nSCHEMA: ";
        size_t pl = 10;
        size_t o = 0;
        if (ml > sizeof(body) - pl - sl - 1) ml = sizeof(body) - pl - sl - 1;
        memcpy(body, T.markdown, ml);
        o = ml;
        memcpy(body + o, pfx, pl);
        o += pl;
        if (sl > sizeof(body) - o - 1) sl = sizeof(body) - o - 1;
        memcpy(body + o, T.schema, sl);
        o += sl;
        body[o] = 0;
        (void)roe_doc_verify_learn(A, corpus_id, key, body, "local_table", 1, 0);
    }
    if (out) {
        out->source = ROE_DOC_LOCAL_PDF;
        out->certified_local = auto_cert_local ? 1 : 0;
        out->cost_units = A->local_unit_cost;
        snprintf(out->sig, sizeof out->sig, "%s", T.sig);
        {
            size_t ml = strlen(T.markdown);
            size_t sl = strlen(T.schema);
            size_t o = 0;
            if (ml > sizeof(out->body) - 12 - sl) ml = sizeof(out->body) - 12 - sl;
            memcpy(out->body, T.markdown, ml);
            o = ml;
            memcpy(out->body + o, "\n\nSCHEMA: ", 10);
            o += 10;
            if (sl > sizeof(out->body) - o - 1) sl = sizeof(out->body) - o - 1;
            memcpy(out->body + o, T.schema, sl);
            out->body[o + sl] = 0;
        }
    }
    return 0;
}
