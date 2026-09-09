# Explicit-slot changed-boundary review

Reviewer: the separate read-only `constituent_boundary_review` agent context.
Scope: the third source artifact against `61d92cf`; no holdout, native execution,
builds or edits by the reviewer. This is a reused source-review context, not a
claim of fresh-context authorship or statistical independence.

Cycle 1 exercised 86 authored probes and identified loss of the byte operand's
representation role. Bare `byte A/B/E/F` selected ASCII without resolving the
spelling. Six new assertions failed before repair. Byte inputs now retain that
role and require a quoted scalar or explicit codepoint representation before a
ready proposal can escape; control/OOD refusal remains prior to clarification.
A transient local-variable compiler error was retained and corrected before the
successful 561-test focused run; compilation failure was not counted as RED.

Cycle 2 exercised 39 probes. It verified the ambiguity fix but found that the
byte guard rejected matching-delimiter literals for the quote characters
themselves. Two assertions failed before the core's exact single-character
literal predicate was shared with the byte guard. The focused run passed 563 tests.

Cycle 3 found no remaining required issue. All 40 authored probes behaved as
expected, covering quote literals, bare-byte ambiguity, malformed literals,
controls, out-of-domain operands, conflicting directions and extra actions.

Final reviewed identities:

- Proposal: `d6533870bd35c7e17b43346380a999e5974db76c11c6a775a39f2103c4a4d975`
- Syntax: `8e1fce0ba39610100aa9797753ca60e95e557580bd5c09e1214f197ce5c86f5f`
- DLL: `590d1ba2e46e6fafadb5d522253a3194ed8f6a5328d3f2f46494520c3941b213`

These are scoped review/probe results, not exhaustive security or language
acceptance. Every failed confirmation and review regression remains retained.
