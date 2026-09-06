/* Residual FFI converter — other-model sites → typed discrete rows.
 *
 * Residual until complete. Complete → CERT-shaped propose (auto_cert=false).
 * Admit is never this module. Floats cannot convert. Empty SHOW fails closed.
 *
 * Law: propose ≠ admit · claimed_cert never set here · not AGI.
 */
#ifndef CNET_FFI_CONVERT_H
#define CNET_FFI_CONVERT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_FFI_SITE 64
#define CNET_FFI_TOK 48
#define CNET_FFI_SHOW 192
#define CNET_FFI_MAX_OBS 8
#define CNET_FFI_ROW 160
#define CNET_FFI_REASON 96

typedef struct {
    char site[CNET_FFI_SITE]; /* named tensor / host site */
    char sent[CNET_FFI_TOK];  /* typed hop sent (lemma / id) */
    char got[CNET_FFI_TOK];   /* discrete form received */
    char show[CNET_FFI_SHOW]; /* mandatory audit English */
} CnetFfiObs;

typedef struct {
    int need_site;
    int need_sent;
    int need_got;
    int need_show;
    CnetFfiObs obs[CNET_FFI_MAX_OBS];
    int n_obs;
    int complete;
    int converted;
    int admitted; /* must stay 0 */
    char reason[CNET_FFI_REASON];
} CnetFfiConv;

void cnet_ffi_init(CnetFfiConv *C);
int cnet_ffi_require(CnetFfiConv *C, const char *port); /* site|sent|got|show */
int cnet_ffi_note(CnetFfiConv *C, const CnetFfiObs *o); /* residual note */
int cnet_ffi_complete(CnetFfiConv *C);                  /* 1 iff all ports filled */
/* CERT-shaped lemma<TAB>got row. 0 only if complete. Does not admit. */
int cnet_ffi_convert(CnetFfiConv *C, char *row, size_t cap);
/* Pending PROPOSE.json under dir. auto_cert=false. */
int cnet_ffi_propose(CnetFfiConv *C, const char *dir);
/* Gold file for evolve gold_file path. Not admit. Incomplete refused. */
int cnet_ffi_write_gold(CnetFfiConv *C, const char *packs, char *path_out,
                        size_t cap);
/* Must refuse. Converter never admits. */
int cnet_ffi_admit(CnetFfiConv *C);
int cnet_ffi_selftest(void);

typedef enum {
    CNET_FFI_CMD_NONE = 0,
    CNET_FFI_CMD_NOTE = 1,
    CNET_FFI_CMD_STATUS = 2,
    CNET_FFI_CMD_PROPOSE = 3,
    CNET_FFI_CMD_RESET = 4,
    CNET_FFI_CMD_GOLD = 5
} CnetFfiCmd;

/* Closed grammar. "ffi note site=X sent=Y got=Z" or positional.
 * who are you → NONE. */
int cnet_ffi_parse(const char *q, CnetFfiCmd *cmd, CnetFfiObs *o);
void cnet_ffi_init_ports(CnetFfiConv *C); /* init + require site/sent/got/show */
int cnet_ffi_status_line(const CnetFfiConv *C, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_FFI_CONVERT_H */
