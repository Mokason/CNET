#ifndef CNET_VSA_LEXICON_H
#define CNET_VSA_LEXICON_H

/* Learned lexicon for the wide topical space (Random Indexing).
 *
 * Every word keeps its quasi-orthogonal identity (the hash-seeded signature
 * the runtime already uses) and gains a context signature learned offline by
 * one sparse sweep over the retained teacher corpora: each occurrence adds the
 * sparse ternary index vectors of its window neighbours, cyclically permuted
 * by their offset so direction is kept, damped by neighbour frequency. The
 * stored vector is normalize(identity + beta * context), scaled by an inverse
 * document-frequency weight and quantised to int8 under one global scale, so
 * at query time a word costs one binary search and 2048 adds, no PRNG.
 *
 * Version 3 adds three learned extensions, all offline, all in the same file:
 *  - phrases: frequent adjacent content-word pairs get their own entry (key =
 *    cnet_vsa_lexicon_phrase_key of the two word keys), learned like a word
 *    from the pair's window context (and a distilled vector when given). At
 *    query time an adjacent pair costs one more binary search.
 *  - subword backoff: character n-grams of the vocabulary words carry the sign
 *    of the mean LEARNED context of the words containing them (fastText-style
 *    composition derived after learning, not random hashing). A word absent
 *    from the table is composed from its n-grams; only when none is known does
 *    it fall back to its hash identity.
 *  - training receipt: cnet_vsa_lexicon_train moves the stored vectors of the
 *    terms of (question, capsule) pairs toward the capsule's corpus centroid
 *    and away from the hardest other one; pair count, epochs, rate and margin
 *    are recorded in the header.
 *
 * The table is a versioned, digest-covered artifact. A capsule sealed with the
 * LEX encoder records the lexicon tag; a registry refuses such a capsule when
 * a different (or no) lexicon is active, because the geometry its radius was
 * certified in would be gone. */

#include <stddef.h>
#include <stdint.h>
#include "cnet_vsa_text.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_LEX_MAGIC    0x5845534Cu /* 'LSEX' */
#define CNET_VSA_LEX_VERSION  3u /* 3: phrases, subword backoff, training receipt (2026-09-12); v1/v2 tables are refused, rebuild them */
/* Distilled context file: magic 'DSTL', uint32 count, uint32 dim (2048), then
 * count x (uint64 key + dim float32). Produced offline from a transformer.
 * Keys are word keys or phrase keys. */
#define CNET_VSA_DISTILL_MAGIC 0x4C545344u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t dim;            /* CNET_VSA_TOPICAL_DIM */
    uint32_t count;          /* entries (words + phrases), sorted by key */
    uint32_t key_encoder;    /* encoder whose word key normalisation was used (STEM) */
    uint32_t window;         /* context window radius */
    uint32_t nnz;            /* nonzeros per sparse index vector */
    uint32_t min_count;
    float    beta;           /* context weight against identity */
    float    global_scale;   /* int8 = round(v * idf * global_scale) */
    uint32_t fallback_mag;   /* int8 magnitude of an identity-only vector (unknown word) */
    uint32_t reflective;     /* reflective passes applied after the sweep */
    float    max_df;         /* context words above this document frequency were ignored */
    uint32_t center;         /* 1 if context signatures were mean-centred */
    uint32_t remove_pcs;     /* top principal directions projected out after centring */
    uint32_t distilled;      /* keys (words + phrases) that received a distilled transformer context */
    float    distill_alpha;
    float    idf_floor;
    float    idf_power;
    uint32_t phrases;        /* entries that are adjacent-pair phrases */
    uint32_t phrase_min_count;
    float    phrase_weight;  /* folded into the phrase entries' int8 */
    uint32_t subwords;       /* CnetVsaLexiconSubword records stored after the entries */
    uint32_t subword_min_n;
    uint32_t subword_max_n;
    uint32_t subword_min_words;
    uint32_t subword_max_words;
    float    subword_weight; /* norm of a composed unknown word relative to an identity-only word */
    uint32_t trained_pairs;  /* (question, capsule) pairs the table was trained on; 0 = untrained */
    uint32_t trained_sentences; /* corpus sentences paired with their own centroid during training */
    uint32_t train_epochs;
    float    train_lr;
    float    train_margin;
    uint64_t sentences;
    uint64_t tokens;         /* content tokens swept */
    uint64_t corpus_hash;    /* order-free hash of the swept token stream */
    uint64_t digest;         /* FNV-1a over this header (digest zeroed), all entries and all subwords */
    uint32_t reserved[2];
} CnetVsaLexiconHeader;

