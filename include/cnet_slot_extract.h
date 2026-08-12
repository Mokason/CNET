/* Pack-local slot extractors — Milestone C (ops/systemd only).
 *
 * Fixed grammars rewrite conversational parameterized ops phrasing onto
 * sealed CERT skill patterns. No open NLU. No auto-CERT. No malloc.
 *
 * Examples:
 *   "is cnet-web active"     → "systemctl --user status cnet-web"
 *   "status of cnetd"        → "systemctl --user status cnetd"
 *   "restart hermes-gateway" → "systemctl --user restart hermes-gateway"
 *   "is cnet-marble running" → "cnet-marble status"  (special sealed skill)
 *
 * Gate: make slot_extract → SLOT_EXTRACT_PASS
 */
#ifndef CNET_SLOT_EXTRACT_H
#define CNET_SLOT_EXTRACT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_SLOT_UNIT 96
#define CNET_SLOT_OUT 512

typedef enum {
    CNET_SLOT_ACT_NONE = 0,
    CNET_SLOT_ACT_STATUS = 1,
    CNET_SLOT_ACT_RESTART = 2,
    CNET_SLOT_ACT_START = 3,
    CNET_SLOT_ACT_STOP = 4
} CnetSlotAction;

typedef struct {
    int applied;
    CnetSlotAction action;
    char unit[CNET_SLOT_UNIT];
    char reason[48];
    char action_name[16];
} CnetSlotMeta;

/* Try ops/systemd fixed grammars. Returns 1 if out was rewritten. */
int cnet_slot_extract_ops(const char *in, char *out, size_t cap,
                          CnetSlotMeta *meta);

int cnet_slot_extract_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SLOT_EXTRACT_H */
