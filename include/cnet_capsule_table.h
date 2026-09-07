#ifndef CNET_CAPSULE_TABLE_H
#define CNET_CAPSULE_TABLE_H
#include "cnet_capsule.h"

/* One asset schema inside the existing schema-2 frontend.cvfa. Its bytes are
 * the complete canonical owner table, NOT an additional package format.
 * Authority is the operator's private data directory and explicit correction
 * declaration. SHA256 checks integrity/freshness; it does not authenticate a
 * third party or establish the truth of the supplied labels. */
#define CNET_CAPSULE_TABLE_SCHEMA 0x43544501u
#define CNET_CAPSULE_TABLE_MAX_BYTES 4096u
#define CNET_CAPSULE_TABLE_MAX_ROWS 256u
#define CNET_CAPSULE_SYMBOL_MAX_KEY 48u
#define CNET_CAPSULE_SYMBOL_MAX_LABEL 128u
typedef struct {
    char dataset[32], authority[16], sha256[65], unit[64];
    Port input, output;
    unsigned count;
    unsigned char keys[256];
    unsigned short values[256];
    int symbolic;
    /* Offsets preserve the exact source bytes, including literal spaces,
     * quotes and backslashes. The numeric compiler uses row ordinal -> itself. */
    unsigned short symbol_key_offsets[256], symbol_label_offsets[256];
    unsigned char symbol_key_lengths[256], symbol_label_lengths[256];
    size_t length;
    char source[CNET_CAPSULE_TABLE_MAX_BYTES + 1];
} CnetCapsuleTable;

/* ASCII [a-z][a-z0-9_]{0,30}; fixed source path ROOT/DATASET.tsv. */
int cnet_capsule_table_dataset(const char *dataset);
/* Symbol keys are exact ASCII [A-Za-z0-9_.:-]{1,48}, not normalized intent.
 * These helpers refuse numeric sources; unknown tokens never acquire a code. */
int cnet_capsule_table_symbol_index(const CnetCapsuleTable *table,const char *token,unsigned *row);
int cnet_capsule_table_key_at(const CnetCapsuleTable *table,unsigned row,char *text,size_t cap);
/* Call only after executing the source-bound certified unit and checking its
 * final freshness. A wrong returned ordinal is a refusal, never another label. */
int cnet_capsule_table_render(const CnetCapsuleTable *table,unsigned requested_row,
    unsigned returned_value,char *text,size_t cap);
/* Owner-controlled absolute root, nofollow traversal, private regular source,
 * single link, bounded stable read. Clears output on failure. The owner must
 * prevent concurrent source mutation throughout an answer; rechecking before
 * output is not a filesystem transaction or post-return freshness promise. */
int cnet_capsule_table_read(const char *root,const char *dataset,
    CnetCapsuleTable *out,char *error,size_t cap);
CnetCapsuleTable *cnet_capsule_table_parse(const void *asset,size_t length,
    const Contract *contract,const HybridCoverage *coverage,char *error,size_t cap);
int cnet_capsule_table_fresh(const CnetCapsuleTable *table,const char *root);
/* Equal complete ports require equal FULL identities; shorter tags never
 * authorize a collision. Separate version interfaces may coexist. */
int cnet_capsule_table_compatible(const CnetCapsuleTable *a,const CnetCapsuleTable *b);
/* Create one new directory with the existing CNB/manifest/asset, then reimport
 * and certify it at the unchanged .05 margin. Never overwrites. 0=durable,
 * 1=refused, 3=complete artifact retained after uncertain publication sync.
 * No service activation, external process, network or teacher is invoked. */
int cnet_capsule_table_build(const char *root,const char *dataset,const char *output);
#endif
