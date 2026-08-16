# CRC intent recoverer (development)

Status: **DEVELOPMENT RECOVERER — F11 SCORES UNCHANGED**

## What this is

A fail-closed, schema-guided CRC recoverer on the typed admit path. When
WordLM abstains, `crc8_atm` may be proposed only if the existing closed
token-role frame already binds ATM CRC-8 and exactly one in-range octet.
Generic checksum and non-ATM CRC-8 still refuse (S10 second-gate rule).

This is not a learned door. The pinned WordLM is not retrained. The
WordLM threshold is not lowered. No CRC capsule is added. The 1,296-row
capsule geometry is unchanged.

## What this is not

F11 cannot honestly lose the 16 covered CRC abstentions. The
authenticated v5 result remains:

```text
CNET_7B_COMPETE_PASS suite=CNET-ASI-5-v5 cnet_exact=432/448
crc8_atm 48/64
```

A new candidate/fixture freeze cycle is required before any new compete
score may be claimed. Broader claims stay **WITHHELD**.

## Evidence boundary

Rules were authored and stressed on a new answer-free CRC development
set (`cnet_compete_crc_dev_*`). That set is audited against
`benchmarks/cnet_asi5_v5/excluded_prompts.tsv`. It does not read
held-out / cases / digests / FREEZE_PROTOCOL / the 448, and it does not
write recovery from those rows or their paraphrases.

Frozen ASI-5 v5 fixture files are unchanged.

## Local gate

```text
make -j1 cnet_7b_crc_recover
```

No 8B compete. No GitHub Actions. No fake PASS.
