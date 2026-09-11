# Three bounded transformer-free capability measurements

Run from the repository root:

```sh
bash experiments/hdc_capabilities/build.sh
python3 experiments/hdc_capabilities/language.py
python3 experiments/hdc_capabilities/procedures.py
python3 experiments/hdc_capabilities/responses.py
python3 experiments/hdc_capabilities/run.py
make cnet_vsa_evidence_bench
```

Only the Python standard library and existing CNET C sources are used. There are
no model or network calls. The measurement driver pins itself to the first
available CPU, freezes and hashes all fixtures before scoring, and records raw
outputs and timings in `result/cnet_hdc_capabilities_20260911.json`.

Language compares the previous structural experiment's finite canonical parser
with a larger handwritten grammar. It is not a comparison with the separate
managed intent parser or a learned general language model. The 480 examples
contain 64 legacy statements, 192 additional supported surface combinations,
64 unseen constructions, and 160 required refusals. The lexicon and parser rules
are declared in source. Recombining known grammatical elements does not prove
understanding of genuinely new syntax; the unseen construction outcomes are
reported separately. Timing includes Python calls, with 21 repetitions.

Procedure acquisition compares example lookup with enumeration of 84 relation
programs, comprising four relations and lengths 1–3. Four independent graph-tool
demonstrations normally identify a program; no CNET answers are used as training
labels. The learner retains every consistent program and only predicts when they
all agree. The 80 tasks produce 480 fresh-graph queries: 192 covered, 96 with
inconsistent demonstrations, 96 underdetermined, and 96 outside the declared
program language. These are bounded program-synthesis measurements, not a test of
all CNET learning systems. The program-space prior, discovery time, and retained
memory count. A zero-error sample does not guarantee rejection of every procedure
outside that prior.

Response composition compares the actual `cnet_vsa_ngram_generate` primitive used
by capsule generation with a Python proof composer, and then with the promoted C
operator. Both generators receive the same facts and a proposed answer; the
baseline is even seeded with the proposed answer to isolate composition from
answer discovery. The baseline uses production steering/repetition parameters,
28 tokens, and a freshly built n-gram store. This primitive does not perform the
capsule scope checks; the experiment measures evidence validation and composition,
not end-to-end certified dispatch. No synthetic certification flag is set.

The 96 response cases contain 32 valid requests and 64 requests requiring refusal
(missing paths, multiple endpoints, contradictions, or a wrong proposed answer).
Scoring extracts the leading answer and factual triples, checks each triple
against the supplied signed facts, and checks for a complete supporting path.
There is no requirement to match one exact phrasing. Unstructured language beyond
these triples is not semantically adjudicated. The deterministic composer emits
only this restricted format. Generation and verification time count; n-gram
training and storage are recorded separately. Native timings exclude ctypes
marshalling; Python timings include Python call overhead. Warm native results are
medians of three runs after warm-up. Values below one microsecond should not be
interpreted as precise timer-free latency guarantees.

The C port is tested on the same frozen response fixtures for parity, then on a
separate seed with 2,048 arbitrary graphs and 4,096 queries. That differential test
uses an independent bitset oracle, includes cycles and intersecting branches,
and validates every accepted proof. Native sanitizers and malformed CLI input
checks run separately.

The grammar and procedure learners remain experimental because their additional
costs do not meet the admission requirement. The response operator is available
as an explicit CLI operation and C API. This adds a bounded capability; it does
not turn arbitrary crawler prose into trusted facts or replace ordinary capsule
generation automatically. Broad semantic grounding, arbitrary procedure learning,
open-ended writing, and transformer superiority remain WITHHELD.

Publishing validation: the adapter supports both the original working-tree encoder
API and the older encoder API on master, and records `response_encoder_profile`.
Use `--out result/cnet_hdc_capabilities_master_20260911` to retain a fresh master
run separately from the original experiment. The original JSON is an archived
working-tree measurement; its recorded source identities are not a claim that all
of those uncommitted prerequisite sources were published with this operator.
The clean-master results use the same frozen fixtures and are stored separately.
