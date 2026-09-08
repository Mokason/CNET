#ifndef CNET_KB_RECALL_H
#define CNET_KB_RECALL_H
#include <stddef.h>

/* Read-only lexical retrieval of unverified legacy notes. Returns 1 for one
 * complete displayable match, 0 for no match, -1 for an unreadable/unsafe bank.
 * No truth, freshness, certification or training authority is conferred.
 * Regular JSONL bank <=8 MiB, query <512 bytes, decoded chunk <=2048 bytes.
 * Output is cleared on refusal; records which do not fit are never clipped. */
int cnet_kb_recall(const char *path, const char *query, char *out, size_t cap);
#endif