typedef struct {
    uint64_t key;            /* FNV-1a of the normalised word, or a phrase key */
    uint32_t count;          /* corpus frequency */
    float    idf;            /* weight folded into q8 */
    int8_t   q8[CNET_VSA_TOPICAL_DIM];
} CnetVsaLexiconEntry;

typedef struct {
    uint64_t key;            /* FNV-1a of the n-gram bytes under the subword seed */
    uint32_t words;          /* vocabulary words containing the n-gram */
    uint32_t mag;            /* per-coordinate int8 magnitude: rarer n-grams weigh more */
    uint64_t bits[CNET_VSA_TOPICAL_WORDS]; /* sign of the mean learned context of those words */
} CnetVsaLexiconSubword;

typedef struct {
    CnetVsaLexiconHeader hdr;
    CnetVsaLexiconEntry *entries;     /* hdr.count, sorted by key */
    CnetVsaLexiconSubword *subwords;  /* hdr.subwords, sorted by key (NULL when 0) */
    uint64_t *keys;                   /* packed copies of the entry keys (built at load; binary search
                                         over 8-byte keys instead of 2 KB records keeps it in cache) */
    uint64_t *skeys;                  /* likewise for the subwords */
} CnetVsaLexicon;

typedef struct {
    uint32_t window;      /* default 3 */
    uint32_t nnz;         /* default 16 */
    uint32_t min_count;   /* default 2 */
    uint32_t max_vocab;   /* default 16384 */
    float    beta;        /* default 0.5 */
    uint32_t reflective;  /* default 0 */
    float    max_df;      /* default 0.30: words in more than this fraction of corpora do not act as contexts */
    uint32_t center;      /* default 1: subtract the mean context signature (removes the common direction) */
    uint32_t remove_pcs;  /* default 0: also project out the top-K principal directions of the signatures */
    const char *distilled; /* optional CNET_VSA_DISTILL file: per-key context vectors written offline from a
                             transformer (tools/cnet_vsa_lexicon_distill.py); NULL = pure Random Indexing */
    float distill_alpha;  /* context = (1-alpha) * random-indexing + alpha * distilled, for keys present (default 0.5) */
    float idf_floor;      /* default 0.25: minimum weight of a ubiquitous word (0.05 lets rare terms dominate a query) */
    float idf_power;      /* default 1.0: weight = idf^power */
    uint32_t max_phrases; /* default 0 (none): adjacent content-word pairs kept as entries, most frequent first */
    uint32_t phrase_min_count; /* default 3 */
    float    phrase_weight;    /* default 1.0 */
    uint32_t max_subwords;     /* default 0 (none): character n-grams kept for unknown-word composition */
    uint32_t subword_min_n;    /* default 3 */
    uint32_t subword_max_n;    /* default 5 */
    uint32_t subword_min_words; /* default 4: an n-gram seen in fewer vocabulary words is memorisation, not backoff */
    uint32_t subword_max_words; /* default 0 = 5% of the vocabulary: an n-gram seen in more words says nothing specific */
    float    subword_weight;   /* default 1.0 */
    const char *vocab_dump;    /* optional TSV of the words and phrases kept (kind, key, surface / word keys, count, df) */
} CnetVsaLexiconBuildOpts;

typedef struct {
    uint64_t files, sentences, tokens, distinct;
    uint32_t vocab;
    uint32_t phrases;
    uint32_t subwords;
    double   sweep_seconds;
} CnetVsaLexiconBuildReport;

typedef struct {
    uint32_t epochs;         /* default 8 */
    float    lr;             /* default 0.05 */
    float    margin;         /* default 0.10 (cosine) */
    uint64_t seed;           /* pair order */
    uint32_t sentences_per_corpus; /* default 0: also pair up to N of each corpus's own train sentences with
                                      its centroid (the contract itself), spread over the corpus; keeps the
                                      sentence geometry the router certifies on while questions are learned */
} CnetVsaLexiconTrainOpts;

