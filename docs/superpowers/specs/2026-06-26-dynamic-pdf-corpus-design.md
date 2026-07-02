# Dynamic PDF Corpus → Replay-Trained Word-LM — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** Give CNET the ability to read a PDF, autosplit it into a dataset, and learn
it — with a **dynamic, append-only corpus** that expands across ingests (uncapped),
replay-retraining the scalable word-LM on the full accumulated corpus each time.

---

## 1. Goal & acceptance criteria

Build a pure-C, self-contained pipeline `PDF → text → clean sentences → persistent
corpus → cce_wordlm` driven by `make pdflearn [file.pdf ...]`.

Acceptance:

1. `make pdflearn some.pdf` extracts text from a **text-born** PDF, appends the cleaned
   sentences to a persistent corpus, replay-trains `cce_wordlm` on the full corpus, and
   free-runs real-word generated text.
2. **Dynamic/expandable:** re-running with another PDF **appends** to the store; the
   corpus and the vocab **grow** (no fixed caps), and the retrained model reflects *all*
   ingested documents (replay → no forgetting).
3. **Idempotent ingest:** ingesting the same PDF twice does not duplicate the corpus
   (exact-line dedup).
4. **Robust on bad input:** encrypted / CID-Type0-font / scanned (image-only) PDFs are
   **detected and skipped** with a clear status — never a crash or garbage corpus.
5. **No deps, no network:** all tests run on **embedded** PDFs/byte-vectors; no external
   files or HTTP.

## 2. Pipeline

```
file.pdf ─▶ [pdf_extract] ─▶ text ─▶ [corpus_split] ─▶ clean sentences ─┐
                                                                         ▼
                                                       [corpus_store]  append (dedup)
                                                         corpus.txt  ◀── grows across ingests
                                                                         │ load FULL store
                                                                         ▼
                                              build growable vocab ─▶ [cce_wordlm] (sized to
                                                                       current vocab) ─▶ generate
```

Each `make pdflearn newdoc.pdf` appends `newdoc` and **replay-retrains on everything**.
With no PDF arg it retrains/generates from whatever is already accumulated.

## 3. Modules (responsibilities + interfaces)

Each is one file with one responsibility, testable in isolation.

