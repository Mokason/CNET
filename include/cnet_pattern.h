#ifndef CNET_PATTERN_H
#define CNET_PATTERN_H

/* Pattern runtime — fluid patterns that freeze into contract callouts.
 *
 * 4D pattern address (not a 2D N→blackbox→M grid):
 *   code   — discrete identity (token / hash / feature bag digest)
 *   role   — port tag / semantic lane (what kind of wire)
 *   time   — version + lifecycle (fluid → improving → frozen)
 *   place  — residency slot (hot / cold / unloaded)
 *
 * Edges are neural-like connections but explicit:
 *   in_addr → body → out_addr
 *   body ∈ { unit | tool | math | skill | hermetic }
 *   state ∈ { fluid | improving | frozen | demoted }
 *
 * Learning = move fluid edges; improve; freeze when verified absolute enough.
 * Callout = contract-style invoke of a frozen (or best fluid) edge.
 *
 * Gate: make pattern_runtime → PATTERN_RUNTIME_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_PATTERN_NAME_MAX 64
#define CNET_PATTERN_PATH_MAX 512
#define CNET_PATTERN_MAX_EDGES 256
#define CNET_PATTERN_MAX_HOT 32
#define CNET_PATTERN_CODE_DIM 8 /* fixed code vector (doubles as bits/ints) */

typedef enum {
    CNET_PAT_STATE_FLUID = 0,     /* candidate, still learning */
    CNET_PAT_STATE_IMPROVING = 1, /* verified some uses, not frozen */
    CNET_PAT_STATE_FROZEN = 2,    /* absolute enough — contract callout */
    CNET_PAT_STATE_DEMOTED = 3    /* failed check, do not prefer */
} CnetPatternState;

typedef enum {
    CNET_PAT_BODY_NONE = 0,
    CNET_PAT_BODY_UNIT = 1,     /* certified CNB unit name */
    CNET_PAT_BODY_TOOL = 2,     /* calculator | wiki | web | math_eval | … */
    CNET_PAT_BODY_MATH = 3,     /* math_solve method name */
    CNET_PAT_BODY_SKILL = 4,    /* SKILL.md path under skills dir */
    CNET_PAT_BODY_HERMETIC = 5  /* built-in identity / pass-through test */
} CnetPatternBodyKind;

/* 4D pattern address */
typedef struct {
    double code[CNET_PATTERN_CODE_DIM]; /* discrete-ish codes (0/1 or small ints as doubles) */
    char role[PORT_TAG_MAX];            /* semantic tag / lane */
    uint32_t version;                   /* time axis: bumps on improve */
    uint32_t place_slot;                /* residency index or UINT32_MAX if cold */
} CnetPatternAddr;

typedef struct {
    char name[CNET_PATTERN_NAME_MAX];
    CnetPatternAddr in_addr;
    CnetPatternAddr out_addr;
    Port in_port;
    Port out_port;
    CnetPatternState state;
    CnetPatternBodyKind body_kind;
    char body_ref[CNET_PATTERN_PATH_MAX]; /* unit/tool/method/path */
    uint32_t heat;
    uint32_t last_use; /* clock ticks */
    uint32_t successes;
    uint32_t failures;
    double reliability; /* successes / (successes+failures), frozen when high */
    uint64_t created_unix;
    uint64_t frozen_unix;
    int resident; /* 1 = hot slot occupied for this edge */
} CnetPatternEdge;

typedef struct {
    CnetPatternEdge edges[CNET_PATTERN_MAX_EDGES];
    size_t edge_count;
    int hot[CNET_PATTERN_MAX_HOT]; /* indices into edges[] that are resident */
    int hot_count;
    int hot_cap; /* max resident edges */
    uint32_t clock;
    char store_path[CNET_PATTERN_PATH_MAX];
    char skills_dir[CNET_PATTERN_PATH_MAX];
    char base_path[CNET_PATTERN_PATH_MAX]; /* CNB for unit bodies (optional) */
    void *soul_host;                       /* opaque SoulHost* when bound */
    int soul_owned;                        /* 1 = runtime must soul_close */
    double freeze_min_reliability; /* default 0.95 */
    uint32_t freeze_min_successes; /* default 3 */
} CnetPatternRuntime;

typedef struct {
    int found;
    int edge_index;
    CnetPatternState state;
    CnetPatternBodyKind body_kind;
    char body_ref[CNET_PATTERN_PATH_MAX];
    char answer[1024];
    char detail[256];
    int loaded;   /* residency load happened */
    int froze;    /* transitioned to frozen this call */
    double reliability;
} CnetPatternCalloutReport;

