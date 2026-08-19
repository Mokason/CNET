#ifndef CNET_CORE_BUS_H
#define CNET_CORE_BUS_H

/* CORE bus — four verbs only.
 *
 *   LEASE    open weights as silent teacher (must unbind before serve)
 *   TABLE    build full-domain table; exact student ≥ 0.95; unbind teacher
 *   CERTIFY  specialist_admit the student (only after TABLE)
 *   RESULT   prove via CERT student(s), or ABSTAIN
 *
 * Product law:
 *   - Outside a table, abstain.
 *   - Creative may only emit a table (propose), never answer a turn.
 *   - Logical may only prove or abstain.
 *   - OPEN_CHAT / ROE / residual must NOT answer turns.
 *   - Residual never auto-CERT. Teacher leaves after TABLE.
 *   - Next domain only after current brick serves without LLM.
 *
 * Gate: make cnet_core_bus → CNET_CORE_BUS_PASS
 */

#include "cnet_dc_invent.h"
#include "cnet_weight_convert.h"
#include "nn.h"
#include "router.h"
#include "specialist.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_CORE_BUS_NAME 64
#define CNET_CORE_BUS_TEXT 768
#define CNET_CORE_BUS_MAX_BRICKS 8

typedef enum {
    CNET_CORE_BUS_IDLE = 0,
    CNET_CORE_BUS_LEASED = 1,
    CNET_CORE_BUS_TABLED = 2,
    CNET_CORE_BUS_CERTIFIED = 3
} CnetCoreBusState;

/* Parked CERT brick — serves without teacher/LLM. */
typedef struct {
    char name[CNET_CORE_BUS_NAME];
    char domain_tag[32]; /* turn prefix, e.g. q1_xor16 */
    float lut_table[16];
    BinaryTransformNetwork student;
    PrimitiveRegistry reg; /* per-brick admit isolation */
    Specialist specialist;
    int live;
    int certified;
} CnetCoreBrick;

typedef struct {
    CnetCoreBusState state;
    OracleRegistry oracles;
    OraclePolicy policy;
    CnetWeightFile file;
    CnetWeightOracle wo;
    BinaryTransformNetwork student;
    int student_live;
    float lut_table[16];
    int lut_live;
    PrimitiveRegistry reg;
    Specialist specialist;
    int certified;
    size_t residual_answer_blocked;
    size_t open_chat_blocked;
    size_t results_proved;
    size_t results_abstained;
    char domain[32];
    char name[CNET_CORE_BUS_NAME];
    char domain_tag[32];
    /* Installed bricks (after CERTIFY + park). */
    CnetCoreBrick bricks[CNET_CORE_BUS_MAX_BRICKS];
    int n_bricks;
} CnetCoreBus;

typedef struct {
    int proved;
    int abstained;
    int claimed_cert;
    unsigned in_nibble;
    unsigned out_nibble;
    char spoken[CNET_CORE_BUS_TEXT];
    char refusal[80];
    char brick[CNET_CORE_BUS_NAME];
} CnetCoreBusResult;

void cnet_core_bus_init(CnetCoreBus *b);
void cnet_core_bus_free(CnetCoreBus *b);

int cnet_core_bus_lease(CnetCoreBus *b, const char *gguf_path, const char *name);
int cnet_core_bus_table(CnetCoreBus *b, CnetWeightConvertReport *rep);
int cnet_core_bus_certify(CnetCoreBus *b);
int cnet_core_bus_result(CnetCoreBus *b, const char *turn, CnetCoreBusResult *out);
int cnet_core_bus_unbind(CnetCoreBus *b);
int cnet_core_bus_creative_emit_table(CnetCoreBus *b, CnetWeightConvertReport *rep);

/* Park certified student into brick bank; bus returns IDLE for next domain.
   Refuses if not CERTIFIED or bank full. Teacher must already be gone. */
int cnet_core_bus_park_brick(CnetCoreBus *b, const char *domain_tag);

/* Host Bonsai Q1_0 → domain LUT GGUF.
   mode 0: lut[i]=(i+bit)&15   mode 1: lut[i]=(i^bit)&15 (bit in {0,1}) */
int cnet_core_bus_write_q1_domain_from_host(const char *bonsai_gguf,
                                            const char *tensor_name,
                                            const char *out_gguf);
int cnet_core_bus_write_q1_domain_from_host_ex(const char *bonsai_gguf,
                                               const char *tensor_name,
                                               const char *out_gguf, int mode);

/* Full brick pipeline: write domain → lease → table → certify → park.
   Returns 0 if brick serves without teacher. */
int cnet_core_bus_make_brick(CnetCoreBus *b, const char *bonsai_gguf,
                             const char *tensor_name, const char *domain_gguf,
                             const char *brick_name, const char *domain_tag,
                             int mode, CnetWeightConvertReport *rep);

/* miss-log → e-graph propose only (never admit). */
int cnet_core_bus_misslog_propose(const char *miss_path, char *name_out,
                                  size_t name_cap, CnetDcTerm *brick_out);

/* Optional process bus for cnetd (NULL until loaded). */
CnetCoreBus *cnet_core_bus_global(void);
int cnet_core_bus_global_load_env(void); /* CNET_CORE_BUS_BRICKS_DIR */

#ifdef __cplusplus
}
#endif

#endif /* CNET_CORE_BUS_H */
