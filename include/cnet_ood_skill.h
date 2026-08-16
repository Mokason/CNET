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
#define CNET_OOD_WIKI "wiki_inform_v1"
#define CNET_OOD_HELD CNET_HELD_CONTRACT
#define CNET_OOD_TITLE 96
#define CNET_OOD_EXTRACT 512

#define CNET_OOD_OP_ADD 1
#define CNET_OOD_OP_SUB 2
#define CNET_OOD_OP_MUL 3

/* Remaining content words as Wiki_Title. 0 if a title was written. */
int cnet_ood_subject(const char *turn, char *title, size_t cap);

/* Two integers + plus/minus/times. 0 if parsed. */
int cnet_ood_parse_arith(const char *turn, unsigned long *a, unsigned long *b,
                         int *op);

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
