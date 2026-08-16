# CNET-ASI-5 v3 results

Status: **FAIL — four preregistered absolute gates missed; broader claims WITHHELD**

The frozen one-shot run used fixture-freeze commit
`e94b5dfa360bb78acffb4399bd0844af5f18ba6e` (tree
`a45b0a4376e54be203c6f375383c235f84fa31db`) and artifact manifest
`a2df92efd81f2d0780894d14da992f6974db7591bd8c5cb7139ad782533c79b3`.
Both backends completed all 448 rows exactly once. No individual held-out
answer was inspected before scoring, and no floor was changed.

## Aggregate result

| Metric | Bonsai 8B baseline | CNET native C | Difference |
|---|---:|---:|---:|
| Overall exact | 204/448 (45.5357%) | 283/448 (63.1696%) | CNET +79 rows / +17.6339 pp |
| Covered exact | 107/320 (33.4375%) | 160/320 (50.0000%) | CNET +53 rows / +16.5625 pp |
| OOD correct abstention | 97/128 (75.7813%) | 123/128 (96.0938%) | CNET +26 rows / +20.3125 pp |
| Selective accuracy | 107/282 (37.9433%) | 160/165 (96.9697%) | CNET +59.0264 pp |
| Unsafe OOD answers | 31 | 5 | CNET -26 |
| Median latency | 882.451158 ms | 0.129212 ms | CNET 6,829.483x faster |
| P95 latency | 1,035.565187 ms | 0.358885 ms | CNET 2,885.507x faster |

CNET exceeded the pinned baseline on all three comparative accuracy gates.
That bounded comparison does not constitute a PASS because the plan also
requires strict absolute safety and coverage floors.

## Size

| Property | Bonsai 8B baseline | CNET native C | Ratio |
|---|---:|---:|---:|
| Parameters | 8,188,548,096 | 91,581 | CNET is 89,413.176x smaller |
| Complete artifact bytes | 1,158,654,496 | 312,669 | CNET is 3,705.690x smaller |
| CNET base bytes | — | 55,755 | — |
| CNET capsule bytes | — | 255,109 | — |

The CNET artifact contains six independently portable certified capsules with
1,296 exhaustive certification rows. Corruption and incompatible manifests
were refused, residual calls were zero, and the sanitizer gates passed.

## Failed immutable gates

1. Covered answer coverage was 160/320 (50.00%), below 95%.
2. Selective accuracy was 160/165 (96.9697%), below 99%.
3. Five OOD rows were answered unsafely; the floor is zero.
4. Only 16 composed rows were answered with three coverage checks each
   (48 total checks); all 64 composed rows are required.

The terminal verdict was:

`CNET_7B_COMPETE_FAIL suite=CNET-ASI-5-v3 failed_gates=4 broader_claims=WITHHELD`

## Evidence identities

| Evidence | SHA-256 |
|---|---|
| Aggregate result log | `2223e4e872e4ded65aefa5d985d5ee8591337381bdfd7e8649cdd133834b7e31` |
| Archive build evidence | `f845a12889c94e053527a2e73e0952f6b45ce8fec3f26b264436ace55f678cde` |
| Capsule sanitizer evidence | `2eb3176db8887d79ab8f1dd80edeb4e3562092fd9dc3716ed36b8d70793531f0` |
| Baseline journal | `f0db2d38a68d7e223dadb321ab7a22f2772e4b232acc9830422fabb30909ea97` |
| Baseline high-water anchor | `4634dde01452d19281d09e450437016a5f2b37ac1f22b63092de440de9d6ac1b` |
| CNET journal | `dabd6c33f8d82680e543e60b95c58e136270485ceacb3e7f65bda16a502529f9` |
| CNET high-water anchor | `8dcba5bbc98b5a1c04f111ebfbbcfc5fd80888689073603c4f6a69df24ef4ae9` |

The canonical journals and anchors remain owner-private under
`/home/marble/.local/state/cnet/cnet_asi5_v3/results`. The immutable scoring
contract is documented in `plans/cnet_7b_competition_v3_20260813.md`.
