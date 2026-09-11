# Three capability measurements — 2026-09-11

One bounded improvement was added: evidence-checked response composition, exposed through `cnet_vsa_cli explain-facts` and `cnet_vsa_evidence_response`. The grammar and procedure candidates remain experimental. No transformer was called. Broad transformer superiority remains WITHHELD.

| Measurement | Baseline | Candidate | Admission |
|---|---|---|---|
| Interpret supplied fact sentences | 64/320 valid statements; 160/160 required refusals | 256/320 valid; 160/160 refusals | Experimental: slower and more memory |
| Learn short relation programs | Example lookup: 0/192 fresh covered graphs; 288/288 refusals | 192/192 covered; 288/288 refusals | Experimental: learning and covered-query costs |
| Compose supported responses | N-gram: 2/96 fully supported outputs/refusals | C operator: 96/96 | Added as an explicit typed operation |

## What these results establish

Language: the baseline is the previous structural experiment’s canonical active/passive parser, not CNET’s separate managed intent subsystem. The candidate adds a handwritten lexicon and compositional normalization. It handles the 192 new combinations of supported forms, including passive roles, polarity, capitalization, and spacing. Both parsers refuse all 64 genuinely unseen constructions. This is grammar coverage expansion, not demonstrated acquisition of unfamiliar syntax.

Median call latency rises from 0.190 to 1.002 µs; even legacy statements rise from 0.446 to 0.500 µs. Compiled regex storage rises from 1,080 to 3,840 bytes, with another 1,230 bytes for the lexicon; peak traced call allocation rises from 1,342 to 1,425 bytes. These results do not meet the speed/memory criterion.

Procedures: the candidate searches all 84 relation sequences of lengths 1–3 over four relations, retaining every sequence consistent with independently verified examples. Only unanimous predictions are accepted. Training demonstrations and test graphs are disjoint; the search receives demonstration answers, not the hidden target program. The baseline is an explicit example-lookup control, not a claim that all existing CNET learning systems use this method. This establishes transfer within the declared program language, not arbitrary procedure acquisition.

Median discovery time is 92.1 µs versus 3.9 µs to build example lookup. Covered-query time rises from 0.782 to 0.952 µs. Aggregate latency falls because empty version spaces refuse cheaply; using that aggregate to claim universally faster execution would be misleading. Median retained per-task state falls from 5,440 to 124 bytes, but the shared program language costs another 6,008 bytes. Amortization over many tasks was not measured.

Responses: both lanes receive the same signed facts and proposed answer. The production n-gram primitive is seeded with that answer, giving it an answer-retention advantage; the experiment isolates composition and evidence validation. It uses production steering/repetition parameters and 28 tokens, without capsule scope routing. The new operator verifies the full graph condition and emits an answer plus a supporting path, or refuses. The sample contains 32 valid requests and 64 invalid or inconsistent requests.

The n-gram lane produces 2/32 complete supported answers and refuses 0/64 invalid requests. The new C operator produces 32/32 supported answers and refuses 64/64. Median measured native time is 468.1 µs for n-gram generation and below 1 µs for the operator (raw timer value 0.100 µs). Native timing excludes ctypes marshalling. The Python prototype also passes 96/96 at 1.172 µs median.

Example n-gram output begins with the supplied answer but can append unsupported triples. The evidence composer emits only a complete checked path. Scoring checks leading answer, explicit triple support, and path completeness; it does not require one exact wording. Unrestricted prose outside this syntax is not semantically adjudicated. This comparison therefore does not measure general writing quality.

## Added operation and resource accounting

Run `bin/cnet_vsa_cli explain-facts <facts.txt> <start-id> <relation-id>...`. The file contains positive or negative canonical fact rows; the response states the unique endpoint and one supporting path. The public in-memory API is in `include/cnet_vsa_evidence.h`. Full contract, example, limits, and exit codes are documented in `plans/cnet_vsa_evidence_operator_20260911.md`.

The operator admits 128 entities, eight relation IDs, eight hops and 512 facts. It detects contradictions on every reachable positive edge, permits intermediate branches that reconverge, and rejects multiple final endpoints. A mismatched proposed answer refuses. This checks consistency with supplied facts, not the external truth of those facts. It is not a new certified capsule or packaging system.

Operator memory: no heap, no mutable global state, 2,976-byte GCC stack frame, plus caller-owned fact/output storage. The n-gram engine baseline allocates 5,279,768 bytes. The CLI reserves at most 8 KiB for fact rows and bypasses general GPU/document initialization for this command. Setup/file I/O is outside the sub-microsecond operator timing.

The standalone operator adds 1,644 object text bytes and zero object data/BSS. Against a reconstructed pre-addition CLI using the same compiler and current unrelated sources, linked text grows 189,078→193,497 bytes, data 1,420→1,436, and BSS 14,120→14,328: 4,643 additional section bytes. That integration overhead is real; the improvement is lower runtime cost for the new evidence task, not zero growth of the executable. No GPU throughput or energy measurement was made.

## Verification and limits

- Missing-command RED reproduced before CLI integration. Native API assertions cover reconvergence, ambiguity, contradiction on an alternate branch, missing answers, wrong proposals, invalid bounds and short buffers.
- Malformed-input CLI checks pass, including embedded NUL, trailing content, excessive facts/lines, oversized rows and invalid IDs. The NUL check failed during development and was fixed with a bounded byte reader.
- Independent seed 20260913: 2,048 arbitrary cyclic/intersecting graphs, 4,096 query comparisons against an integer-bitset oracle; all agree and all 341 accepted proofs pass edge/role/path checks.
- Native AddressSanitizer and UndefinedBehaviorSanitizer checks pass.
- New evidence gate and existing CLI, q8, generation, router and verify-fast gates pass. Generated tracked `cce.dll` was restored to its pre-check contents; prior user work is preserved.
- Manual review covered input bounds, atomic output, negative-evidence semantics, memory, caller isolation and absence of network/teacher dependencies. No certification floors changed.

`auto` and the live crawler retain their current behavior. Automatic selection of this operator from arbitrary language is not established; the language candidate was not admitted. Grammar generalization, open-ended procedure learning, unrestricted synthesis and broad transformer superiority remain WITHHELD. The earlier three capsule safety findings are outside this change and remain open.

Frozen fixture SHA-256: `05dbad46e2e8d0d12a9cd9dc35c6e29791e6179ed74197033110b6e1b3b21378`. The adjacent JSON stores per-case outputs, timings, source identities and resource results. Timing repeats are not independent examples; surface/program families share procedural structure. The exact C port used the original frozen response fixtures for parity and a separate graph seed for broader validation. This original run records the pre-publication working tree; a fresh clean-master run is retained in `cnet_hdc_capabilities_master_20260911.json`.
