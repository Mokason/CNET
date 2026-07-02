# Extraction Quality — `/Differences` Font Recovery + Quality Filter (Lever 1) — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** Raise PDF-extraction quality so recall and graduation see clean prose:
recover the garbled streams by decoding the font `/Differences` encoding (the book
has no `/ToUnicode`), and drop residual junk (TOC/page-number fragments, un-recovered
garbage) with an English-likeness quality filter. With benchmarks.

---

## 1. Goal & acceptance criteria

1. **Font-encoding recovery (no full PDF parser).** Parse the PDF's `/Differences`
   arrays, map glyph names → text (AGL-lite, incl. ligatures), and decode each content
   stream with the candidate map that maximizes English-likeness (best-match — sidesteps
   the `ObjStm` object-graph needed for perfect font association).
2. **Quality filter.** A `corpus_quality_keep(sentence)` predicate (real-word fraction +
   token count + alpha ratio) drops residual junk.
3. **Measured improvement** on `aivalueplaybook.pdf`: corpus good-fraction **up** vs the
   current extractor; sampled **ligature fixes** (`specic`→`specific`); the **storytelling
   Q&A no longer returns symbol-soup**; junk-tile count down.
4. **No regression**: existing `make pdftest`/`tiermem_test`/`graduate` stay green (synthetic
   test PDFs have no `/Differences` → best-match picks raw WinAnsi = current behavior).
5. **Benchmarks**: per-phase timing (parse diffs / extract+decode / quality filter) + the
   before/after quality numbers.

## 2. What the book exposes (grounded)

- `/ToUnicode` = **0** (clean path absent).
- `/Differences` = **6**, glyph names incl. ligatures (`f_i`,`f_l`,`f_f`,`f_f_i`,`T_h`);
  fonts are subset **WinAnsi + Differences** (low codes remapped to ligatures; the
  symbol-soup streams remap many codes).
- Page `/Resources` (font-name → object) are in compressed `ObjStm` → **perfect** per-font
  association needs a full object-graph parse — **out of scope** (a separate larger project).

## 3. The mechanism

A PDF content stream's string bytes are *codes*; a code maps to a glyph via the font's
encoding. Codes not in `/Differences` use the base (WinAnsi → the ASCII byte); codes in
`/Differences` map to a named glyph.

- **Parse** every `/Differences[ code /glyph /glyph … ]` (numbers reset the running code;
  each `/name` assigns and increments) → one `code→glyphname` table per font found.
- **Glyph-name → text (AGL-lite)**: contains `_` → split and concatenate each part
  (`f_i`→`fi`, `f_f_i`→`ffi`, `T_h`→`Th`); single ASCII-letter name → that letter; a small
  standard table (`space`,`period`,`comma`,`hyphen`,`one`…`nine`,…); `uniXXXX` →
  codepoint (ASCII only); unknown → drop. Precompute per code → `text[256]`.
- **Per-stream best-match decode**: candidate decoders = `{raw WinAnsi}` ∪ `{each
  Differences map}`. For each, decode the stream's raw codes (code in map → its text; else
  printable-ASCII byte; else drop) and score by **English-likeness** (real-word fraction);
  keep the highest-scoring text. Recovers ligatures in readable streams and symbol-soup in
  remapped streams (when a matching map exists); otherwise raw WinAnsi wins (= today).

## 4. Components

- **`include/pdf/font_decode.h`, `src/pdf/font_decode.c`**
  - `typedef struct { unsigned char set[256]; char text[256][8]; } DiffMap;`
  - `typedef struct { DiffMap maps[16]; int n; } FontDiffs;`
  - `void font_diffs_parse(const unsigned char *pdf, size_t n, FontDiffs *fd);`
  - `void glyph_to_text(const char *name, char *out, size_t cap);`
  - `size_t decode_codes(const unsigned char *raw, size_t n, const DiffMap *map /*NULL=WinAnsi*/, char *out, size_t cap);`
  - `double english_likeness(const char *s);`  /* real-word fraction */
- **Modify `src/pdf/pdf_extract.c`**: `process_content` emits **raw code bytes** (the old
  ASCII filter moves into `decode_codes`); `pdf_extract_text` parses `FontDiffs` once, and
  per content stream extracts raw codes to a temp buffer, runs **best-match** over
  `{NULL} ∪ maps`, and appends the winning text. `map==NULL` reproduces today's behavior.
- **`corpus_split.c`**: add `int corpus_quality_keep(const char *sentence);` (real-word
  fraction ≥ 0.6, token count ≥ 3, alpha+space ratio ≥ 0.85). Callers opt in; existing
  `corpus_split` is unchanged (no surprise count changes for current tests).
- **`tests/test_font_decode.c`** + `make fontdecode`: unit tests (parse, glyph_to_text,
  decode_codes, quality predicate) + the real-book before/after + benchmarks + a Q&A probe.

## 5. Data flow

```
PDF ─▶ font_diffs_parse ─▶ FontDiffs (code->text maps)
content stream ─▶ extract raw codes ─▶ best-match decode over {WinAnsi} ∪ maps ─▶ append best text
corpus ─▶ corpus_split ─▶ corpus_quality_keep gate ─▶ clean tiles  (recall/graduation)
```

## 6. Testing / proofs

- **Unit**: `/Differences` parse on an embedded snippet (`/Differences[31/f_i ...]` →
  code 31 text "fi"); `glyph_to_text("f_l")=="fl"`, `glyph_to_text("space")==" "`;
  `decode_codes` with/without a map; `corpus_quality_keep` accepts a real sentence and
  rejects `"3 1 4 5"` and `:<ZhymmQS>`.
- **Real book before/after** (`aivalueplaybook.pdf`): extract with the old path vs the new
  path; assert english-likeness good-fraction **rises** and at least one sampled ligature
  word is recovered; count junk tiles dropped by the quality filter.
- **Q&A probe**: the storytelling query no longer returns symbol-soup as its top hit.
- Run `make pdftest`/`tiermem_test`/`graduate` after — all green (additive + backwards-
  compatible on synthetic PDFs).

## 7. Benchmarks (required output)

```
PHASE                TIME(ms)   METRIC
parse /Differences      ...     maps=N
extract+decode (old)    ...     chars, good-fraction=A%
extract+decode (new)    ...     chars, good-fraction=B%  (B>A)
quality filter          ...     sentences kept K of M, junk dropped J
```
Plus sampled before→after lines (e.g. `specic -> specific`).

## 8. Honest scope / limits

- Best-match decode is a **heuristic** (no font tracking); a stream whose font map isn't
  among the parsed `/Differences`, or whose best decode still scores low, falls to the
  quality filter (dropped). Recovers **much** of the ligature loss + **some** symbol-soup,
  not 100%.
- Perfect per-font association (the `ObjStm` object-graph parse) is a separate, much larger
  project — flagged, not built.
- The quality filter is opt-in per caller (keeps existing tests' counts stable); wiring it
  into tile_memory/graduate ingestion is a one-line follow-up shown in the test.

## 9. Project notes

- **No git.** Spec written, not committed; verify via filesystem.
- New: `src/pdf/font_decode.c`, a `corpus_quality_keep` addition, `tests/test_font_decode.c`,
  the `fontdecode` target; a moderate `pdf_extract.c` refactor (raw-codes + best-match).
  **Zero core/router/contract edits.**
