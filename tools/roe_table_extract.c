/* Extract spreadsheet → TSV on stdout for ROE table router.
 * Supports: .csv/.tsv passthrough, .xlsx via unzip+xml, .xls via libreoffice.
 *
 * Usage: roe_table_extract PATH
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

static int ends_with_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    size_t i;
    if (ls < lf) return 0;
    for (i = 0; i < lf; i++) {
        char a = (char)tolower((unsigned char)s[ls - lf + i]);
        char b = (char)tolower((unsigned char)suf[i]);
        if (a != b) return 0;
    }
    return 1;
}

static char *slurp_file(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = 0;
    fclose(f);
    if (out_n) *out_n = (size_t)sz;
    return buf;
}

static void emit_cell(const char *s) {
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r') putchar(' ');
        else putchar(*s);
    }
}

static int passthrough_delim(const char *path, char delim) {
    FILE *f = fopen(path, "r");
    char line[1 << 20];
    if (!f) return 2;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        int first = 1;
        char *p = line;
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        /* naive CSV: split on delim, strip quotes */
        while (*p) {
            char cell[4096];
            size_t o = 0;
            if (*p == '"') {
                p++;
                while (*p && o + 1 < sizeof cell) {
                    if (*p == '"' && p[1] == '"') { cell[o++] = '"'; p += 2; continue; }
                    if (*p == '"') { p++; break; }
                    cell[o++] = *p++;
                }
            } else {
                while (*p && *p != delim && o + 1 < sizeof cell) cell[o++] = *p++;
            }
            cell[o] = 0;
            if (!first) putchar('\t');
            first = 0;
            emit_cell(cell);
            if (*p == delim) p++;
        }
        putchar('\n');
    }
    fclose(f);
    return 0;
}

/* Extract a zip member to a temp file using unzip or PowerShell. */
static int zip_extract_member(const char *zip, const char *member, char *out_path, size_t out_sz) {
    char cmd[2048];
    int rc;
#ifdef _WIN32
    snprintf(out_path, out_sz, "roe_xlsx_tmp_%u.xml", (unsigned)(uintptr_t)out_path);
    /* Prefer tar (Windows 10+) which can read zip; fallback Expand-Archive is dir-based. */
    snprintf(cmd, sizeof cmd,
             "tar -xf \"%s\" -O \"%s\" > \"%s\" 2>nul", zip, member, out_path);
    rc = system(cmd);
    if (rc == 0) {
        struct stat st;
        if (stat(out_path, &st) == 0 && st.st_size > 0) return 0;
    }
    remove(out_path);
    snprintf(cmd, sizeof cmd,
             "powershell -NoProfile -Command "
             "\"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
             "$z=[IO.Compression.ZipFile]::OpenRead('%s'); "
             "$e=$z.GetEntry('%s'); "
             "if($e){$r=$e.Open(); $o=[IO.File]::Create('%s'); $r.CopyTo($o); "
             "$o.Close(); $r.Close()}; $z.Dispose()\"",
             zip, member, out_path);
    rc = system(cmd);
#else
    snprintf(out_path, out_sz, "/tmp/roe_xlsx_%d.xml", (int)getpid());
    snprintf(cmd, sizeof cmd, "unzip -p '%s' '%s' > '%s' 2>/dev/null", zip, member, out_path);
    rc = system(cmd);
#endif
    if (rc != 0) return -1;
    {
        struct stat st;
        if (stat(out_path, &st) != 0 || st.st_size == 0) return -1;
    }
    return 0;
}

static void xml_decode_entities(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '&') {
            if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; continue; }
            if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; continue; }
            if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; continue; }
            if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; continue; }
            if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* Collect text inside <t>...</t> concatenating. */
static void collect_t_texts(const char *si, char *out, size_t out_sz) {
    const char *p = si;
    size_t o = 0;
    out[0] = 0;
    while ((p = strstr(p, "<t")) != NULL) {
        const char *gt = strchr(p, '>');
        const char *end;
        if (!gt) break;
        if (gt[-1] == '/') { p = gt + 1; continue; } /* <t .../> */
        p = gt + 1;
        end = strstr(p, "</t>");
        if (!end) break;
        while (p < end && o + 1 < out_sz) out[o++] = *p++;
        p = end + 4;
    }
    out[o] = 0;
    xml_decode_entities(out);
}

