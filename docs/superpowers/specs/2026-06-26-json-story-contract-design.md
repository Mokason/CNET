# JSON-as-Contract over TinyStories — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** A trained, contract-backed capability to *understand* (parse) and *generate*
JSON whose payload is an LM-generated TinyStory, with machine-checkable guarantees
rather than a measured success rate.

---

## 1. Goal & acceptance criteria

Build a capability that:

- **Generates** a well-formed JSON object `{"title":…, "characters":[…], "story":…}`
  whose `story` is produced by the existing TinyStories word-LM.
- **Understands** (parses) such JSON back into its three fields, losslessly.
- Backs both directions with a **machine-checkable contract** in CNET's sense
  (frozen BTN + certified exemplars), not a sampled validity percentage.

Acceptance criteria (from the request):

1. *"trained json having a contract with tiny stories"* — the JSON escape/un-escape
   decisions are **trained** BTNs that pass **exhaustive** certification
   (`CERT_PROVEN`); the round-trip losslessness is proven 256/256; the `characters`
   name-gate's contract is built from **TinyStories** words.
2. *"generating json with stories"* — the demo emits
   `{"title":…, "characters":[…], "story":"Once upon a time …"}` containing a real
   LM-generated TinyStory, and the repo's independent `json_is_valid()` oracle
   confirms each emitted object.

## 2. The foil already in the repo

[`tests/cce_json_bench.c`](../../../tests/cce_json_bench.c) already trains the CCE
char-LM to emit raw JSON character-by-character and **measures** validity
(`generation validity: N/M`) using a stack-based `json_is_valid()`. That is the
"LM emits JSON end-to-end" path — no correctness guarantee. This design takes the
opposite stance: the structure is *proven*, only the story text is the LM's job.
We **reuse** `json_is_valid()` from that file as an independent well-formedness oracle.

## 3. Core insight — JSON well-formedness reduces to a provable atom

For a fixed schema, a JSON object is well-formed **iff** every field *value* is
correctly **string-escaped**; the structural skeleton
(`{`, `"title":`, `,`, `"characters":[`, `]`, `"story":`, `}`) is constant and
trivially well-formed once the values can't break out of their quotes.

String escaping is a **total function over 256 byte values** → an enumerable domain →
`btn_certify_exhaustive` returns a **PROOF** (`CertVerdict CERT_PROVEN`,
`CoverageKind COVERAGE_EXHAUSTIVE`), not evidence. Proving the escape decision over
the *entire* byte domain + fixing the skeleton ⇒ **provably well-formed JSON**.

This is the decimal/hex/logic-gate move (decidable structure proven exactly) plus
the endgate move (the LM owns the uncertain text).

## 4. Components

Each structural piece is a trained BTN frozen behind a contract. The story text is
the existing LM. Renderer/parser tables are fixed constant data.

