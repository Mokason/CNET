/* Domain route table — CERT-first dispatch (Autonomous-ASI).
 *
 * Deterministic chain (single path):
 *   CERT pack → MTK .tskill → Base GGUF → Abstain
 *
 * Representation:
 *   - Static compiled defaults (always present)
 *   - Optional startup load from pack manifest into FIXED slots
 *   - Match path: no malloc, no realloc (scan only)
 *
 * Law: MTK is never chosen when a CERT rule matches.
 *      Fail-closed default = ABSTAIN (no MTK, no KV flush).
 *
 * Gate: make domain_route → DOMAIN_ROUTE_PASS
 */
#ifndef CNET_DOMAIN_ROUTE_H
#define CNET_DOMAIN_ROUTE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_DR_MAX_RULES 256
#define CNET_DR_PAT 96
#define CNET_DR_ID 80
#define CNET_DR_PATH 256

typedef enum {
    CNET_ROUTE_NONE = 0,
    CNET_ROUTE_CERT = 1,      /* fail-closed LOCAL pack */
    CNET_ROUTE_MTK = 2,       /* residual .tskill / CMSK weight adapter */
    CNET_ROUTE_BASE_GGUF = 3, /* plain host, no cartridge swap */
    CNET_ROUTE_ABSTAIN = 4    /* fail-closed: no residual specialty */
} CnetRouteKind;

typedef struct {
    char pattern[CNET_DR_PAT]; /* substring domain cue (lowercase match) */
    CnetRouteKind kind;
    char pack_or_skill[CNET_DR_ID]; /* CERT pack id or skill id */
    char mtk_path[CNET_DR_PATH];    /* .tskill path when kind=MTK */
    int min_pat_len;                /* 0 = use strlen(pattern); confidence floor */
    int active;
} CnetDomainRule;

typedef struct {
    CnetDomainRule rules[CNET_DR_MAX_RULES];
    int n_rules;       /* dynamic overlay count (after static) */
    int n_static;      /* compiled-in count */
    int loaded_file;   /* 1 if manifest loaded */
    char source[CNET_DR_PATH];
} CnetDomainRouter;

typedef struct {
    CnetRouteKind kind;
    char pattern[CNET_DR_PAT];
    char pack_or_skill[CNET_DR_ID];
    char mtk_path[CNET_DR_PATH];
    int pat_len;
    int conf_x1000; /* pattern_len * 1000 / max(query_len,1), capped 1000 */
    int rule_index; /* -1 if abstain default */
    const char *kind_name;
    const char *reason;
} CnetDomainDecision;

/* Init with compiled-in defaults only (no file I/O). */
void cnet_domain_route_init(CnetDomainRouter *R);

/* Load optional manifest (TSV or simple lines). Fixed slots only; truncates
 * excess. Does not allocate. Returns 0 ok, -1 open/parse soft-fail (keeps static). */
int cnet_domain_route_load_file(CnetDomainRouter *R, const char *path);

/* Match query: CERT tier first, then MTK, then BASE, else ABSTAIN.
 * Zero dynamic allocation. */
void cnet_domain_route_resolve(const CnetDomainRouter *R, const char *query,
                               CnetDomainDecision *out);

/* Helpers */
const char *cnet_route_kind_name(CnetRouteKind k);
int cnet_domain_route_n_rules(const CnetDomainRouter *R);

/* Selftest — returns 0 if PASS. */
int cnet_domain_route_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_DOMAIN_ROUTE_H */
