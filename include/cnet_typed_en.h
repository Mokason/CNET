/* Typed English student — discrete slots, not word-CoT, not float CERT.
 *
 * Working state is ids (tense, lemma, hash, form). SHOW renders those ids
 * into English for the operator so hostility/anomaly is visible. Empty SHOW
 * is a fail-closed audit miss. Float blobs cannot persist (CNU 0/1 law).
 *
 * Law: never_self_cert · chain≠CERT · ABSTAIN on unknown lemma · not AGI.
 */
#ifndef CNET_TYPED_EN_H
#define CNET_TYPED_EN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_TE_LEMMA 32
#define CNET_TE_FORM 32
#define CNET_TE_SHOW 192
#define CNET_TE_AUDIT 768
#define CNET_TE_MAX_CAND 4
#define CNET_TE_MAX_VERBS 2048

typedef enum {
    CNET_TE_NONE = 0,
    CNET_TE_PAST = 1,
    CNET_TE_PART = 2,
    CNET_TE_ABSTAIN = 3,
    CNET_TE_PLURAL = 4,
    CNET_TE_ART = 5,
    CNET_TE_GERUND = 6, /* present participle / RULE=ing */
    CNET_TE_S3 = 7,     /* third person singular / RULE=s (verb) */
    CNET_TE_COMP = 8,   /* comparative / RULE=er */
    CNET_TE_SUPER = 9,  /* superlative / RULE=est */
    CNET_TE_ADV = 10,   /* adverb / RULE=ly */
    CNET_TE_UN = 11,    /* un- prefix / RULE=un */
    CNET_TE_RE = 12,    /* re- prefix / RULE=re */
    CNET_TE_NESS = 13,  /* -ness / RULE=ness */
    CNET_TE_ABLE = 14   /* -able / RULE=able */
} CnetTeKind;

typedef struct {
    int kind; /* CnetTeKind */
    char lemma[CNET_TE_LEMMA];
    char form[CNET_TE_FORM];
    uint64_t hash;
    int local; /* 1 = table or certified regular rule */
    int gap;   /* 1 = this candidate uncovered */
    int via_rule; /* 1 = regular -ed transducer, not exception table */
    char show[CNET_TE_SHOW]; /* mandatory operator audit line */
} CnetTeHop;

typedef struct {
    int n_cand;
    CnetTeHop cand[CNET_TE_MAX_CAND];
    int all_local;
    int any_gap;
    int claimed_cert; /* 1 only if all_local && every SHOW filled */
    int grammar_hit;  /* 1 = morphology grammar fired (not open NLU) */
    char show[CNET_TE_AUDIT];
} CnetTeResult;

void cnet_te_init(void);
int cnet_te_ask(const char *q, CnetTeResult *out);
/* Spoken mouth: lemma → form. No hashes. Gap named. */
int cnet_te_format_answer(const CnetTeResult *r, char *out, size_t cap);
int cnet_te_audit_ok(const CnetTeResult *r);
/* Discrete 0/1 occupancy dump (lemma table). Returns 0 and writes digest. */
int cnet_te_persist_discrete(const char *path, char *digest_hex, size_t cap);
/* Must refuse. Float thought is not a capsule. */
int cnet_te_persist_floats(const char *path);
int cnet_te_load_tsv(const char *path); /* overlay; skip duplicate lemmas */
/* Operator gold → live table + overlay TSV. Does not admit; next ask may LOCAL. */
int cnet_te_upsert(const char *lemma, const char *past, const char *part);
/* 1 = overlay applied, 0 = not morphology / already local / multi, -1 = refuse */
int cnet_te_gold_apply(const char *query, const char *answer, const char *tsv_path);
/* Teacher draft → one alphabetic form. Sentences / ABSTAIN refuse. */
int cnet_te_parse_teacher_form(const char *draft, char *form, size_t cap);
/* 1 = propose (teacher ≠ regular, not TABLE). 0 = skip. -1 = refuse.
 * Does not upsert and does not CERT. */
int cnet_te_teacher_consider(const char *lemma, const char *teacher_form);
typedef int (*CnetTeAskFn)(const char *q, char *out, size_t cap);
/* Harvest lemmas → propose TSV. ask=NULL skips live calls. Returns n new rows. */
int cnet_te_teacher_tick(const char *lemmas_path, const char *propose_path,
                         CnetTeAskFn ask);
int cnet_te_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_TYPED_EN_H */