### 4.1 `json_escape_class` — the substantive escaping proof
- Ports: `byte : ONEHOT[256]  →  class : ONEHOT[9]`
- Classes: `{ PASS, ESC_QUOTE(\"), ESC_BSLASH(\\), ESC_BS(\b), ESC_TAB(\t),
  ESC_NL(\n), ESC_FF(\f), ESC_CR(\r), ESC_UNI(\u00XX) }`.
  - PASS  = 0x20–0x7F except `"`(0x22) and `\`(0x5C) → emit the byte literally.
  - the seven short escapes as named.
  - ESC_UNI = all remaining control bytes (0x00–0x1F) **and** high bytes
    (0x80–0xFF) → `\u00XX` (lossless, strictly-valid JSON).
- Train a BTN on all 256 (byte → correct class) pairs, then
  `btn_certify_exhaustive(...) == 0` ⇒ **CERT_PROVEN**. This is where a hand-written
  escaper has bugs (a forgotten control char, mis-handled 0x7F); the exhaustive
  certificate proves the decision is total and correct over every byte.

### 4.2 `json_unescape_tok` — the parser's per-token proof
- Ports: `tok : ONEHOT[U]  →  byte : ONEHOT[256]`, where `U` is the escaped-token
  alphabet (each literal pass-through byte + the seven short escapes + a `\u00XX`
  token family, collapsed to one token id per source byte so the map is a bijection).
- Decides, for each token a JSON-string reader pulls off the stream, the original
  byte. Enumerable → `btn_certify_exhaustive` ⇒ **CERT_PROVEN**. This is the genuine
  inverse *decision* and is not vacuous: it is trained separately from the escaper.

### 4.3 `json_roundtrip` — exhaustive losslessness (256/256)
The headline guarantee: for **every** byte `b` in 0..255,
`parse_one( render( b, escape_class(b) ) ) == b`, where `render` is the fixed
class→string table (§4.1) and `parse_one` is the fixed token reader driving
`json_unescape_tok`. Run as a direct **exhaustive strict replay over all 256 bytes**
in the **property-contract *style*** (`split∘combine = identity`). Note: this is a
direct replay, **not** the `property_check` API — a fixed-width port chain cannot
model the variable-width string rendering, so `property_check` has no clean home here
and is intentionally not used. The two per-port decisions (§4.1, §4.2) are proven by
`btn_certify_exhaustive`; this §4.3 replay proves the *composite* round-trip is
lossless. Result must be **256/256**.

### 4.4 Story slot — reuse the TinyStories word-LM
The `story` value comes from the existing CCE word-LM free-run
(`freerun_word` in [`test_tinystories.c`](../../../test_tinystories.c) / `cce_wordlm`).
Reused, not rebuilt. This is the only uncertain component; it is *inside* the proven
envelope, so it can never make the JSON ill-formed.

### 4.5 `title` — derived deterministically
`title` = Title-Case of the story's first content word, skipping the fixed seed/stop
words `{once, upon, a, an, the}`; if none remain, fall back to the literal first word.
This is a plain deterministic function — **not** a separately certified primitive.
The title's bytes are escaped by the PROVEN `json_escape_class`, so the title is
JSON-safe without its own certificate; a dedicated Title-Case contract would be
gold-plating (YAGNI).

### 4.6 `character` name-gate — reuse the endgate pattern verbatim
- Ports: `word : ONEHOT[V]  →  is_name : ONEHOT[2]`.
- Decidable core (words appearing capitalized / as proper nouns in the corpus)
  certified exactly with `btn_certify`; the residual gets **conformal abstention**
  (`conformal_calibrate_btn` / `conformal_classify_or_abstain`, α≈0.10) — accept iff
  the prediction set is a singleton, else ABSTAIN (drop / defer).
- The `characters` array = the story's words the gate accepts. This is direct reuse
  of [`tests/endgate_demo.c`](../../../tests/endgate_demo.c)'s machinery, symmetric
  with the sentence-terminator primitive.

## 5. Data flow

**Generate**
```
story  = wordLM.freerun()                                  // LM (uncertain)
title  = titlecase(first_content_words(story))             // certified per-byte map
chars  = [ w for w in words(story) if name_gate(w)==NAME ] // endgate-style gate
json   = "{\"title\":\""      + escape(title)            + "\","
       +  "\"characters\":["  + join(escape(c) for c in chars) + "],"
       +  "\"story\":\""      + escape(story)            + "\"}"
```
Provably well-formed: `escape` PROVEN over all bytes ⇒ no value can break its quotes;
skeleton constant ⇒ object well-formed. Confirmed per-object by `json_is_valid(json)`.

**Understand (parse)**
```
fields = json_parse(json)        // deterministic scan of the fixed frame + unescape
assert fields.title == title && fields.characters == chars && fields.story == story
```

## 6. Deliverable

A single self-contained demo **`tests/jsonstory_demo.c`** + a **`make jsonstory`**
target (mirroring the `endgate` and `cce_json_bench` targets), reusing the CCE
word-LM and the `contract`, `coverage`, and `conformal` machinery. (Round-trip
losslessness uses the property-contract *style* — a direct exhaustive replay, not the
`property_check` API; see §4.3.) It prints:

- `json_escape_class` verdict — must be `CERT_PROVEN` (exhaustive over 256).
- `json_unescape_tok` verdict — must be `CERT_PROVEN`.
- round-trip losslessness — must be **256/256**.
- name-gate: `btn_certify` pass + conformal coverage line.
- a generated JSON story object, its `json_is_valid()` result, and the parse-back
  field-equality assertion.

A consolidated regression assertion (escape PROVEN ∧ unescape PROVEN ∧ round-trip
256/256 ∧ `json_is_valid` on the emitted object ∧ parse-back equality) makes the demo
a green/red gate, runnable like the other `make`-target demos.

## 7. Testing approach (TDD)

Write the assertions first and watch them fail, then implement until green:

1. `btn_certify_exhaustive(json_escape_class)` returns `CERT_PROVEN` (fails before the
   net is trained to the full table).
2. `btn_certify_exhaustive(json_unescape_tok)` returns `CERT_PROVEN`.
3. round-trip replay == 256/256 (fails on any escaping/parsing bug).
4. `json_is_valid(emitted_object)` == true.
5. parse-back field equality holds for a generated object.

Reuse `json_is_valid()` as the independent oracle by **copying the minimal stack
validator** into the demo, keeping `jsonstory_demo.c` self-contained like
`endgate_demo.c` (no new shared header, no edit to `cce_json_bench.c`).

## 8. Out of scope (YAGNI)

- Streaming / incremental JSON LM (that is the `cce_json_bench` foil).
- Arbitrary or nested JSON schemas — the fixed `{title, characters, story}` only.
- Persisting certificates (CNET never persists certs; replay is cheap).
- Full NER for `characters` — the per-word conformal gate is sufficient.

## 9. Project notes

- **No git on this repo.** The `.git` was removed; run no git commands. This spec is
  written to disk but not committed; verify state via the filesystem.
- Reuses, does not modify, the CCE core, contract, coverage, conformal, and property
  layers — zero core-router edits expected, consistent with prior domain additions.
