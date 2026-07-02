#ifndef CORPUS_STORE_H
#define CORPUS_STORE_H
#include "corpus_split.h"   /* StrList */
/* Append each sentence as a line, skipping exact duplicates already present.
   Returns the number of NEW lines written. Creates the file if absent. */
size_t corpus_append(const char *path, const StrList *sentences);
/* Load the whole store into a growable list (one line per sentence). 0 on success. */
int    corpus_load(const char *path, StrList *out);
/* Number of lines currently stored (0 if absent). */
size_t corpus_count(const char *path);
#endif