/* --- lifecycle --- */
CNET_API void cnet_pattern_runtime_init(CnetPatternRuntime *rt);
CNET_API void cnet_pattern_runtime_from_env(CnetPatternRuntime *rt);
CNET_API int cnet_pattern_runtime_load(CnetPatternRuntime *rt, const char *path);
CNET_API int cnet_pattern_runtime_save(const CnetPatternRuntime *rt, const char *path);

/* Encode a string query into a pattern address (stable FNV mix into code[]). */
CNET_API void cnet_pattern_addr_from_text(const char *text, const char *role,
                                          CnetPatternAddr *out);

/* Propose a fluid edge (learning). Returns edge index or -1. */
CNET_API int cnet_pattern_propose(CnetPatternRuntime *rt,
                                  const char *name,
                                  const CnetPatternAddr *in_addr,
                                  const CnetPatternAddr *out_addr,
                                  Port in_port, Port out_port,
                                  CnetPatternBodyKind body_kind,
                                  const char *body_ref);

/* Record success/failure; may promote fluid→improving→frozen. */
CNET_API int cnet_pattern_feedback(CnetPatternRuntime *rt, int edge_index,
                                   int success);

/* Force freeze if eligible (or force=1). */
CNET_API int cnet_pattern_freeze(CnetPatternRuntime *rt, int edge_index,
                                 int force);

/* Demote frozen/improving edge after failed callout. */
CNET_API int cnet_pattern_demote(CnetPatternRuntime *rt, int edge_index);

/* Residency: ensure edge is hot; may unload coldest LFRU victim. */
CNET_API int cnet_pattern_load(CnetPatternRuntime *rt, int edge_index);
CNET_API int cnet_pattern_unload(CnetPatternRuntime *rt, int edge_index);
CNET_API void cnet_pattern_tick(CnetPatternRuntime *rt); /* clock++ , optional heat decay */

/* Find best edge for query text + role (prefer frozen, then improving, then fluid). */
CNET_API int cnet_pattern_find(const CnetPatternRuntime *rt, const char *query,
                               const char *role);

/* Contract callout: find → load → execute body → feedback. */
CNET_API int cnet_pattern_callout(CnetPatternRuntime *rt, const char *query,
                                  const char *role,
                                  CnetPatternCalloutReport *rep);

/* Bootstrap: register built-in hermetic + math/tool bridges as fluid seeds. */
CNET_API int cnet_pattern_bootstrap_defaults(CnetPatternRuntime *rt);

/* SoulHost / CNB unit binding (optional — unit body callouts). */
CNET_API int cnet_pattern_bind_soul(CnetPatternRuntime *rt, void *soul_host);
CNET_API int cnet_pattern_bind_base(CnetPatternRuntime *rt, const char *base_path);
CNET_API void cnet_pattern_unbind_soul(CnetPatternRuntime *rt);
/* Import live CNB units as frozen UNIT edges (max_n <=0 means all that fit). */
CNET_API int cnet_pattern_import_units(CnetPatternRuntime *rt, int max_n);
/* Freeze all improving edges that meet reliability thresholds. Returns count. */
CNET_API int cnet_pattern_promote_all(CnetPatternRuntime *rt);
/* Propose a fluid curriculum edge (kind + text). */
CNET_API int cnet_pattern_propose_curriculum(CnetPatternRuntime *rt,
                                             const char *kind,
                                             const char *text);

/* Observe a verified math_solve / learn success and propose/improve edge. */
CNET_API int cnet_pattern_observe_math(CnetPatternRuntime *rt, const char *question,
                                       const char *method, const char *answer,
                                       int verified);
CNET_API int cnet_pattern_observe_skill(CnetPatternRuntime *rt, const char *query,
                                        const char *skill_path, int success);

CNET_API const char *cnet_pattern_state_name(CnetPatternState s);
CNET_API const char *cnet_pattern_body_name(CnetPatternBodyKind k);

/* Stats */
CNET_API size_t cnet_pattern_count(const CnetPatternRuntime *rt);
CNET_API size_t cnet_pattern_count_state(const CnetPatternRuntime *rt,
                                         CnetPatternState s);

#ifdef __cplusplus
}
#endif

#endif /* CNET_PATTERN_H */
