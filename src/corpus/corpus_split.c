/* Clean extracted text into training sentences: de-hyphenate line breaks, normalize
   whitespace, split on . ! ?, drop junk (too-short fragments, bare page numbers).
   Pure, no I/O. */
#include "../../include/corpus/corpus_split.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void strlist_init(StrList *s){ s->lines=NULL; s->count=0; s->cap=0; }
void strlist_push(StrList *s, const char *line){
    if (s->count==s->cap){ s->cap = s->cap? s->cap*2 : 16; s->lines=(char**)realloc(s->lines, s->cap*sizeof(char*)); }
    s->lines[s->count++] = strdup(line);
}
void strlist_free(StrList *s){ for(size_t i=0;i<s->count;i++) free(s->lines[i]); free(s->lines); strlist_init(s); }

static int is_pagenumber(const char *t){ for(const char*p=t;*p;p++) if(!isdigit((unsigned char)*p)) return 0; return *t!=0; }

int corpus_quality_keep(const char *s){
    size_t alpha_space=0,total=0;
    for(const char*p=s;*p;p++){ total++; if(isalpha((unsigned char)*p)||*p==' ') alpha_space++; }
    int words=0,toks=0;
    for(const char *p=s; *p; ){
        while(*p && !isalpha((unsigned char)*p)) p++;
        if(!*p) break;
        const char *st=p; int hasv=0;
        while(isalpha((unsigned char)*p)){ char c=(char)tolower((unsigned char)*p); if(strchr("aeiouy",c)) hasv=1; p++; }
        int len=(int)(p-st); toks++; if(len>=2 && hasv) words++;
    }
    double rw = toks? (double)words/toks : 0.0;
    double ar = total? (double)alpha_space/total : 0.0;
    return (toks>=3 && rw>=0.6 && ar>=0.85) ? 1 : 0;
}

void corpus_split(const char *text, StrList *out) {
    size_t n = strlen(text);
    char *buf = (char*)malloc(n+1); size_t b=0;
    /* 1) de-hyphenate "x-\n y" -> "xy"; normalize whitespace to single spaces. */
    for (size_t i=0;i<n;i++){
        char c = text[i];
        if (c=='-' && i+1<n && (text[i+1]=='\n'||text[i+1]=='\r')) {
            i++;
            while (i+1<n && (text[i+1]=='\n'||text[i+1]=='\r'||text[i+1]==' '||text[i+1]=='\t')) i++;
            continue;
        }
        if (c=='\n'||c=='\r'||c=='\t') c=' ';
        if (c==' ' && b>0 && buf[b-1]==' ') continue;
        buf[b++]=c;
    }
    buf[b]=0;
    /* 2) split on . ! ? ; trim; drop junk (len<6 or bare page number). */
    size_t start=0;
    for (size_t i=0;i<=b;i++){
        char c = buf[i];
        if (c=='.'||c=='!'||c=='?'||c==0){
            size_t end = (c==0)? i : i+1;          /* include terminator */
            while (start<end && buf[start]==' ') start++;
            if (end>start){
                size_t len = end-start;
                char *sent=(char*)malloc(len+1); memcpy(sent,buf+start,len); sent[len]=0;
                size_t L=strlen(sent); while(L>0&&sent[L-1]==' ') sent[--L]=0;
                if (strlen(sent)>=6 && !is_pagenumber(sent)) strlist_push(out, sent);
                free(sent);
            }
            start = i+1;
        }
    }
    free(buf);
}
