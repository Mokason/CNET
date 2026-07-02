#ifndef CORPUS_SPLIT_H
#define CORPUS_SPLIT_H
#include <stddef.h>
typedef struct { char **lines; size_t count, cap; } StrList;  /* realloc-grown */
void strlist_init(StrList *s);
void strlist_push(StrList *s, const char *line);   /* copies line */
void strlist_free(StrList *s);
/* text -> cleaned sentences appended to out. Pure, no I/O. */
void corpus_split(const char *text, StrList *out);
/* English-likeness gate: keep a sentence iff real-word fraction >= 0.6, token
   count >= 3, and alpha+space ratio >= 0.85. Returns 1 to keep, 0 to drop. */
int corpus_quality_keep(const char *sentence);
#endif
