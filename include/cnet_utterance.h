/* CNET utterance — C-native sentence composition (never Teacher, never self-CERT).
 *
 * Fills phrase-bank templates from runtime state. Voice policy: LOCAL only by default.
 */
#ifndef CNET_UTTERANCE_H
#define CNET_UTTERANCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_UTTER_TEXT 768
#define CNET_UTTER_KEY 32
#define CNET_UTTER_VAL 160
#define CNET_UTTER_MAX_KV 24
#define CNET_UTTER_MAX_BANK 32
#define CNET_UTTER_PAT 96

typedef struct {
    char key[CNET_UTTER_KEY];
    char val[CNET_UTTER_VAL];
} CnetUtterKV;

typedef struct {
    /* free-form slots */
    CnetUtterKV kv[CNET_UTTER_MAX_KV];
    int n_kv;
    /* common fields (also mirrored into kv) */
    char name[48];
    char source[24];
    char skill[64];
    char domain[32];
    char pattern[CNET_UTTER_PAT];
    char base_answer[CNET_UTTER_TEXT];
    char chain_brief[CNET_UTTER_TEXT];
    double local_hit;
    double dopamine;
    double serotonin;
    double adenosine;
    int miss_n;
    int llm_n;
    int never_voice_llm; /* 1 = default: refuse speaking LLM drafts */
} CnetUtterState;

typedef struct {
    char id[40];
    char when[48];   /* match intent/tag: identity|status|miss|chain|generic */
    char tmpl[CNET_UTTER_TEXT];
} CnetUtterPhrase;

typedef struct {
    CnetUtterPhrase bank[CNET_UTTER_MAX_BANK];
    int n_bank;
    int ready;
} CnetUtterBank;

void cnet_utter_state_init(CnetUtterState *S);
int cnet_utter_state_set(CnetUtterState *S, const char *key, const char *val);
int cnet_utter_state_setf(CnetUtterState *S, const char *key, const char *fmt, ...);

void cnet_utter_bank_init_default(CnetUtterBank *B);
/* optional: load id\twhen\ttemplate lines */
int cnet_utter_bank_load_tsv(CnetUtterBank *B, const char *path);

/* Pick template by when-tag (identity/status/miss/chain/generic) or pattern heuristic */
const CnetUtterPhrase *cnet_utter_pick(const CnetUtterBank *B, const CnetUtterState *S,
                                       const char *when_hint);

/* Expand {key} slots into out. Returns 0 ok. */
int cnet_utter_compose(const CnetUtterBank *B, const CnetUtterState *S, const char *when_hint,
                       char *out, size_t cap);

/* Assemble a sentence from live slots and small function-word glue.
   This is not a phrase-bank blob and not residual GGUF text. */
int cnet_utter_compose_native(const CnetUtterState *S, const char *when_hint,
                              char *out, size_t cap);

/* 1 if spoken is well-formed, carries live slots, and is not a filled
   default-bank template. */
int cnet_utter_fluency_check(const CnetUtterBank *B, const CnetUtterState *S,
                             const char *when_hint, const char *spoken);

/* Voice gate: 1 = allowed to TTS this source under policy */
int cnet_utter_may_voice(const CnetUtterState *S, const char *source);

/* Format simple chain hops "A | B | C" into speakable brief */
int cnet_utter_chain_brief(const char *chain_raw, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_UTTERANCE_H */
