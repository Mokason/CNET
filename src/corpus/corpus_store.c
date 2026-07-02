/* Persistent, append-only, newline-delimited corpus store. The dynamic piece:
   the corpus accumulates across ingests, with exact-line dedup so re-ingesting a
   document is idempotent. Source-agnostic (any text producer can append). */
#include "../../include/corpus/corpus_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int corpus_load(const char *path, StrList *out) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    size_t cap=256, b=0; char *line=(char*)malloc(cap); int c;
    while ((c=fgetc(f))!=EOF){
        if (c=='\n'){ line[b]=0; if(b>0) strlist_push(out,line); b=0; }
        else { if(b+1>=cap){ cap*=2; line=(char*)realloc(line,cap); } line[b++]=(char)c; }
    }
    if (b>0){ line[b]=0; strlist_push(out,line); }
    free(line); fclose(f); return 0;
}

size_t corpus_count(const char *path) {
    StrList s; strlist_init(&s); if (corpus_load(path,&s)!=0) return 0;
    size_t n=s.count; strlist_free(&s); return n;
}

/* Minimal open-addressing string hash set (borrows the string pointers). */
static unsigned long hs_hash(const char *s){ unsigned long h=5381; int c; while((c=(unsigned char)*s++)) h=((h<<5)+h)^c; return h; }
typedef struct { const char **k; size_t cap, mask, n; } HSet;
static void hs_init(HSet *h, size_t hint){ size_t c=1024; while(c < hint*2) c<<=1; h->cap=c; h->mask=c-1; h->n=0;
    h->k=(const char**)calloc(c,sizeof(char*)); }
static void hs_free(HSet *h){ free(h->k); }
static int hs_add(HSet *h, const char *s){  /* returns 1 if newly added, 0 if present */
    size_t i = hs_hash(s) & h->mask;
    while (h->k[i]){ if (strcmp(h->k[i],s)==0) return 0; i=(i+1)&h->mask; }
    h->k[i]=s; h->n++; return 1;
}

size_t corpus_append(const char *path, const StrList *sentences) {
    StrList existing; strlist_init(&existing); corpus_load(path, &existing); /* ok if absent */
    HSet seen; hs_init(&seen, existing.count + sentences->count + 16);
    for (size_t j=0;j<existing.count;j++) hs_add(&seen, existing.lines[j]);
    FILE *f = fopen(path, "a"); if (!f){ hs_free(&seen); strlist_free(&existing); return 0; }
    size_t added=0;
    for (size_t i=0;i<sentences->count;i++){
        if (hs_add(&seen, sentences->lines[i])){ fprintf(f, "%s\n", sentences->lines[i]); added++; }
    }
    fclose(f); hs_free(&seen); strlist_free(&existing); return added;
}