static int load_shared_strings(const char *xml, char ***out, int *nout) {
    const char *p = xml;
    int cap = 256, n = 0;
    char **arr = (char **)malloc((size_t)cap * sizeof(char *));
    if (!arr) return -1;
    while ((p = strstr(p, "<si")) != NULL) {
        const char *end;
        char buf[8192];
        if (p[3] != '>' && p[3] != ' ' && p[3] != '/') { p += 3; continue; }
        end = strstr(p, "</si>");
        if (!end) break;
        {
            size_t L = (size_t)(end - p);
            char *chunk = (char *)malloc(L + 1);
            if (!chunk) break;
            memcpy(chunk, p, L);
            chunk[L] = 0;
            collect_t_texts(chunk, buf, sizeof buf);
            free(chunk);
        }
        if (n >= cap) {
            cap *= 2;
            arr = (char **)realloc(arr, (size_t)cap * sizeof(char *));
        }
        arr[n] = (char *)malloc(strlen(buf) + 1);
        strcpy(arr[n], buf);
        n++;
        p = end + 5;
    }
    *out = arr;
    *nout = n;
    return 0;
}

static int col_row(const char *ref, int *col, int *row) {
    int c = 0, r = 0;
    const char *p = ref;
    if (!p || !isalpha((unsigned char)*p)) return -1;
    while (isalpha((unsigned char)*p)) {
        c = c * 26 + (toupper((unsigned char)*p) - 'A' + 1);
        p++;
    }
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) r = r * 10 + (*p++ - '0');
    *col = c - 1;
    *row = r - 1;
    return 0;
}

typedef struct { int r, c; char *v; } Cell;

