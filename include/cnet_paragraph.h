#ifndef CNET_PARAGRAPH_H
#define CNET_PARAGRAPH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* restaurant_inform_v1 — finite CamRest-shaped hop, native paragraph.
   Not residual speech. Not an ASI-5 compete unit. */

#define CNET_PARA_CONTRACT "restaurant_inform_v1"
#define CNET_PARA_RUBRIC "cnet_para_slottrue_v1"
#define CNET_PARA_SLOT 24
#define CNET_PARA_NAME 48
#define CNET_PARA_TEXT 384

typedef struct {
    char area[CNET_PARA_SLOT];
    char food[CNET_PARA_SLOT];
    char price[CNET_PARA_SLOT];
    char name[CNET_PARA_NAME];
} CnetRestRow;

/* 0 unique hit, 1 abstain (empty / ambiguous / unknown). */
int cnet_rest_lookup(const char *area, const char *food, const char *price,
                     CnetRestRow *row);

/* Closed-vocab wrap → slots. Unknown tokens ignored. 0 if any slot found. */
int cnet_rest_from_wrap(const char *wrap, CnetRestRow *query);

/* ≥2 sentences from a bound row. Refuses an unbound row. */
int cnet_rest_paragraph(const CnetRestRow *row, char *out, size_t cap);

/* 1 if spoken is slot-true, multi-sentence, and not residual. */
int cnet_para_slottrue_v1(const char *spoken, const CnetRestRow *row);

#ifdef __cplusplus
}
#endif

#endif
