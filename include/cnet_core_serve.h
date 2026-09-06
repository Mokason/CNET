#ifndef CNET_CORE_SERVE_H
#define CNET_CORE_SERVE_H

/* Plain .lut calculation bank (no GGUF convert, no admission).
 * Files carry no certification evidence; results are always uncertified.
 *
 * RESULT: calculate a valid nibble request or abstain.
 * Never OPEN_CHAT / never residual mouth.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_SERVE_MAX_BRICKS 24
#define CNET_SERVE_TAG 32
#define CNET_SERVE_NAME 64
#define CNET_SERVE_TEXT 256

typedef struct {
    char tag[CNET_SERVE_TAG];
    char name[CNET_SERVE_NAME];
    float lut[16];
    int live;
} CnetServeBrick;

typedef struct {
    CnetServeBrick bricks[CNET_SERVE_MAX_BRICKS];
    int n;
    char dir[512];
    size_t proves;
    size_t abstains;
    size_t reloads;
} CnetServeBank;

typedef struct {
    int proved;
    int abstained;
    int claimed_cert;
    unsigned in_nibble;
    unsigned out_nibble;
    char spoken[CNET_SERVE_TEXT];
    char brick[CNET_SERVE_NAME];
    char refusal[80];
} CnetServeResult;

void cnet_serve_bank_init(CnetServeBank *b);
int cnet_serve_bank_load_dir(CnetServeBank *b, const char *dir);
int cnet_serve_bank_reload(CnetServeBank *b); /* re-read dir */
int cnet_serve_save_lut(const char *dir, const char *tag, const char *name,
                        const float lut[16]);
int cnet_serve_result(CnetServeBank *b, const char *turn, CnetServeResult *out);
/* 1 if turn's leading token is a live brick tag (nibble may be invalid). */
int cnet_serve_owns(const CnetServeBank *b, const char *turn);
/* lut_b[lut_a[i]] into tag_out .lut. Never CERT residual. */
int cnet_serve_compose_tags(CnetServeBank *b, const char *tag_a, const char *tag_b,
                            const char *tag_out);

/* Process-global bank for cnetd. */
CnetServeBank *cnet_serve_global(void);
int cnet_serve_global_load_env(void); /* CNET_CORE_BUS_BRICKS_DIR */

#ifdef __cplusplus
}
#endif

#endif /* CNET_CORE_SERVE_H */
