/* PDF font /Differences recovery: parse code->glyph maps, glyph->text (AGL-lite,
   incl. ligatures), and per-stream best-match decode (the candidate map that
   maximizes English-likeness wins). See docs/superpowers/specs. */
#include "../../include/pdf/font_decode.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

void glyph_to_text(const char *name, char *out, size_t cap){
    if(cap) out[0]=0;
    if(!name||!*name||cap<2) return;
    if(strchr(name,'_')){                         /* ligature/compound: split on '_' */
        size_t o=0; const char *p=name;
        while(*p && o+1<cap){ const char *st=p; while(*p && *p!='_') p++;
            char part[64]; int len=(int)(p-st); if(len>63) len=63; memcpy(part,st,len); part[len]=0;
            char t[8]; glyph_to_text(part,t,sizeof(t));
            for(int k=0;t[k]&&o+1<cap;k++) out[o++]=t[k];
            if(*p=='_') p++; }
        out[o]=0; return;
    }
    if(strlen(name)==1){ char c=name[0];
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')){ out[0]=c; out[1]=0; return; } }
    static const struct { const char*n; const char*t; } T[] = {
        {"space"," "},{"period","."},{"comma",","},{"hyphen","-"},{"colon",":"},{"semicolon",";"},
        {"exclam","!"},{"question","?"},{"parenleft","("},{"parenright",")"},{"quotesingle","'"},
        {"quotedbl","\""},{"slash","/"},{"percent","%"},{"ampersand","&"},{"endash","-"},{"emdash","-"},
        {"quoteright","'"},{"quoteleft","'"},{"quotedblleft","\""},{"quotedblright","\""},{"bullet","-"},
        {"zero","0"},{"one","1"},{"two","2"},{"three","3"},{"four","4"},{"five","5"},
        {"six","6"},{"seven","7"},{"eight","8"},{"nine","9"},
        {"fi","fi"},{"fl","fl"},{"ff","ff"},{"ffi","ffi"},{"ffl","ffl"},{0,0} };
    for(int i=0;T[i].n;i++) if(strcmp(T[i].n,name)==0){ snprintf(out,cap,"%s",T[i].t); return; }
    if(strncmp(name,"uni",3)==0 && strlen(name)>=7){ int v=0,ok=1;
        for(int k=3;k<7;k++){ char c=name[k]; int d=(c>='0'&&c<='9')?c-'0':(c>='A'&&c<='F')?c-'A'+10:(c>='a'&&c<='f')?c-'a'+10:-1;
            if(d<0){ ok=0; break; } v=v*16+d; }
        if(ok && v>=0x20 && v<0x7f){ out[0]=(char)v; out[1]=0; return; } }
    out[0]=0;   /* unknown -> drop */
}

void font_diffs_parse(const unsigned char *pdf, size_t n, FontDiffs *fd){
    fd->n=0;
    const char *p=(const char*)pdf;
    for(size_t i=0;i+12<n && fd->n<16;i++){
        if(memcmp(p+i,"/Differences",12)!=0) continue;
        size_t j=i+12; while(j<n && p[j]!='[' && p[j]!='>') j++;
        if(j>=n || p[j]!='[') continue;
        j++;
        DiffMap *dm=&fd->maps[fd->n]; memset(dm,0,sizeof(*dm));
        int code=0;
        while(j<n && p[j]!=']'){
            if(p[j]==' '||p[j]=='\r'||p[j]=='\n'||p[j]=='\t'){ j++; continue; }
            if(p[j]>='0'&&p[j]<='9'){ code=0; while(j<n&&p[j]>='0'&&p[j]<='9'){ code=code*10+(p[j]-'0'); j++; } }
            else if(p[j]=='/'){ j++; char name[64]; int k=0;
                while(j<n && p[j]!='/' && p[j]!=']' && p[j]!=' ' && p[j]!='\r' && p[j]!='\n' && p[j]!='\t'){ if(k<63) name[k++]=p[j]; j++; }
                name[k]=0;
                if(code>=0 && code<256){ char t[8]; glyph_to_text(name,t,sizeof(t));
                    if(t[0]){ dm->set[code]=1; snprintf(dm->text[code],8,"%s",t); } code++; }
            }
            else j++;
        }
        fd->n++;
    }
}

size_t decode_codes(const unsigned char *raw, size_t n, const DiffMap *map, char *out, size_t cap){
    size_t o=0;
    for(size_t i=0;i<n;i++){ unsigned char c=raw[i];
        if(map && map->set[c]){ const char *t=map->text[c]; for(size_t k=0;t[k];k++) if(o+1<cap) out[o++]=t[k]; }
        else if((c>=0x20&&c<0x7f)||c=='\n'||c=='\r'||c=='\t'){ if(o+1<cap) out[o++]=(char)c; }
    }
    if(o<cap) out[o]=0;
    return o;
}

double english_likeness(const char *s){
    int words=0, toks=0;
    for(const char *p=s; *p; ){
        while(*p && !isalpha((unsigned char)*p)) p++;
        if(!*p) break;
        const char *st=p; int hasv=0;
        while(isalpha((unsigned char)*p)){ char c=(char)tolower((unsigned char)*p); if(strchr("aeiouy",c)) hasv=1; p++; }
        int len=(int)(p-st); toks++; if(len>=2 && hasv) words++;
    }
    return toks? (double)words/toks : 0.0;
}

size_t font_decode_pick_best(const unsigned char *raw, size_t rawlen, const FontDiffs *fd, char *out, size_t cap){
    static char cand[1<<20];
    size_t bl=decode_codes(raw,rawlen,NULL,out,cap);   /* WinAnsi into out */
    double best=english_likeness(out); int best_rec=0;
    for(int m=0; m<fd->n; m++){
        size_t cl=decode_codes(raw,rawlen,&fd->maps[m],cand,sizeof(cand));
        double sc=english_likeness(cand);
        int rec=0; for(size_t i=0;i<rawlen;i++) if(fd->maps[m].set[raw[i]]) rec++;
        /* primary: English-likeness; tiebreak: more recovered font codes (so a
           1-word stream's ligature still gets recovered when likeness ties). */
        if(sc > best + 1e-9 || (sc > best - 1e-9 && rec > best_rec)){
            best=sc; best_rec=rec;
            size_t k=(cap>0 && cl<cap-1)?cl:(cap>0?cap-1:0); memcpy(out,cand,k); out[k]=0; bl=k;
        }
    }
    return bl;
}