typedef struct {
    uint32_t pairs, sentence_pairs, corpora, epochs, terms_touched, explicit_negatives;
    float    train_top1_before, train_top1_after;  /* on the training pairs themselves (a sanity number, not a result) */
    double   seconds;
} CnetVsaLexiconTrainReport;

void cnet_vsa_lexicon_build_opts_default(CnetVsaLexiconBuildOpts *o);
void cnet_vsa_lexicon_train_opts_default(CnetVsaLexiconTrainOpts *o);

/* Sweep every *_corpus.txt under corpus_dir and write the table. */
int cnet_vsa_lexicon_build(const char *corpus_dir, const CnetVsaLexiconBuildOpts *opts,
                           const char *out_path, CnetVsaLexiconBuildReport *report);

/* Supervised pass: pairs_tsv holds "<corpus name>\t<question>" lines where the
 * name matches <name>_corpus.txt under corpus_dir (the capsule's certified
 * corpus is its contract; its centroid is the target). Every term of a
 * question that is a table entry moves toward that centroid and away from the
 * hardest other centroid (or the explicit confusable capsule named in an optional
 * third column, when it violates the margin); centroids are recomputed each epoch from the moved
 * table; row norms (idf) are preserved. Writes a new table with a receipt. */
int cnet_vsa_lexicon_train(const char *in_path, const char *out_path, const char *corpus_dir,
                           const char *pairs_tsv, const CnetVsaLexiconTrainOpts *opts,
                           CnetVsaLexiconTrainReport *report);

/* Load and verify (magic, version, dim, digest). Fails closed. */
int cnet_vsa_lexicon_load(CnetVsaLexicon *lex, const char *path);
void cnet_vsa_lexicon_free(CnetVsaLexicon *lex);

/* Binary search by normalised word key; NULL if absent. */
const CnetVsaLexiconEntry *cnet_vsa_lexicon_find(const CnetVsaLexicon *lex, const char *key_word);
const CnetVsaLexiconEntry *cnet_vsa_lexicon_find_hash(const CnetVsaLexicon *lex, uint64_t key);
const CnetVsaLexiconSubword *cnet_vsa_lexicon_find_subword(const CnetVsaLexicon *lex, uint64_t key);

/* The lexicon the LEX encoder uses. NULL clears. The pointer must outlive use. */
void cnet_vsa_lexicon_set_active(const CnetVsaLexicon *lex);
/* Activate the lexicon a registry ships with: <dir>/registry.lex, else the
 * CNET_VSA_LEXICON environment path. A no-op when one is already active (a
 * mismatching one then refuses the capsules at admission, by design). Returns
 * 1 activated, 0 nothing to activate, <0 a table was present but did not
 * verify (nothing is activated: fail closed). */
int cnet_vsa_lexicon_activate_default(const char *dir);
/* Path of the table activated by cnet_vsa_lexicon_activate_default, or NULL. */
const char *cnet_vsa_lexicon_active_path(void);
const CnetVsaLexicon *cnet_vsa_lexicon_active(void);
/* Low 32 bits of the active lexicon digest; 0 when none is active. */
uint32_t cnet_vsa_lexicon_active_tag(void);

/* Word key normalisation shared with the runtime (stemmed content word). */
uint64_t cnet_vsa_lexicon_word_key(const char *token);
/* Hash of an already-normalised key word (what cnet_vsa_lexicon_find applies). */
uint64_t cnet_vsa_lexicon_key_hash(const char *key_word);
/* Key of the phrase formed by two adjacent content words (order matters). */
uint64_t cnet_vsa_lexicon_phrase_key(uint64_t first, uint64_t second);
/* Keys of the character n-grams of a normalised word (with boundary markers),
 * n in [min_n, max_n]; returns the number written (at most cap). */
int cnet_vsa_lexicon_subword_keys(const char *key_word, uint32_t min_n, uint32_t max_n,
                                  uint64_t *out, int cap);
/* Contribution of a word absent from the table, in the table's int8 scale:
 * the composed subword vector when the table has one and at least one n-gram
 * is known (returns 1), else the hash identity at the identity magnitude
 * (returns 0). out has CNET_VSA_TOPICAL_DIM lanes and is overwritten. */
int cnet_vsa_lexicon_oov_vector(const CnetVsaLexicon *lex, const char *key_word, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_LEXICON_H */
