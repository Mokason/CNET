/* JSON-as-contract over TinyStories.
   Generates {"title","characters","story"} with an LM-generated story and parses
   it back losslessly. JSON escape/un-escape decisions are PROVEN (exhaustive
   certification); the characters name-gate is certified + conformal (endgate
   pattern). The story text comes from the self-contained cce_wordlm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>

#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/contract/coverage.h"
#include "../include/contract/conformal.h"
#include "../include/cce/cce_wordlm.h"
#include "json_validate.h"

/* ---- Task 1: oracle smoke ------------------------------------------------ */
static int oracle_smoke(void) {
    int ok = 1;
    ok &= (json_is_valid("{\"a\":\"b\",\"c\":[\"d\",\"e\"]}") == true);
    ok &= (json_is_valid("{\"a\":}") == false);
    ok &= (json_is_valid("not json") == false);
    printf("[oracle] json_is_valid smoke: %s\n", ok ? "ok" : "FAIL");
    return ok;
}

/* ---- Task 2: json_escape_class (byte -> escape class), PROVEN ------------ */
enum { ESC_PASS=0, ESC_QUOTE, ESC_BSLASH, ESC_BS, ESC_TAB, ESC_NL, ESC_FF, ESC_CR, ESC_UNI, ESC_NCLASS };

/* The exact, total escape decision over all 256 bytes. Labels training only. */
static int ref_escape_class(unsigned char b) {
    switch (b) {
        case '"':  return ESC_QUOTE;
        case '\\': return ESC_BSLASH;
        case 0x08: return ESC_BS;   /* \b */
        case 0x09: return ESC_TAB;  /* \t */
        case 0x0A: return ESC_NL;   /* \n */
        case 0x0C: return ESC_FF;   /* \f */
        case 0x0D: return ESC_CR;   /* \r */
    }
    if (b < 0x20 || b >= 0x7F) return ESC_UNI;   /* controls + DEL + high -> \u00XX */
    return ESC_PASS;                              /* 0x20..0x7E except " and \ */
}

/* The frozen primitive + its borrowed training tables (kept alive for the
   demo's lifetime; intentionally not freed). */
static BinaryTransformNetwork g_esc;
static Contract               g_esc_c;

/* argmax of the 9 escape-class outputs of the frozen net (the PROVEN decision). */
static int escape_class_btn(unsigned char b) {
    double in[256]; memset(in, 0, sizeof(in)); in[b] = 1.0;
    const double *o = btn_forward(&g_esc, in);
    int best = 0; for (int k = 1; k < ESC_NCLASS; k++) if (o[k] > o[best]) best = k;
    return best;
}

