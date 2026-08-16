# CNET-ASI-5 v1 held-out result

Status: **FAIL**. Broader claims remain **WITHHELD**.

This is the immutable first held-out result for evaluator commit
`d5babfd5aaf2b9a9173147a233bf0788d2b70ebb` and tree
`d121d5e90cd9010fc89b258ef5ed0f83f31bbb70`. All 448 rows ran exactly once
for each backend. The result is recorded even though CNET beat the pinned
baseline on the comparative metrics, because four absolute gates did not pass.

| Metric | CNET native C | Bonsai 8B CPU Q1_0 |
|---|---:|---:|
| Overall exact decisions | 376/448 (0.839285714) | 197/448 (0.439732143) |
| Covered exact decisions | 272/320 (0.850000000) | 98/320 (0.306250000) |
| Covered answer coverage | 272/320 (0.850000000) | 211/320 (0.659375000) |
| Selective accuracy | 272/296 (0.918918919) | 98/234 (0.418803419) |
| OOD correct abstention | 104/128 (0.812500000) | 99/128 (0.773437500) |
| Unsafe OOD answers | 24 | 29 |
| Contract violations | 0 | 9 |
| Invocation failures | 0 | 0 |
| Median latency | 0.125356 ms | 718.490771 ms |
| p95 latency | 0.342214 ms | 893.763723 ms |

Per-lane exact accuracy:

| Lane | CNET native C | Bonsai 8B CPU Q1_0 |
|---|---:|---:|
| `increment_mod256` | 40/64 | 30/64 |
| `minutes_to_seconds` | 64/64 | 46/64 |
| `crc8_atm` | 64/64 | 0/64 |
| `access_policy_v1` | 64/64 | 20/64 |
| `compose3_mod256` | 40/64 | 2/64 |
| OOD | 104/128 | 99/128 |

The CNET artifact contains a 91,581-parameter base and six certified capsules.
Its complete size is 312,669 bytes, versus 1,158,654,496 bytes and
8,188,548,096 parameters for the pinned baseline. Six units passed 1,296
exhaustive certification rows, portable import, corruption refusal, and
incompatibility refusal before the held-out run.

Failed immutable gates:

- covered answer coverage was 0.85; required at least 0.95;
- selective accuracy was 0.918918919; required at least 0.99;
- unsafe OOD answers were 24; required zero;
- only 120 composition per-hop checks were observed; full covered composition
  requires 192.

Evidence SHA-256:

| Evidence | SHA-256 |
|---|---|
| Terminal scorecard | `60ab3a9ff94dc4c415177338e22d6654ea37a95a7311b0e42a9849a0428640b1` |
| Baseline journal | `85f9371887f645d6e8f393a9e7f176aa9456fb3b2c7614ae3c2519d9f107fbaf` |
| CNET journal | `8b9f0ad7947bb2cf353f73d2518d6ff65ef9a913bb5bcb986d313f1ecb500ac3` |
| CNET artifact manifest | `88eec77f3d9acdbd9685813e272041df4e6b503e402152a646b9e826197b4b19` |

No individual held-out prompt or answer was inspected after scoring. Follow-up
work may use only the aggregate lane and gate results above plus independent
public development cases. Any subsequent comparison must use a newly frozen
benchmark version; v1 cannot be rerun as evidence for a modified system.
