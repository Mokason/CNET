/* Query normalize + conversational alias map (Milestone A).
 *
 * Deterministic C only — no embeddings, no soft CERT seal.
 * Aliases rewrite open phrasing onto sealed CERT skill patterns
 * (e.g. "introduce yourself" → "who are you") so LOCAL still hits
 * binary pattern match.
 *
 * Law: aliases never mint CERT; they only map to already-sealed patterns.
 * Probe short-circuit must still run on the original user string.
 *
 * Gate: make query_alias → QUERY_ALIAS_PASS
 */
#ifndef CNET_QUERY_ALIAS_H
#define CNET_QUERY_ALIAS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_QA_MAX_ALIASES 128
#define CNET_QA_PAT 96
#define CNET_QA_CANON 96
#define CNET_QA_OUT 512

typedef struct {
    char alias[CNET_QA_PAT];
    char canonical[CNET_QA_CANON];
    int active;
} CnetQueryAlias;

typedef struct {
    CnetQueryAlias aliases[CNET_QA_MAX_ALIASES];
    int n;
    int n_static;
    int loaded_file;
    char source[256];
} CnetQueryAliasTable;

typedef struct {
    int normalized;     /* 1 if normalize changed bytes */
    int alias_hit;      /* 1 if an alias rewrote the string */
    char matched_alias[CNET_QA_PAT];
    char canonical[CNET_QA_CANON];
} CnetQueryPrepareMeta;

/* Init compiled-in soul/ops conversational aliases. */
void cnet_query_alias_init(CnetQueryAliasTable *T);

/* Optional TSV overlay: alias_phrase<TAB>canonical_pattern  (# comments ok). */
int cnet_query_alias_load_file(CnetQueryAliasTable *T, const char *path);

/* Lowercase, expand contractions, collapse space, strip trailing ?!. */
void cnet_query_normalize(const char *in, char *out, size_t cap);

/* Longest word-boundary alias rewrite into out. Returns 1 if rewritten. */
int cnet_query_alias_apply(const CnetQueryAliasTable *T, const char *in,
                           char *out, size_t cap, CnetQueryPrepareMeta *meta);

/* normalize → alias. Always writes out. meta optional. */
void cnet_query_prepare(const CnetQueryAliasTable *T, const char *in, char *out,
                        size_t cap, CnetQueryPrepareMeta *meta);

/* Whole normalized phrase, allowing only bounded courtesy wrappers (e.g.
 * "please", "right now"). Refuses inputs >= CNET_QA_OUT bytes, never prefix
 * matches a truncated request. phrase must be a normalized literal. */
int cnet_query_phrase_is_whole(const char *query, const char *phrase);

/* 1 only for a whole identity question about being an llm/chatbot.
 * Mentioning a model and "you" elsewhere is not an identity request. */
int cnet_query_identity_bot(const char *normalized);

int cnet_query_alias_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_QUERY_ALIAS_H */
