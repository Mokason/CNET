#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/pdf/inflate.h"
#include "../include/pdf/pdf_extract.h"
#include "../include/corpus/corpus_split.h"
#include "../include/corpus/corpus_store.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { g_checks++; if(!(cond)){ g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);} } while(0)

static void test_inflate(void) {
    /* raw DEFLATE of ("the quick brown fox jumps over the lazy dog. " x4), 180 bytes. */
    static const unsigned char VEC[] = {
        0x2b,0xc9,0x48,0x55,0x28,0x2c,0xcd,0x4c,0xce,0x56,0x48,0x2a,0xca,0x2f,0xcf,0x53,
        0x48,0xcb,0xaf,0x50,0xc8,0x2a,0xcd,0x2d,0x28,0x56,0xc8,0x2f,0x4b,0x2d,0x52,0x28,
        0x01,0x4a,0xe7,0x24,0x56,0x55,0x2a,0xa4,0xe4,0xa7,0xeb,0x81,0x79,0x83,0x40,0x31,
        0x00 };
    const char *want =
        "the quick brown fox jumps over the lazy dog. "
        "the quick brown fox jumps over the lazy dog. "
        "the quick brown fox jumps over the lazy dog. "
        "the quick brown fox jumps over the lazy dog. ";
    unsigned char out[1024];
    unsigned long olen = sizeof(out), slen = sizeof(VEC);
    int rc = puff(out, &olen, VEC, &slen);
    CHECK(rc == 0, "puff returns 0");
    CHECK(olen == strlen(want), "puff output length matches");
    CHECK(olen <= sizeof(out) && memcmp(out, want, olen) == 0, "puff output bytes match");
}

/* Minimal uncompressed PDF: one content stream "BT (Hello World) Tj ET". */
static const char PDF_PLAIN[] =
"%PDF-1.4\n"
"4 0 obj<</Length 24>>stream\n"
"BT (Hello World) Tj ET\n"
"endstream endobj\n";

static void test_extract_plain(void) {
    char out[256]; size_t olen = 0;
    PdfStatus st = pdf_extract_text((const unsigned char*)PDF_PLAIN, sizeof(PDF_PLAIN)-1, out, sizeof(out), &olen);
    CHECK(st == PDF_OK, "plain PDF status OK");
    CHECK(strstr(out, "Hello World") != NULL, "plain PDF yields 'Hello World'");
}

static const char PDF_TJ[] =
"%PDF-1.4\n4 0 obj<</Length 40>>stream\n"
"BT [(Hello)-400(World)] TJ ET\n"
"endstream endobj\n";

static void test_extract_tj(void) {
    char out[256]; size_t olen=0;
    PdfStatus st = pdf_extract_text((const unsigned char*)PDF_TJ, sizeof(PDF_TJ)-1, out, sizeof(out), &olen);
    CHECK(st == PDF_OK, "TJ PDF status OK");
    CHECK(strstr(out, "Hello World") != NULL || strstr(out, "Hello  World") != NULL,
          "TJ inserts space between Hello and World");
}

/* content stream bytes = 0x78,0x9c + raw DEFLATE of "BT (Hello Flate) Tj ET". */
static const unsigned char FLATE_STREAM[] = {
    0x78,0x9c,
    0x73,0x0a,0x51,0xd0,0xf0,0x48,0xcd,0xc9,0xc9,0x57,0x70,0xcb,0x49,0x2c,0x49,0xd5,
    0x54,0x08,0xc9,0x52,0x70,0x0d,0x01,0x00 };

static void test_extract_flate(void) {
    unsigned char pdf[512]; size_t k=0;
    const char *head = "%PDF-1.4\n4 0 obj<</Filter/FlateDecode/Length 99>>stream\n";
    memcpy(pdf+k, head, strlen(head)); k+=strlen(head);
    memcpy(pdf+k, FLATE_STREAM, sizeof(FLATE_STREAM)); k+=sizeof(FLATE_STREAM);
    const char *tail = "\nendstream endobj\n";
    memcpy(pdf+k, tail, strlen(tail)); k+=strlen(tail);
    char out[256]; size_t olen=0;
    PdfStatus st = pdf_extract_text(pdf, k, out, sizeof(out), &olen);
    CHECK(st == PDF_OK, "flate PDF status OK");
    CHECK(strstr(out, "Hello Flate") != NULL, "flate PDF yields 'Hello Flate'");
}

static void test_extract_encrypted(void) {
    const char *enc = "%PDF-1.4\ntrailer<</Root 1 0 R/Encrypt 5 0 R>>\n";
    char out[64]; size_t olen=0;
    PdfStatus st = pdf_extract_text((const unsigned char*)enc, strlen(enc), out, sizeof(out), &olen);
    CHECK(st == PDF_ENCRYPTED, "encrypted PDF is detected and skipped");
}

static void test_split(void) {
    StrList s; strlist_init(&s);
    corpus_split("The cat sat. It was warm! Was it?  A bit.", &s);
    CHECK(s.count == 4, "splits into 4 sentences");
    CHECK(s.count>0 && strcmp(s.lines[0], "The cat sat.")==0, "first sentence clean");
    strlist_free(&s);

    StrList h; strlist_init(&h);
    corpus_split("a long exam-\nple word here that is fine.", &h);
    CHECK(h.count==1 && strstr(h.lines[0],"example")!=NULL, "de-hyphenates line breaks");
    strlist_free(&h);

    StrList j; strlist_init(&j);
    corpus_split("ok enough.\n  42\n  also good enough here.", &j);
    int has42=0; for(size_t k=0;k<j.count;k++) if(strcmp(j.lines[k],"42")==0) has42=1;
    CHECK(!has42, "bare page-number line dropped");
    strlist_free(&j);
}

static void test_store(void) {
    const char *path = "test_corpus_tmp.txt";
    remove(path);
    StrList a; strlist_init(&a); strlist_push(&a,"one sentence here."); strlist_push(&a,"two sentence here.");
    size_t added1 = corpus_append(path, &a);
    CHECK(added1==2, "first append writes 2 lines");
    size_t added2 = corpus_append(path, &a);          /* same content again */
    CHECK(added2==0, "re-appending identical content adds 0 (idempotent)");
    CHECK(corpus_count(path)==2, "count stays 2 after dup append");
    StrList b; strlist_init(&b); strlist_push(&b,"three sentence here.");
    corpus_append(path,&b);
    CHECK(corpus_count(path)==3, "distinct content grows the store");
    StrList loaded; strlist_init(&loaded); corpus_load(path,&loaded);
    CHECK(loaded.count==3, "load returns all 3 lines");
    strlist_free(&a); strlist_free(&b); strlist_free(&loaded);
    remove(path);
}

int run_test_pdf(void) {
    printf("=== test_pdf ===\n");
    test_inflate();
    test_extract_plain();
    test_extract_tj();
    test_extract_flate();
    test_extract_encrypted();
    test_split();
    test_store();
    printf("%d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}

#ifndef TEST_ALL
int main(void) { return run_test_pdf() == 0 ? 0 : 1; }
#endif