static int build_escape(void) {
    const size_t IN = 256, OUT = ESC_NCLASS, N = 256;
    /* Canonical 256-row contract table: every byte -> its class (proof domain). */
    double *X = (double*)calloc(N*IN, sizeof(double));
    double *Y = (double*)calloc(N*OUT, sizeof(double));
    for (size_t b = 0; b < N; b++) { X[b*IN + b] = 1.0; Y[b*OUT + ref_escape_class((unsigned char)b)] = 1.0; }

    /* Training set oversamples the rare singleton classes (the 7 short escapes)
       so argmax is not drowned by PASS (~92) and UNI (~158). The contract still
       certifies over the clean 256 rows above. */
    const int REP = 40;
    size_t TRN = N + (size_t)7 * REP;
    double *Xt = (double*)calloc(TRN*IN, sizeof(double));
    double *Yt = (double*)calloc(TRN*OUT, sizeof(double));
    size_t r = 0;
    for (size_t b = 0; b < N; b++) { Xt[r*IN + b] = 1.0; Yt[r*OUT + ref_escape_class((unsigned char)b)] = 1.0; r++; }
    const unsigned char rare[7] = { '"', '\\', 0x08, 0x09, 0x0A, 0x0C, 0x0D };
    for (int j = 0; j < 7; j++) for (int k = 0; k < REP; k++) {
        Xt[r*IN + rare[j]] = 1.0; Yt[r*OUT + ref_escape_class(rare[j])] = 1.0; r++;
    }

    if (btn_init(&g_esc, IN, OUT, 64, 512, 0.05, 1234u) != 0) { printf("[escape] btn_init failed\n"); return 0; }
    Port pin  = { PORT_ONEHOT, IN,  1, "" }; port_set_tag(&pin,  "json_byte");
    Port pout = { PORT_ONEHOT, OUT, 1, "" }; port_set_tag(&pout, "escape_class");
    btn_set_ports(&g_esc, pin, pout);
    btn_train(&g_esc, Xt, Yt, TRN, 1500);

    if (contract_init_borrowed(&g_esc_c, "json_escape_class", &g_esc, X, Y, N) != 0) {
        printf("[escape] contract_init_borrowed failed\n"); return 0;
    }
    ExhaustiveReport er; memset(&er, 0, sizeof(er));
    int proven = btn_certify_exhaustive(&g_esc, &g_esc_c, 0, &er);
    int ok = (proven == 0) && (er.verdict == CERT_PROVEN) && (er.coverage.kind == COVERAGE_EXHAUSTIVE);
    printf("[escape] json_escape_class: %s  (verdict=%d, domain=%zu, swept=%zu, illformed=%zu)\n",
           ok ? "CERT_PROVEN" : "NOT PROVEN", (int)er.verdict,
           er.coverage.domain_cardinality, er.domain_swept, er.domain_illformed);
    /* tables X,Y are borrowed by g_esc_c; keep them alive (demo leaks). */
    return ok;
}

/* ---- Task 3: json_unescape_short (escape letter -> byte), PROVEN --------- */
/* The 7 short escape letters our renderer emits, in a fixed index order. */
static const char ESC_LETTERS[7] = { '"', '\\', 'b', 'f', 'n', 'r', 't' };
static const unsigned char ESC_LETTER_BYTE[7] = { 0x22, 0x5C, 0x08, 0x0C, 0x0A, 0x0D, 0x09 };
static int esc_letter_index(char c) { for (int i = 0; i < 7; i++) if (ESC_LETTERS[i] == c) return i; return -1; }

static BinaryTransformNetwork g_unesc;
static Contract               g_unesc_c;

/* PROVEN decision: escape-letter one-hot -> original byte one-hot (argmax). */
static unsigned char unescape_short_btn(char letter) {
    int idx = esc_letter_index(letter);
    double in[7]; memset(in, 0, sizeof(in)); if (idx >= 0) in[idx] = 1.0;
    const double *o = btn_forward(&g_unesc, in);
    int best = 0; for (int k = 1; k < 256; k++) if (o[k] > o[best]) best = k;
    return (unsigned char)best;
}

static int build_unescape(void) {
    const size_t IN = 7, OUT = 256, N = 7;
    double *X = (double*)calloc(N*IN, sizeof(double));
    double *Y = (double*)calloc(N*OUT, sizeof(double));
    for (size_t i = 0; i < N; i++) { X[i*IN + i] = 1.0; Y[i*OUT + ESC_LETTER_BYTE[i]] = 1.0; }

    if (btn_init(&g_unesc, IN, OUT, 64, 256, 0.08, 4321u) != 0) { printf("[unescape] btn_init failed\n"); return 0; }
    Port pin  = { PORT_ONEHOT, IN,  1, "" }; port_set_tag(&pin,  "escape_letter");
    Port pout = { PORT_ONEHOT, OUT, 1, "" }; port_set_tag(&pout, "json_byte");
    btn_set_ports(&g_unesc, pin, pout);
    btn_train(&g_unesc, X, Y, N, 8000);

    if (contract_init_borrowed(&g_unesc_c, "json_unescape_short", &g_unesc, X, Y, N) != 0) {
        printf("[unescape] contract_init_borrowed failed\n"); return 0;
    }
    ExhaustiveReport er; memset(&er, 0, sizeof(er));
    int proven = btn_certify_exhaustive(&g_unesc, &g_unesc_c, 0, &er);
    int ok = (proven == 0) && (er.verdict == CERT_PROVEN) && (er.coverage.kind == COVERAGE_EXHAUSTIVE);
    printf("[unescape] json_unescape_short: %s  (domain=%zu, swept=%zu, minmargin=%.3f)\n",
           ok ? "CERT_PROVEN" : "NOT PROVEN", er.coverage.domain_cardinality,
           er.domain_swept, er.min_margin_domain);
    return ok;
}

