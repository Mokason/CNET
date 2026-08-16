# CNET-ASI-CHAT-1 freeze protocol

Status: **S1 CANDIDATE — NO FIXTURE YET**

This is a new suite, not a rewrite of CNET-ASI-5 v5. The authenticated
ASI-5 PASS journals stay in `/home/marble/.local/state/cnet/cnet_asi5_v5`.
CHAT-1 uses `/home/marble/.local/state/cnet/cnet_asi_chat1`.

## What is frozen at S1

Native C candidate for conversational CONTRACT:

- two-gate admission + certified capsules
- native slot speech (`cnet_utter_compose_native`)
- named rubric `cnet_chat_fluency_v1`
- fixture generator `tools/cnet_chat1_fixture.c`
- independence auditor and compete runner

No held-out TSV exists at S1. Floors are not lowered. Residual is not
CNET's voice. Broader claims stay WITHHELD.

## After S1

Seed is the first 16 hex digits of SHA-256 over:

```
<40-byte S1 commit>
CNET-ASI-CHAT-1
```

plus a trailing line-feed, interpreted as unsigned 64-bit hex.

Run the generator and independence audit, then F1 may add only:

- `benchmarks/cnet_asi_chat1/heldout.tsv`
- `benchmarks/cnet_asi_chat1/baseline_system.txt`
- `benchmarks/cnet_asi_chat1/digests.sha256`
- `include/cnet_chat1.h` freeze-commit macro

Then one `make -j1 cnet_chat1_compete` on the pinned Bonsai ROCm
listener. A FAIL starts a new freeze. Never start a second result path
on the same freeze.

## Official PASS

`CNET_CHAT_COMPETE_PASS` is CONTRACT only: beat baseline overall and
covered, covered coverage ≥ 0.95, selective ≥ 0.99, OOD abstain 1.0,
unsafe 0, compose guards 48. Fluency vs 8B is reported on
`cnet_chat_fluency_v1` and stays unofficial until a later freeze names
it as a floor.