static int xlsx_to_tsv(const char *path) {
    char tmp_ss[512], tmp_sheet[512];
    char *ss_xml = NULL, *sheet_xml = NULL;
    char **ss = NULL;
    int nss = 0;
    Cell *cells = NULL;
    int ncells = 0, cap = 0;
    int max_r = 0, max_c = 0, r, c, i;
    const char *p;

    if (zip_extract_member(path, "xl/sharedStrings.xml", tmp_ss, sizeof tmp_ss) == 0) {
        ss_xml = slurp_file(tmp_ss, NULL);
        remove(tmp_ss);
        if (ss_xml) load_shared_strings(ss_xml, &ss, &nss);
        free(ss_xml);
    } else {
        remove(tmp_ss);
    }

    /* Prefer sheet1.xml */
    if (zip_extract_member(path, "xl/worksheets/sheet1.xml", tmp_sheet, sizeof tmp_sheet) != 0) {
        remove(tmp_sheet);
        fprintf(stderr, "xlsx: cannot extract worksheet\n");
        return 2;
    }
    sheet_xml = slurp_file(tmp_sheet, NULL);
    remove(tmp_sheet);
    if (!sheet_xml) return 2;

    p = sheet_xml;
    while (p && *p) {
        const char *c1 = strstr(p, "<c ");
        const char *c2 = strstr(p, "<c>");
        const char *end, *rattr, *tattr, *vtag, *vend;
        char ref[32] = {0};
        char t[8] = {0};
        char val[8192] = {0};
        int ci, ri;
        if (!c1 && !c2) break;
        if (!c1) p = c2;
        else if (!c2) p = c1;
        else p = (c1 < c2) ? c1 : c2;
        end = strstr(p, "</c>");
        if (!end) {
            end = strstr(p, "/>");
            if (!end) break;
        }
        rattr = strstr(p, "r=\"");
        if (rattr && rattr < end) {
            rattr += 3;
            {
                int k = 0;
                while (rattr[k] && rattr[k] != '"' && k < 31) { ref[k] = rattr[k]; k++; }
                ref[k] = 0;
            }
        }
        tattr = strstr(p, "t=\"");
        if (tattr && tattr < end) {
            tattr += 3;
            {
                int k = 0;
                while (tattr[k] && tattr[k] != '"' && k < 7) { t[k] = tattr[k]; k++; }
                t[k] = 0;
            }
        }
        vtag = strstr(p, "<v>");
        if (vtag && vtag < end) {
            vtag += 3;
            vend = strstr(vtag, "</v>");
            if (vend) {
                size_t L = (size_t)(vend - vtag);
                if (L >= sizeof val) L = sizeof val - 1;
                memcpy(val, vtag, L);
                val[L] = 0;
            }
        } else {
            /* inline string <is><t>... */
            const char *is = strstr(p, "<is>");
            if (is && is < end) collect_t_texts(is, val, sizeof val);
        }
        if (ref[0] && col_row(ref, &ci, &ri) == 0) {
            if (t[0] == 's' && val[0]) {
                int idx = atoi(val);
                if (idx >= 0 && idx < nss) snprintf(val, sizeof val, "%s", ss[idx]);
            }
            if (ncells >= cap) {
                cap = cap ? cap * 2 : 256;
                cells = (Cell *)realloc(cells, (size_t)cap * sizeof(Cell));
            }
            cells[ncells].r = ri;
            cells[ncells].c = ci;
            cells[ncells].v = (char *)malloc(strlen(val) + 1);
            strcpy(cells[ncells].v, val);
            if (ri > max_r) max_r = ri;
            if (ci > max_c) max_c = ci;
            ncells++;
        }
        p = end + 1;
    }
    free(sheet_xml);

    for (r = 0; r <= max_r; r++) {
        int any = 0;
        char **row = (char **)calloc((size_t)max_c + 1, sizeof(char *));
        for (i = 0; i < ncells; i++)
            if (cells[i].r == r) {
                row[cells[i].c] = cells[i].v;
                if (cells[i].v && cells[i].v[0]) any = 1;
            }
        if (any) {
            for (c = 0; c <= max_c; c++) {
                if (c) putchar('\t');
                if (row[c]) emit_cell(row[c]);
            }
            putchar('\n');
        }
        free(row);
    }

    for (i = 0; i < ncells; i++) free(cells[i].v);
    free(cells);
    for (i = 0; i < nss; i++) free(ss[i]);
    free(ss);
    return 0;
}

static int xls_via_libreoffice(const char *path) {
    char cmd[2048], outdir[256], csvpath[512];
#ifdef _WIN32
    snprintf(outdir, sizeof outdir, "roe_xls_tmp");
#else
    snprintf(outdir, sizeof outdir, "/tmp/roe_xls_%d", (int)getpid());
#endif
    MKDIR(outdir);
    snprintf(cmd, sizeof cmd,
             "libreoffice --headless --convert-to csv --outdir %s \"%s\" >/dev/null 2>&1",
             outdir, path);
    if (system(cmd) != 0) return 2;
    /* find first csv — use basename */
    {
        const char *base = path, *s;
        for (s = path; *s; s++)
            if (*s == '/' || *s == '\\') base = s + 1;
        snprintf(csvpath, sizeof csvpath, "%s/%s", outdir, base);
        /* replace extension with .csv */
        {
            char *dot = strrchr(csvpath, '.');
            if (dot) strcpy(dot, ".csv");
            else strcat(csvpath, ".csv");
        }
    }
    if (passthrough_delim(csvpath, ',') != 0) return 2;
    remove(csvpath);
    return 0;
}

int main(int argc, char **argv) {
    const char *path;
    if (argc < 2) {
        fprintf(stderr, "usage: roe_table_extract PATH\n");
        return 2;
    }
    path = argv[1];
    if (ends_with_ci(path, ".csv")) return passthrough_delim(path, ',');
    if (ends_with_ci(path, ".tsv")) return passthrough_delim(path, '\t');
    if (ends_with_ci(path, ".xlsx")) return xlsx_to_tsv(path);
    if (ends_with_ci(path, ".xls")) return xls_via_libreoffice(path);
    fprintf(stderr, "unsupported extension\n");
    return 2;
}