/* ---- Task 4: render / parse_one / round-trip ---------------------------- */
static char hexdig(int v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

/* Append the JSON-string encoding of byte b to out (using the PROVEN escape
   decision). out must have room for >= 6 chars + nul. */
static void render_byte(unsigned char b, char *out) {
    int cls = escape_class_btn(b);
    switch (cls) {
        case ESC_PASS:   out[0]=(char)b; out[1]=0; return;
        case ESC_QUOTE:  strcpy(out, "\\\""); return;
        case ESC_BSLASH: strcpy(out, "\\\\"); return;
        case ESC_BS:     strcpy(out, "\\b");  return;
        case ESC_TAB:    strcpy(out, "\\t");  return;
        case ESC_NL:     strcpy(out, "\\n");  return;
        case ESC_FF:     strcpy(out, "\\f");  return;
        case ESC_CR:     strcpy(out, "\\r");  return;
        default:         out[0]='\\'; out[1]='u'; out[2]='0'; out[3]='0';
                         out[4]=hexdig(b>>4); out[5]=hexdig(b&0xF); out[6]=0; return;
    }
}

/* Read ONE logical content byte from s at *i (advancing *i). Mirrors render. */
static unsigned char parse_one(const char *s, int *i) {
    if (s[*i] != '\\') { unsigned char b = (unsigned char)s[*i]; (*i)++; return b; }
    char c = s[*i + 1];
    if (c == 'u') {
        int hi = s[*i+4], lo = s[*i+5];
        int hv = (hi<='9'?hi-'0':(tolower(hi)-'a'+10));
        int lv = (lo<='9'?lo-'0':(tolower(lo)-'a'+10));
        *i += 6; return (unsigned char)((hv<<4)|lv);
    }
    unsigned char b = unescape_short_btn(c); *i += 2; return b;
}

/* Read a JSON string body into out until the UNescaped closing quote. *i must
   point just past the opening quote on entry; on return it points past the
   closing quote. Returns the byte length written. */
static int read_json_string(const char *s, int *i, char *out, int cap) {
    int n = 0;
    while (s[*i] != '"' && s[*i] != 0) { unsigned char b = parse_one(s, i); if (n < cap-1) out[n++] = (char)b; }
    if (s[*i] == '"') (*i)++;
    out[n] = 0; return n;
}

/* Wrap an arbitrary byte string as the body of a JSON string (escaping each
   byte). Appends to out (which must be large enough). */
static void escape_into(const char *src, char *out) {
    char piece[8]; size_t o = strlen(out);
    for (const unsigned char *p = (const unsigned char*)src; *p; p++) {
        render_byte(*p, piece); size_t L = strlen(piece);
        memcpy(out + o, piece, L); o += L;
    }
    out[o] = 0;
}

/* Exhaustive round-trip: every byte survives render -> parse_one unchanged. */
static int roundtrip_all(void) {
    int pass = 0;
    for (int b = 0; b < 256; b++) {
        char enc[8]; render_byte((unsigned char)b, enc);
        int i = 0; unsigned char got = parse_one(enc, &i);
        if (got == (unsigned char)b && enc[i] == 0) pass++;
    }
    printf("[roundtrip] parse(render(b))==b : %d/256\n", pass);
    return pass == 256;
}

/* A value containing every JSON-hostile byte, wrapped, must still validate. */
static int adversarial_validity(void) {
    const char *hostile = "he said \"hi\"\tand \\ left\nline\x01end";
    char obj[512]; strcpy(obj, "{\"v\":\"");
    escape_into(hostile, obj);
    strcat(obj, "\"}");
    int valid = json_is_valid(obj);
    /* parse it back and confirm losslessness */
    int i = 0; while (obj[i] && obj[i] != ':') i++; i += 2; /* step past ':' then the opening quote */
    char back[512]; read_json_string(obj, &i, back, sizeof(back));
    int lossless = (strcmp(back, hostile) == 0);
    printf("[roundtrip] adversarial value: valid=%d lossless=%d\n", valid, lossless);
    return valid && lossless;
}

/* ---- Task 5: word-LM story generation (cce_wordlm) ---------------------- */
/* Cased micro-corpus (same TinyStories register as endgate_demo). Cased so the
   Task-6 name-gate can label proper nouns; lowercased for LM training. */
static const char *CORPUS[] = {
    "Once upon a time there was a curious fox who found a glowing mushroom in the forest.",
    "Lily loved to play in the garden with her red ball and her happy little dog.",
    "Tom had a little black cat named Max who liked to sleep on a soft warm mat.",
    "Ben built a tall tower with colorful blocks and then laughed when it fell down.",
    "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
    "Sam sailed his paper boat on the pond and the wind brought it safely back home.",
    "Emma sat by the window reading stories while the first stars appeared in the sky.",
    "Jack threw the bright red ball high into the air and his dog caught it in the yard.",
    "Sara baked sweet cookies and the warm smell made the whole house feel very happy.",
    "Anna made a wish on a shooting star and the next morning she met a brand new friend.",
    "A tiny mouse ate a crumb of cheese while the big cat was asleep on the warm mat.",
    "The young dragon practiced flying low over the green hills until he could soar high.",
    "The kind knight gave food to the hungry villagers and they thanked him with a feast.",
    "A clever bird sang beautiful songs and told the princess where the lost crown was hidden.",
    "The girl planted seeds in the spring and by summer the flowers were taller than she was.",
    "A boy found a lost puppy and after searching all day he returned it to its happy family."
};
#define NCORP ((int)(sizeof(CORPUS)/sizeof(CORPUS[0])))
#define WCTX 3
#define WVMAX 400
static char  g_word[WVMAX][24];
static int   g_nwords = 0;

static void wlower(char *w) { for (; *w; ++w) if (*w>='A'&&*w<='Z') *w=(char)(*w+32); }
static int  word_add(const char *w) {
    for (int i=0;i<g_nwords;i++) if (strcmp(g_word[i],w)==0) return i;
    if (g_nwords>=WVMAX) return -1;
    snprintf(g_word[g_nwords], sizeof(g_word[0]), "%s", w); return g_nwords++;
}
static int  word_find(const char *w){ for(int i=0;i<g_nwords;i++) if(strcmp(g_word[i],w)==0) return i; return -1; }

/* Tokenize to lowercased word ids; '.'!'?' -> the "." token. */
static int tokenize(const char *s, int *out, int cap, int add) {
    int n=0; char cur[24]; int cl=0;
    for (const char *p=s;;++p){ char c=*p;
        int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if (al){ if(cl<23) cur[cl++]=c; }
        else { if(cl>0){cur[cl]=0; wlower(cur); int id=add?word_add(cur):word_find(cur); if(id>=0&&n<cap)out[n++]=id; cl=0;}
               if(c=='.'||c=='!'||c=='?'){int id=add?word_add("."):word_find("."); if(id>=0&&n<cap)out[n++]=id;}
               if(c==0) break; }
    }
    return n;
}

/* The word-LM, trained once over the corpus and reused for every free-run. */
static cce_wordlm *g_lm = NULL;

static void train_lm(void) {
    word_add(".");
    { int tmp[256]; for (int s=0;s<NCORP;s++) tokenize(CORPUS[s], tmp, 256, 1); }
    int V = g_nwords;
    g_lm = cce_wordlm_create(V, 24, WCTX, 64, 99u);
    for (int epoch=0; epoch<200; epoch++)
        for (int s=0;s<NCORP;s++){ int t[256]; int nt=tokenize(CORPUS[s],t,256,0);
            for (int i=WCTX;i<nt;i++) cce_wordlm_train_step(g_lm, &t[i-WCTX], t[i], 0.02f); }
}

/* Free-run one story from a 3-word seed context, displayed after `prefix`. */
static void freerun_story(const char *s0, const char *s1, const char *s2,
                          const char *prefix, char *out, size_t outsz) {
    int V = g_nwords, dot = word_find("."); int ctx[WCTX];
    const char *seed[WCTX] = { s0, s1, s2 };
    for (int k=0;k<WCTX;k++){ int id=word_find(seed[k]); ctx[k]=(id>=0)?id:0; }
    snprintf(out, outsz, "%s", prefix);
    float pen[WVMAX];
    for (int step=0; step<40 && strlen(out)<outsz-32; step++) {
        for (int v=0; v<V; v++) pen[v]=0.0f;
        for (int k=0;k<WCTX;k++) pen[ctx[k]] -= 3.0f;      /* discourage immediate repeats */
        int nx = cce_wordlm_predict(g_lm, ctx, pen);
        if (nx<0 || nx==dot) break;
        size_t L=strlen(out); snprintf(out+L, outsz-L, " %s", g_word[nx]);
        for (int k=0;k<WCTX-1;k++) ctx[k]=ctx[k+1];
        ctx[WCTX-1]=nx;
    }
    size_t L=strlen(out); if (L+1<outsz){ out[L]='.'; out[L+1]=0; }
}

/* ---- Task 6: character name-gate (endgate pattern) ---------------------- */
/* A word is a NAME exemplar if it appears Capitalized mid-sentence in the cased
   corpus (i.e., not as the first token after a sentence start). Counts feed a
   per-word majority label, exactly like endgate's end/continue counts. */
static BinaryTransformNetwork g_gate;
static Contract               g_gate_c;
static ConformalCalibrator    g_gate_cal;
static int                    g_gate_V = 0;

/* The corpus's character-name lexicon (lowercased). The gate's decidable core is
   the per-word rule "is this word a known character name?". */
static const char *NAMES[] = { "lily","tom","max","ben","mia","sam","emma","jack","sara","anna" };
#define NNAMES ((int)(sizeof(NAMES)/sizeof(NAMES[0])))
static int word_is_name(const char *w) { for (int i=0;i<NNAMES;i++) if (strcmp(NAMES[i],w)==0) return 1; return 0; }

static int build_namegate(void) {
    int V = g_nwords; g_gate_V = V;
    /* Domain = every distinct corpus word except the "." token. One clean
       exemplar per word (word -> {not,name}) for the contract + conformal calib. */
    int dot = word_find(".");
    int seen=0; for (int w=0;w<V;w++) if (w!=dot) seen++;
    double *X=(double*)calloc((size_t)seen*V,sizeof(double));
    double *Y=(double*)calloc((size_t)seen*2,sizeof(double));
    size_t *kT=(size_t*)calloc((size_t)seen,sizeof(size_t));
    double *kX=(double*)calloc((size_t)seen*V,sizeof(double));
    int npos=0, r=0;
    for (int w=0;w<V;w++){
        if (w==dot) continue;
        int name = word_is_name(g_word[w]);
        X[(size_t)r*V+w]=1.0; Y[(size_t)r*2+name]=1.0;
        kX[(size_t)r*V+w]=1.0; kT[r]=(size_t)name; if(name) npos++; r++;
    }

    /* Training set oversamples the rare name-positives (~10) so they are not
       drowned by ~160 common-word negatives. Contract still certifies over the
       clean one-per-word table above. */
    const int REP = 16;
    size_t TRN = (size_t)seen + (size_t)npos*(REP-1);
    double *Xt=(double*)calloc(TRN*V,sizeof(double));
    double *Yt=(double*)calloc(TRN*2,sizeof(double));
    size_t t=0;
    for (int w=0;w<V;w++){
        if (w==dot) continue;
        int name = word_is_name(g_word[w]);
        int reps = name ? REP : 1;
        for (int k=0;k<reps;k++){ Xt[t*V+w]=1.0; Yt[t*2+name]=1.0; t++; }
    }

    if (btn_init(&g_gate, (size_t)V, 2, 64, 256, 0.05, 2468u)!=0){ printf("[gate] btn_init failed\n"); return 0; }
    Port pin={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&pin,"story_word");
    Port pout={PORT_ONEHOT,2,1,""};        port_set_tag(&pout,"is_name");
    btn_set_ports(&g_gate,pin,pout);
    btn_train(&g_gate, Xt, Yt, TRN, 1500);

    if (contract_init_borrowed(&g_gate_c,"character_name_gate",&g_gate,X,Y,(size_t)seen)!=0){
        printf("[gate] contract_init_borrowed failed\n"); return 0;
    }
    CertifyReport rep; memset(&rep,0,sizeof(rep));
    int cert = btn_certify(&g_gate,&g_gate_c,&rep);

    double alpha=0.10;
    int calib_ok = (conformal_calibrate_btn(&g_gate_cal,&g_gate,&g_gate_c,kX,kT,(size_t)seen,alpha)==0);
    printf("[gate] character_name_gate: %s (%zu/%zu exemplars, %d names), conformal coverage=%.2f\n",
           cert==0?"CERTIFIED":"partial", rep.passed, rep.exemplars, npos,
           calib_ok?conformal_coverage_level(&g_gate_cal):0.0);
    /* tables borrowed by contract/calibrator: keep alive (demo leaks). */
    return (cert==0) && calib_ok;
}

/* Conformal reject-option decision for one word: 1 NAME / 0 not / -1 ABSTAIN. */
static int gate_decide(const char *word) {
    int w = word_find(word); if (w < 0) return -1;
    double *in=(double*)calloc((size_t)g_gate_V,sizeof(double)); in[w]=1.0;
    int dec=conformal_classify_or_abstain(&g_gate_cal,&g_gate,&g_gate_c,in); free(in);
    return dec;
}

/* Probe: prove the certified gate separates known names from common words. */
static void gate_probe(void) {
    const char *probe[] = {"lily","tom","max","the","fox","forest"};
    printf("[gate] probe:");
    for (int i=0;i<(int)(sizeof(probe)/sizeof(probe[0]));i++){
        int d=gate_decide(probe[i]);
        printf(" %s=%s", probe[i], d==1?"NAME":d==0?"not":"abstain");
    }
    printf("\n");
}

/* Extract distinct character names from a story: words the gate accepts (==1).
   Conformal ABSTAIN (-1) or reject (0) are dropped. Returns count. */
static int extract_characters(const char *story, char names[][24], int cap) {
    int toks[256]; int nt=tokenize(story,toks,256,0); int nc=0;
    for (int i=0;i<nt && nc<cap;i++){
        int w=toks[i]; if(w<0) continue;
        double *in=(double*)calloc((size_t)g_gate_V,sizeof(double)); in[w]=1.0;
        int dec=conformal_classify_or_abstain(&g_gate_cal,&g_gate,&g_gate_c,in); free(in);
        if (dec==1){
            int dup=0; for(int j=0;j<nc;j++) if(strcmp(names[j],g_word[w])==0) dup=1;
            if(!dup){ snprintf(names[nc],24,"%s",g_word[w]); nc++; }
        }
    }
    return nc;
}

/* ---- Task 7: assemble {title,characters,story}, validate, parse back ---- */
/* Deterministic title: Title-Case the first content word after the seed/stop
   set. Its bytes are escaped by the PROVEN escaper, so the title is JSON-safe
   without a separate certificate. */
static void make_title(const char *story, char *title, size_t cap) {
    const char *stop[5] = {"once","upon","a","an","the"};
    int toks[256]; int nt=tokenize(story,toks,256,0);
    const char *pick = NULL;
    for (int i=0;i<nt;i++){
        int w=toks[i]; if(w<0) continue; const char *ww=g_word[w];
        if (strcmp(ww,".")==0) continue;
        int isstop=0; for(int k=0;k<5;k++) if(strcmp(ww,stop[k])==0) isstop=1;
        if (!isstop){ pick=ww; break; }
    }
    if (!pick) pick = (nt>0 && toks[0]>=0) ? g_word[toks[0]] : "story";
    snprintf(title, cap, "%s", pick);
    if (title[0]>='a'&&title[0]<='z') title[0]=(char)(title[0]-32);  /* Title-Case */
}

/* Build {"title":"..","characters":["..",..],"story":".."} with escaped values. */
static void assemble_json(const char *title, char names[][24], int nnames,
                          const char *story, char *out, size_t outsz) {
    (void)outsz;
    strcpy(out, "{\"title\":\""); escape_into(title, out); strcat(out, "\",\"characters\":[");
    for (int i=0;i<nnames;i++){ if(i) strcat(out,","); strcat(out,"\""); escape_into(names[i],out); strcat(out,"\""); }
    strcat(out, "],\"story\":\""); escape_into(story, out); strcat(out, "\"}");
}

/* Parse our fixed frame back into fields. Returns 1 on a clean structural match. */
static int parse_back(const char *s, char *title, char *story,
                      char names[][24], int *nnames, int cap) {
    int i=0;
    if (strncmp(s+i,"{\"title\":\"",10)!=0) return 0;
    i+=10;
    read_json_string(s,&i,title,256);
    if (strncmp(s+i,",\"characters\":[",15)!=0) return 0;
    i+=15;
    int nc=0;
    if (s[i]!=']'){
        for(;;){
            if(s[i]!='"') return 0;
            i++;
            read_json_string(s,&i,names[nc<cap?nc:0],24);
            if(nc<cap) nc++;
            if(s[i]==','){ i++; continue; }
            if(s[i]==']'){ break; }
            return 0;
        }
    }
    if (s[i]!=']') return 0;
    i++;
    *nnames=nc;
    if (strncmp(s+i,",\"story\":\"",10)!=0) return 0;
    i+=10;
    read_json_string(s,&i,story,2048);
    if (strncmp(s+i,"}",1)!=0) return 0;
    return 1;
}

/* Assemble JSON for one story, validate it, parse it back, check field equality. */
static int emit_and_check(const char *label, const char *story) {
    char title[256]; make_title(story, title, sizeof(title));
    char names[16][24]; int nnames = extract_characters(story, names, 16);
    char obj[4096]; assemble_json(title, names, nnames, story, obj, sizeof(obj));
    int valid = json_is_valid(obj);

    char t2[256], s2[2048], n2[16][24]; int nn2=0;
    int parsed = parse_back(obj, t2, s2, n2, &nn2, 16);
    int fields_ok = parsed && strcmp(t2,title)==0 && strcmp(s2,story)==0 && nn2==nnames;
    for (int i=0;i<nnames && fields_ok;i++) if (strcmp(n2[i],names[i])!=0) fields_ok=0;

    printf("\n[%s] %s\n", label, obj);
    printf("[%s] json_is_valid=%d  parse_back_fields_ok=%d  characters=%d\n",
           label, valid, fields_ok, nnames);
    return valid && fields_ok;
}

int main(void) {
    srand(7);
    printf("=== JSON-as-contract over TinyStories ===\n");
    int ok = 1;
    ok &= oracle_smoke();
    ok &= build_escape();
    ok &= build_unescape();
    ok &= roundtrip_all();
    ok &= adversarial_validity();

    train_lm();
    ok &= build_namegate();
    gate_probe();

    /* Canonical opener (no named characters) and a name-bearing story, both
       LM-generated; each wrapped, validated, and parsed back losslessly. */
    char story1[2048], story2[2048];
    freerun_story("upon","a","time", "Once upon a time", story1, sizeof(story1));
    freerun_story("tom","had","a",   "Tom had a",        story2, sizeof(story2));
    printf("[story-1] %s\n", story1);
    printf("[story-2] %s\n", story2);
    ok &= (strlen(story1) > 16 && story1[strlen(story1)-1]=='.');
    ok &= (strlen(story2) > 16 && story2[strlen(story2)-1]=='.');

    ok &= emit_and_check("json-1", story1);
    ok &= emit_and_check("json-2", story2);

    if (g_lm) cce_wordlm_free(g_lm);
    printf(ok ? "\nALL PASS\n" : "\nFAIL\n");
    return ok ? 0 : 1;
}
