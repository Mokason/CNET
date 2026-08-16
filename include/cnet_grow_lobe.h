#ifndef CNET_GROW_LOBE_H
#define CNET_GROW_LOBE_H

/* grow_lobe_v1 — parallel train lobe beside Core.
   Ternary QAT student. External labels only. Does not serve as CERT.
   Not an ASI-5 unit. Anti-collapse: refuse Core Tier-A text. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_GROW_CONTRACT "grow_lobe_v1"
#define CNET_GROW_TEXT 512
#define CNET_GROW_PAIRS 512

typedef enum {
    CNET_GROW_SRC_NONE = 0,
    CNET_GROW_SRC_EXTERNAL = 1, /* teacher / leftover holder / verified tool */
    CNET_GROW_SRC_USER = 2,
    CNET_GROW_SRC_TOOL = 3,
    CNET_GROW_SRC_TIER_A = 4    /* Core certified — refuse */
} CnetGrowSource;

typedef struct {
    char text[CNET_GROW_TEXT];
    int target; /* next-token id in the tiny vocab, or -1 unused */
    CnetGrowSource source;
} CnetGrowPair;

typedef struct CnetGrowLobe CnetGrowLobe;

int cnet_grow_admit(const CnetGrowPair *pair); /* 0 admit, 1 refuse */

int cnet_grow_open(CnetGrowLobe **out);
void cnet_grow_close(CnetGrowLobe *l);

int cnet_grow_ingest(CnetGrowLobe *l, const CnetGrowPair *pair);
int cnet_grow_seed_english(CnetGrowLobe *l);
/* Qwen/Bonsai-style <|im_start|> / <|im_end|> turns (chat_template.jinja). */
int cnet_grow_seed_chat_template(CnetGrowLobe *l);
int cnet_grow_wrap_chat(char *out, size_t cap, const char *user,
                        const char *assistant);
/* Pull teacher English from the held model (8B or hook). EXTERNAL only.
   Stored as a chat-template turn, not bare prose. */
int cnet_grow_distill_held(CnetGrowLobe *l, const char *const *prompts, int n);
int cnet_grow_count(const CnetGrowLobe *l);
int cnet_grow_refused(const CnetGrowLobe *l);

int cnet_grow_train(CnetGrowLobe *l, int steps, double *loss_out);
int cnet_grow_train_last(CnetGrowLobe *l, int passes, double *loss_out);
/* Replay the newest `window` pairs in FP (does not flip QAT). */
int cnet_grow_train_recent(CnetGrowLobe *l, int window, int passes,
                           double *loss_out);
/* Next-char hit rate on stored teacher English. 0 ok. */
int cnet_grow_prefix_acc(CnetGrowLobe *l, double *acc_out);
/* Same metric on the newest `window` pairs (assistant body if wrapped). */
int cnet_grow_prefix_acc_recent(CnetGrowLobe *l, int window, double *acc_out);

/* After train: greedy char decode. claimed_cert stays 0. */
int cnet_grow_may_speak(const CnetGrowLobe *l);
int cnet_grow_speak(CnetGrowLobe *l, const char *prompt, char *out, size_t cap,
                    int n_new);
int cnet_grow_claimed_cert(const CnetGrowLobe *l);

#ifdef __cplusplus
}
#endif

#endif