### 3.1 `src/pdf/inflate.c` — `include/pdf/inflate.h`
Embedded **puff** (Mark Adler's public-domain raw-DEFLATE inflater), wrapped for PDF:
```c
/* Raw DEFLATE (no zlib header). dest/destlen are in/out (capacity/used). 0 = ok. */
int  puff(unsigned char *dest, unsigned long *destlen,
          const unsigned char *source, unsigned long *sourcelen);
/* FlateDecode: strips the 2-byte zlib header + trailing adler32, calls puff.
   Returns bytes written, or -1 on malformed input. */
long pdf_flate_decode(const unsigned char *in, size_t in_len,
                      unsigned char *out, size_t out_cap);
```
A pure decompressor; depends only on stdlib. Implemented as a compact, self-contained
raw-DEFLATE inflater (bit reader + fixed/dynamic Huffman + LZ77 back-references — the
standard algorithm), **gated by the embedded test vector**: nothing that depends on
inflate is built until it decompresses the vector exactly. (The `puff` interface name is
kept so the implementation can be swapped for Adler's puff verbatim later without churn.)

### 3.2 `src/pdf/pdf_extract.c` — `include/pdf/pdf_extract.h`
```c
typedef enum { PDF_OK=0, PDF_ENCRYPTED, PDF_UNSUPPORTED_FONTS, PDF_NO_TEXT, PDF_MALFORMED } PdfStatus;
/* Extract text-born content into out (nul-terminated, truncated to cap).
   Returns a status; *out_len gets the byte count. Never reads past pdf[n]. */
PdfStatus pdf_extract_text(const unsigned char *pdf, size_t n,
                           char *out, size_t cap, size_t *out_len);
```
Scope (text-born only):
- Find `stream … endstream` blocks; if the preceding object dict contains `/FlateDecode`,
  inflate via `pdf_flate_decode`; otherwise take the stream raw.
- Inside `BT … ET`, handle text operators `Tj`, `TJ`, `'`, `"`. Decode `(...)` string
  literals (escapes `\( \) \\ \n \r \t \ddd`) and `<hex>` strings. Map bytes through
  **WinAnsi/ASCII** (identity for printable ASCII; a small high-byte table).
- **Space heuristic** (the quality crux): in a `TJ` array, insert a space when a kerning
  number is more negative than a tuned threshold (scaled by the current font size from
  `Tf`); insert a space (or newline) on `Td`/`TD`/`T*`/`Tm` line moves. This recovers
  word boundaries when the PDF emits no space characters.
- **Detect-and-skip:** `/Encrypt` in the trailer → `PDF_ENCRYPTED`; Type0/CID fonts (no
  simple decode) or a stream with zero extractable text → `PDF_UNSUPPORTED_FONTS` /
  `PDF_NO_TEXT`. Return the status; emit whatever clean text was recoverable.

### 3.3 `src/corpus/corpus_split.c` — `include/corpus/corpus_split.h`
```c
typedef struct { char **lines; size_t count, cap; } StrList;   /* realloc-grown */
void strlist_free(StrList *s);
/* text -> cleaned sentences (growable). Pure, no I/O. */
void corpus_split(const char *text, StrList *out);
```
Normalize whitespace; de-hyphenate line-break splits (`exam-\nple` → `example`); drop
junk (bare page numbers, ultra-short fragments); split on `.!?` into sentences.

### 3.4 `src/corpus/corpus_store.c` — `include/corpus/corpus_store.h`
The **dynamic** piece: a persistent, append-only, newline-delimited corpus file.
```c
/* Append each sentence as a line, skipping exact duplicates already in the file.
   Returns the number of NEW lines written. Creates the file if absent. */
size_t corpus_append(const char *path, const StrList *sentences);
/* Load the whole store into a growable list (one line per sentence). */
int    corpus_load(const char *path, StrList *out);
/* Number of lines currently stored (0 if absent). */
size_t corpus_count(const char *path);
```
Source-agnostic — any text producer can feed it later, not just PDFs.

### 3.5 Driver `tests/pdflearn_demo.c` + `make pdflearn [file.pdf ...]`
For each PDF arg: read file → `pdf_extract_text` (skip on non-`PDF_OK`, log status) →
`corpus_split` → `corpus_append`. Then: `corpus_load` the full store → build a
**growable hash vocab** (string→id, no cap) → `cce_wordlm_create(V = current vocab_size, …)`
(replay rebuilds the model each ingest, so V is sized exactly to the current vocab — no
headroom needed) → train by **streaming windows** over every sentence (call
`cce_wordlm_train_step` per
window; no giant materialized matrix) → free-run samples. Print stats: store path, total
sentences, vocab size, new lines added this run, generated samples. An embedded sample PDF
is used when no path is given (so the target runs with zero arguments).

## 4. "Dynamic / not capped" guarantees

- **Persistent + accumulating:** `corpus.txt` survives across runs and grows monotonically.
- **No fixed caps:** corpus list, vocab hash, and training all use realloc-growth /
  streaming — no `MAX_STORIES`, no `WMAX_VOCAB`. Vocab grows to whatever the data needs;
  `cce_wordlm` is O(V·d), so large V is affordable.
- **Replay = no forgetting:** every retrain consumes the full accumulated corpus, so older
  documents are never lost.
- **Future upgrade (out of scope now):** the persistent store + growable structures make
  warm-start incremental training a later option without re-architecting.

## 5. Testing strategy (TDD, dependency-free)

- **inflate:** embed a precomputed DEFLATE/zlib **test vector** (generated offline via
  PowerShell `System.IO.Compression.DeflateStream` or Python `zlib`) of a known string →
  assert exact decompression.
- **pdf_extract:** embed a tiny hand-crafted **uncompressed** PDF with `BT (Hello World) Tj
  ET` → output contains "Hello World"; a `TJ` kerning case → a space is inserted; one
  embedded **FlateDecode** PDF → extraction works; an `/Encrypt` PDF → returns
  `PDF_ENCRYPTED`, no crash.
- **corpus_split:** de-hyphenation, whitespace, and sentence-boundary cases.
- **corpus_store:** append a list twice → second append adds 0 new lines (idempotent);
  count is stable. Append a distinct list → count + vocab grow.
- **end-to-end:** ingest the embedded sample twice (store stable) then a second distinct
  document (store + vocab grow); generation produces real-word text.

Each module ships with assertions in a self-checking demo/test that returns nonzero on
failure (the `make`-target pattern used by `endgate` / `jsonstory`). Run the existing
`make test` after to confirm no regression (changes are additive — new dirs + new target).

## 6. Contract philosophy

PDF extraction and corpus splitting are **heuristic, not decidable total functions**, so
they get **no frozen contracts** — this is plain data infrastructure feeding the LM /
contract layer (where proofs live). No certified primitives here; we do not gold-plate.

## 7. Out of scope (YAGNI)

- CID/Type0 font decoding, ToUnicode CMaps, encrypted PDFs, scanned/OCR (detected + skipped).
- Warm-start / incremental training (replay only for now).
- Network/HTTP ingestion (the store is source-agnostic; PDFs are the only producer built).
- Contracts on the parsing/splitting stages.

## 8. Risks

- **Extraction quality** varies by PDF; the MVP targets text-born PDFs with simple fonts
  and a kerning-based space heuristic; it degrades/skips on others. This is the main risk
  and is built/proven first.
- **Hand-rolled inflate** must be correct — gated by the embedded test vector before
  anything depends on it.
- **Scale of replay** grows with total corpus; acceptable now (fast LM, streamed training),
  revisited only if corpora get very large.

## 9. Phasing

1. `inflate` (+ test vector) — prove decompression.
2. `pdf_extract` — prove on embedded PDFs (uncompressed, FlateDecode, encrypted-skip).
3. `corpus_split` + `corpus_store` — clean sentences + idempotent persistent growth.
4. `pdflearn` driver — replay-train `cce_wordlm`, generate, print stats; end-to-end.

Stage 2 is the risk; stages 3–4 are small and mostly reuse jsonstory's tokenizer/LM glue.

## 10. Project notes

- **No git on this repo.** The `.git` was removed; run no git commands. This spec is
  written to disk, not committed; verify via the filesystem.
- New code lives under `src/pdf/` and `src/corpus/` (mirrors `src/cce/`, `src/router/`);
  training reuses `src/cce/cce_wordlm.c`. **Zero core/router/contract edits.**
