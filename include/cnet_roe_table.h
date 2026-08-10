/* ROE table understanding — CSV/TSV/XLS(X) local-first.
 * Not a second brain: extract → structure → L3 CERT; teacher only on miss.
 */
#ifndef CNET_ROE_TABLE_H
#define CNET_ROE_TABLE_H

#include "cnet_roe_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_TBL_MAX_COLS 48
#define ROE_TBL_MAX_ROWS 256
#define ROE_TBL_CELL 64
#define ROE_TBL_MD 8192

typedef struct {
    int n_cols;
    int n_rows; /* including header row if present */
    char headers[ROE_TBL_MAX_COLS][ROE_TBL_CELL];
    char cells[ROE_TBL_MAX_ROWS][ROE_TBL_MAX_COLS][ROE_TBL_CELL];
    char markdown[ROE_TBL_MD];
    char schema[1024]; /* col types guess */
    char sig[ROE_DOC_SIG_MAX];
    int ok;
} RoeTable;

/* Parse CSV/TSV text into RoeTable; sep=',' or '\t' or 0=auto. */
int roe_table_parse_text(const char *text, char sep, RoeTable *T);

/* Load file: .csv/.tsv native; .xlsx/.xls via helper script. */
int roe_table_load_path(const char *path, RoeTable *T);

/* Emit markdown + schema + signature into T. */
void roe_table_finalize(RoeTable *T);

/* Route table file through doc asset: L3 → local extract CERT → teacher. */
int roe_table_route(RoeDocAsset *A, const char *corpus_id, const char *path,
                    int auto_cert_local, RoeDocReply *out, RoeTable *T_out);

#ifdef __cplusplus
}
#endif

#endif
