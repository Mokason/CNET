/* Text-born PDF text extractor: pulls strings from content streams (uncompressed or
   /FlateDecode) via the Tj/TJ operators, with a kerning-based word-space heuristic.
   Heuristic, not a full PDF parser; detects-and-skips encrypted/no-text PDFs. */
#include "../../include/pdf/pdf_extract.h"
#include "../../include/pdf/inflate.h"
#include "../../include/pdf/font_decode.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define SPACE_KERN 120.0   /* |kern| above this (text units) => a word gap */

/* Emit a raw code byte. The WinAnsi/Differences decode + ASCII filter happens
   later in decode_codes / font_decode_pick_best (best-match per stream). NUL is
   dropped to keep strlen-based consumers safe. */
static void emit(char *out, size_t cap, size_t *o, char c) {
    if (c == 0) return;
    if (*o + 1 < cap) out[(*o)++] = c;
}

/* Decode a (...) literal: s[*i]=='(' on entry; append text; advance *i past ')'. */
static void decode_literal(const unsigned char *s, size_t n, size_t *i,
                           char *out, size_t cap, size_t *o) {
    (*i)++; int depth = 1;
    while (*i < n && depth > 0) {
        unsigned char c = s[*i];
        if (c == '\\') {
            (*i)++; if (*i >= n) break;
            unsigned char e = s[*i];
            if      (e=='n') emit(out,cap,o,'\n');
            else if (e=='r') emit(out,cap,o,'\r');
            else if (e=='t') emit(out,cap,o,'\t');
            else if (e=='(') emit(out,cap,o,'(');
            else if (e==')') emit(out,cap,o,')');
            else if (e=='\\') emit(out,cap,o,'\\');
            else if (e>='0'&&e<='7') {
                int v=0,k=0;
                while (k<3 && *i<n && s[*i]>='0'&&s[*i]<='7') { v=v*8+(s[*i]-'0'); (*i)++; k++; }
                (*i)--; emit(out,cap,o,(char)v);
            }
            else emit(out,cap,o,(char)e);
            (*i)++;
        } else if (c=='(') { depth++; emit(out,cap,o,'('); (*i)++; }
        else if (c==')') { depth--; if (depth>0) emit(out,cap,o,')'); (*i)++; }
        else { emit(out,cap,o,(char)c); (*i)++; }
    }
}

/* Pull text from a decoded content stream: (..) Tj, [ .. ] TJ, <hex> strings. */
static void process_content(const unsigned char *s, size_t n, char *out, size_t cap, size_t *o) {
    for (size_t i = 0; i < n; ) {
        if (s[i]=='[') {                         /* TJ array: strings + kern numbers */
            i++;
            while (i < n && s[i] != ']') {
                if (s[i]=='(') decode_literal(s,n,&i,out,cap,o);
                else if (s[i]=='-' || (s[i]>='0'&&s[i]<='9') || s[i]=='.') {
                    double v = strtod((const char*)s+i, NULL);
                    if (s[i]=='-'||s[i]=='+') i++;
                    while (i<n && ((s[i]>='0'&&s[i]<='9')||s[i]=='.')) i++;
                    if (v < -SPACE_KERN) emit(out,cap,o,' ');
                } else i++;
            }
            if (i<n && s[i]==']') i++;
            emit(out,cap,o,' ');                 /* end of TJ run is a token boundary */
        }
        else if (s[i]=='(') { decode_literal(s,n,&i,out,cap,o); emit(out,cap,o,' '); }
        else if (s[i]=='<' && i+1<n && s[i+1] != '<') {   /* hex string (not a << dict) */
            i++; int hi=-1;
            while (i<n && s[i]!='>') {
                int d = isxdigit(s[i]) ? (s[i]<='9'?s[i]-'0':(tolower(s[i])-'a'+10)) : -1;
                if (d>=0){ if(hi<0) hi=d; else { emit(out,cap,o,(char)((hi<<4)|d)); hi=-1; } }
                i++;
            }
            if (i<n && s[i]=='>') i++;
            emit(out,cap,o,' ');
        }
        else i++;
    }
}

/* bounded substring search (handles embedded nuls; not strstr). */
static int contains(const unsigned char *s, size_t n, const char *pat) {
    size_t pl = strlen(pat);
    if (pl == 0 || pl > n) return 0;
    for (size_t i = 0; i + pl <= n; i++) if (memcmp(s+i, pat, pl) == 0) return 1;
    return 0;
}

/* A real page content stream carries text operators. Font programs, images, and
   object streams (ObjStm) do not -- this filter keeps us from extracting binary
   garbage from the non-content streams that dominate a modern PDF. */
static int looks_like_content(const unsigned char *s, size_t n) {
    return contains(s, n, "BT") && (contains(s, n, "Tj") || contains(s, n, "TJ"));
}

PdfStatus pdf_extract_text(const unsigned char *pdf, size_t n, char *out, size_t cap, size_t *out_len) {
    size_t o = 0; if (cap) out[0]=0;
    const char *p = (const char*)pdf;

    /* encrypted PDFs: detect and skip (we cannot decode the streams). */
    for (size_t i = 0; i + 8 < n; i++)
        if (memcmp(p+i, "/Encrypt", 8) == 0) { if(out_len)*out_len=0; if(cap) out[0]=0; return PDF_ENCRYPTED; }

    FontDiffs fd; font_diffs_parse(pdf, n, &fd);
    static unsigned char codes[1<<20];

    for (size_t i = 0; i + 6 < n; ) {
        if (memcmp(p+i, "stream", 6) == 0) {
            size_t s0 = i + 6;
            if (s0 < n && p[s0]=='\r') s0++;
            if (s0 < n && p[s0]=='\n') s0++;
            size_t e = s0;
            while (e + 9 < n && memcmp(p+e, "endstream", 9) != 0) e++;

            int is_flate = 0;
            size_t lo = (i > 200) ? i - 200 : 0;
            for (size_t j = lo; j + 12 < i; j++)
                if (memcmp(p+j, "/FlateDecode", 12) == 0) { is_flate = 1; break; }

            const unsigned char *cs = NULL; size_t cslen = 0;
            static unsigned char dec[1<<20];
            if (is_flate) {
                long dl = pdf_flate_decode(pdf + s0, e - s0, dec, sizeof(dec));
                if (dl > 0 && looks_like_content(dec, (size_t)dl)) { cs = dec; cslen = (size_t)dl; }
            } else if (looks_like_content(pdf + s0, e - s0)) {
                cs = pdf + s0; cslen = e - s0;
            }
            if (cs) {
                size_t clen = 0;
                process_content(cs, cslen, (char*)codes, sizeof(codes), &clen);   /* raw codes */
                if (o < cap) o += font_decode_pick_best(codes, clen, &fd, out + o, cap - o);
            }
            i = e + 9;
        } else i++;
    }
    if (cap) out[o] = 0;
    if (out_len) *out_len = o;
    return o > 0 ? PDF_OK : PDF_NO_TEXT;
}
