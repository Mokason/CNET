# Unicode 17 explicit case-change evidence

Two small real-source workloads for the existing private table-learning path.
The data is from [Unicode 17.0.0 UnicodeData.txt](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt),
interpreted using [UAX #44 revision 36](https://www.unicode.org/reports/tr44/tr44-36.html).
The data and derived tables are covered by the accompanying [Unicode license](LICENSE).

## Exact contract

| Dataset | Source field (zero-based) | Answered keys | Required abstentions |
| --- | --- | ---: | ---: |
| `unicode17_upper_latin1` | 12, Simple_Uppercase_Mapping | 58 | 198 |
| `unicode17_lower_latin1` | 13, Simple_Lowercase_Mapping | 56 | 200 |

Inputs are integer **code points** 0..255 (U+0000..U+00FF), not UTF-8 bytes.
Outputs are integer code points, with 16-bit capacity. Examples: uppercase
97→65 (`a`→`A`), 181→924 (`µ`→Greek capital mu), and 255→376 (`ÿ`→`Ÿ`).
Lowercase 65→97. Do not truncate outputs to 8 bits.

These are **explicit case-change lookups**, not text case converters. Empty
Unicode fields default to identity in Unicode; this deliberately partial CNET
contract **abstains** on them. Thus uppercase 65, sharp-s 223, controls and digits
abstain. This does not mean Unicode lacks their identity/default mappings. Full
uppercase sharp-s can expand to multiple characters; full/contextual/locale case
mapping, folding, normalization and decoding are outside this contract.

## Provenance and reproducibility

Acquired from the official versioned HTTPS URL on 2026-09-07. The raw excerpt is
exactly the first 256 LF-terminated lines of the full source, without rewriting.
Both extractors check the complete ordered U+0000..U+00FF inventory and 15 fields
per row. The Python converter selects nonempty field 12 or 13, converts hex to
canonical decimal, and emits `CNET_LOCAL_TABLE_V1`, authority `verified_tool`.
The managed integration test performs its own field extraction without calling
the converter or using the host runtime's Unicode casing version.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| Full upstream UnicodeData.txt (not vendored) | 2198209 | `2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c` |
| `UnicodeData-Latin1.txt` | 15707 | `75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4` |
| `unicode17_upper_latin1.tsv` | 546 | `d5a314a48db2e2e86712bf012c20ac40fd1ef68b6120a5cdbf6c5a6aa93916e3` |
| `unicode17_lower_latin1.tsv` | 530 | `54d8c8aa294fa803fb49d0f5ee2cb46357d09950b664163d70f43c2dfaf8d3b7` |
| `LICENSE`, from https://www.unicode.org/license.txt on acquisition date | 1995 | `e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96` |

Pins detect byte changes; they are not publisher signatures. Source approval
depends on the owner's reviewed official HTTPS acquisition and trusted checker.
No CNET answers supply labels. The excerpt is upstream evidence, and `.tsv` files
are input evidence for the existing capsule builder, not a new capsule format.

Run offline from the repository on Linux with Python 3.10+:

```sh
python3 scripts/unicode17_case_tables.py upper
python3 scripts/unicode17_case_tables.py lower
python3 -m unittest discover -s scripts -p test_unicode17_case_tables.py -v
```

The CLI writes only the complete table to stdout. `--source /absolute/UnicodeData.txt`
accepts the exact pinned full upstream file or exact excerpt; it never fetches
anything. Different versions, edits, truncation, newline changes, oversized
files, non-regular files and final symlinks refuse. There is no update-to-latest
option or caller-supplied replacement pin. Repository code and ancestors remain
trusted; this utility is not a general filesystem sandbox.

## Learn and verify

For a new private deployment, authorize **both exact IDs** with `verified_tool`
in its policy before initialization. Follow [the installation and import
contract](../../docs/AUTONOMOUS_LEARNING.md). Copy the reviewed `.tsv` files into
owner-private, single-link source files and import using the hashes above.
Keep this README and LICENSE with distributed source/derived data.

Then issue uncovered demand (`learning ask DEPLOYMENT unicode17_upper_latin1 97`
and `learning ask DEPLOYMENT unicode17_lower_latin1 65`), run the existing bounded
supervisor, and verify both datasets after acceptance. Do not mutate an existing
deployment's policy or reset an expired budget to perform this onboarding.

The repeatable integration gate creates its own disposable deployment:

```sh
make learning_native
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
  --no-restore --filter FullyQualifiedName~LearningUnicodeWorkloadTests --logger trx
```

It publishes the real managed entry, imports both tables, proves initial missing
coverage, admits two actual misses, and runs acquisition/activation with three
probation probes under one original 30-second budget. After both accept, it
checks all 512 dataset/key pairs using installed `verify`, then independently
rechecks all 512 against raw-source-derived expectations under the same final
native generation. Receipts are captured in test output/TRX. The fixture removes
only its own private deployment on exit; it does not install a permanent service.

This measures certified acquisition and coexistence of 114 explicit external
facts plus 398 required abstentions, not held-out prediction gain, realistic
demand performance, general text learning, GPU acceleration or 72-hour acceptance.
All broader claims remain WITHHELD. See the [decision](../../plans/cnet_unicode17_workload_20260907.md).

## Printable ASCII symbolic labels

Two additional bounded sources use the same pinned raw excerpt and license:
`ascii_category.symbols.tsv` and `ascii_bidi.symbols.tsv`. They cover exactly
the 95 Unicode code points U+0020..U+007E. Each key is the literal Unicode name
from field 1, replacing ASCII spaces with underscores and sorting the resulting
ASCII tokens. Hyphens remain: `HYPHEN-MINUS`, not `HYPHEN_MINUS`.

`ascii_category` returns the exact General_Category label from zero-based
field 2; `ascii_bidi` returns the exact Bidi_Class label from field 4. Examples:
`LATIN_CAPITAL_LETTER_A` maps to `Lu` / `L`, `DIGIT_ZERO` to `Nd` / `EN`, and
`SPACE` to `Zs` / `WS`, respectively. These are named property lookups, not a
bidirectional-text algorithm or recognition of arbitrary text. The literal
token `SPACE` is covered; an actual space, `A`, aliases, alternate casing,
controls, DEL and non-ASCII character names are not vocabulary members.
Unknown tokens must abstain. There is no input normalization.

Each source is canonical ASCII `CNET_LOCAL_SYMBOLS_V1`: dataset ID, authority
`verified_tool`, `input_bits 8`, `output_bits 16`, `rows 95`, then sorted
`TOKEN<TAB>LABEL` rows with a final LF. Input bit capacity refers to the bounded
vocabulary index, not the original Unicode code point or UTF-8 bytes. Labels
retain their literal spelling. These files are evidence for the existing
capsule path, not another capsule packaging format.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `ascii_category.symbols.tsv` | 2076 | `f967c633b453c9e8a4d38ddf55484f16bce97d5fac6e60275e9adbe1484e848a` |
| `ascii_bidi.symbols.tsv` | 2020 | `c3c00de586e292da4ad197624b3aaef598a8d170c27283b251a99ed9d4523759` |

Both immutable vocabulary pins are
`ba3fe8b2440a6065c04e714136c1ff742e7e7ab772902ff4558fe3b2699b6984`.
This hashes **only** the 95 sorted ASCII tokens, each followed by LF: no header,
labels, tabs or JSON. Changing a label changes the source digest, not the
vocabulary digest. [The provenance JSON](ascii_symbol_tables.provenance.json)
records upstream, raw, source and vocabulary pins, field selection and coverage.
These integrity pins are not publisher signatures or new source authentication.

Reproduce offline on Linux with Python 3.10+:

```sh
python3 scripts/unicode17_symbol_tables.py ascii_category
python3 scripts/unicode17_symbol_tables.py ascii_bidi
python3 scripts/unicode17_symbol_tables.py provenance
python3 tests/test_unicode17_symbol_tables.py
```

The extractor accepts only the exact bundled 15,707-byte raw prefix, including
with `--source`; unlike the case extractor it does not accept the full upstream
file. It refuses modified, truncated, oversized, non-regular and final-symlink
sources without partial table output. It does not fetch data, consult the host
Unicode database, rewrite files, or update pins. Repository code and ancestors
remain trusted. Retain this README and LICENSE with distributed derived data.

The extraction tests establish byte-exact external labels and vocabulary, not
autonomous acceptance or production deployment. The managed/native integration
gate separately measures acceptance and live behavior. These 190 supplied
property facts are finite source lookups, not held-out generalization or broad
semantic competence; those claims remain WITHHELD. Never use CNET answers as
labels or modify an existing deployment's immutable policy to add these sources.
