#ifndef CNET_OOD_SKILL_H
#define CNET_OOD_SKILL_H

/* ood_handle_v1 — leftover handler.
   Compute / wiki extract / held model. No fact table. */

#include "cnet_held_model.h"
#include "cnet_skill_lane.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_OOD_CONTRACT "ood_handle_v1"
#define CNET_OOD_ADD "add_u32_v1"
#define CNET_OOD_SUB "sub_u32_v1"
#define CNET_OOD_MUL "mul_u32_v1"
#define CNET_OOD_DIV "div_u32_v1"
#define CNET_OOD_MOD "mod_u32_v1"
#define CNET_OOD_CMP "cmp_u32_v1"
#define CNET_OOD_MIN "min_u32_v1"
#define CNET_OOD_MAX "max_u32_v1"
#define CNET_OOD_CLAMP "clamp_u32_v1"
#define CNET_OOD_CLOCK "clock_now_v1"
#define CNET_OOD_COMPOSE "compose_u32_v1"
#define CNET_OOD_HOST_LOAD "host_load_v1"
#define CNET_OOD_HOST_UP "host_uptime_v1"
#define CNET_OOD_HOST_DISK "host_disk_v1"
#define CNET_OOD_HOST_MEM "host_mem_v1"
#define CNET_OOD_HOST_GPU "host_gpu_v1"
#define CNET_OOD_HOST_TEMP "host_temp_v1"
#define CNET_OOD_AND "and_u32_v1"
#define CNET_OOD_OR "or_u32_v1"
#define CNET_OOD_XOR "xor_u32_v1"
#define CNET_OOD_SHL "shl_u32_v1"
#define CNET_OOD_SHR "shr_u32_v1"
#define CNET_OOD_DATE "date_v1"
#define CNET_OOD_MINUTES "minutes_to_seconds_v1"
#define CNET_OOD_CRC8 "crc8_atm_v1"
#define CNET_OOD_WIKI "wiki_inform_v1"
#define CNET_OOD_HELD CNET_HELD_CONTRACT
#define CNET_OOD_TITLE 96
#define CNET_OOD_EXTRACT 512

#define CNET_OOD_OP_ADD 1
#define CNET_OOD_OP_SUB 2
#define CNET_OOD_OP_MUL 3
#define CNET_OOD_OP_DIV 4
#define CNET_OOD_OP_MOD 5
#define CNET_OOD_OP_LT 6
#define CNET_OOD_OP_GT 7
#define CNET_OOD_OP_EQ 8
#define CNET_OOD_OP_MIN 9
#define CNET_OOD_OP_MAX 10
#define CNET_OOD_OP_CLAMP 11
#define CNET_OOD_OP_AND 12
#define CNET_OOD_OP_OR 13
#define CNET_OOD_OP_XOR 14
#define CNET_OOD_OP_SHL 15
#define CNET_OOD_OP_SHR 16

/* Remaining content words as Wiki_Title. 0 if a title was written. */
int cnet_ood_subject(const char *turn, char *title, size_t cap);

/* Two integers + plus/minus/times. 0 if parsed. */
int cnet_ood_parse_arith(const char *turn, unsigned long *a, unsigned long *b,
                         int *op);

/* Number words are overlay, not C. Digits stay encoding. */
void cnet_ood_numerals_clear(void);
int cnet_ood_load_numerals(const char *path); /* n loaded, <0 on error */
int cnet_ood_gold_numeral(const char *query, const char *answer,
                         const char *tsv_path); /* 1 applied, 0 not this domain */

/* 1 if binary arith (two operands + op), even when numerals are unknown. */
int cnet_ood_arith_shaped(const char *turn, int *op);

/* 0 if an unresolved operand word was written (overlay miss). */
int cnet_ood_gap_unknown(const char *turn, char *word, size_t cap);

/* Harvest miss_log arith gaps into propose TSV. n new words. Never CERT. */
int cnet_ood_numeral_tick(const char *miss_log, const char *propose_path,
                         const char *overlay_path);

/* Re-read config + $CNET_PACKS_ROOT overlay. Additive. */
void cnet_ood_numerals_reload(void);

/* JSON "extract":"..." unescape into out. 0 if bound. */
int cnet_ood_json_extract(const char *body, char *out, size_t cap);

/* Compute hop. 0 exact bind. */
int cnet_ood_try_add(const char *turn, CnetSkillLaneResult *out);

/* Fetch url (file:// only with ALLOW_FILE) and bind year or extract. */
int cnet_ood_try_wiki_url(const char *url, unsigned flags, int year_cue,
                          const char *subject, CnetSkillLaneResult *out);

/* Build an encyclopedia URL from the turn, then fetch+bind. */
int cnet_ood_try_wiki(const char *turn, unsigned flags, CnetSkillLaneResult *out);

int cnet_ood_try_held(const char *turn, CnetSkillLaneResult *out);

/* 0 = a hop answered. 1 = still unanswered. */
int cnet_ood_handle(const char *turn, CnetSkillLaneResult *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_OOD_SKILL_H */
