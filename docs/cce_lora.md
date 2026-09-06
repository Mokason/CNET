# Native low-rank adapters

[cce_lora.h](../include/cce/cce_lora.h) defines an adapter for a frozen linear
map: `y += (alpha/rank) * (x A) B`, with A shaped input×rank and B
rank×output. B starts at zero. Applying accumulates into the existing output;
merging modifies the supplied dense weight tensor in place.

The API includes allocation/free, apply/merge, parameter counts, residual
training/evaluation, binary persistence and conversion to a two-block
LINEAR_HEAD cascade. Check negative result codes; a low training loss does
not itself authorize serving.

`cce_lora_set_train_A(lo, 0)` freezes A and trains B only. The earlier
documentation called this VeRA-style option merely planned; it now exists
in the public API. It still needs the same acceptance checks.

## Registry lifecycle

[registry_lora.h](../include/router/registry_lora.h) separates attachment,
teaching, certification and opt-in serving. The installed route/DAG executor
hook checks `lora_certified`: a freshly fitted delta does not automatically
enter that path. The policy counts fixes and regressions against a frozen
base and requires the configured net gain; rejection keeps the base path
rather than weakening the policy.

The public evaluation helper `registry_forward_with_lora` is different: it
applies an attached delta without checking certification or serving opt-in.
Do not expose that direct helper as unguarded certified serving.

The optional orchestrator hook supports cheap-adapter-first maintenance before
dense healing. Representative validation matters: a holdout containing only
known faults measures fixes but cannot measure regressions on base-correct
traffic. Supply an independent representative sampler when making that claim.

```sh
make cce_lora_test registry_lora_test personal_ai_lora_tick
```

Consult target definitions before running traffic-replay experiments.
`CNET_JTC_TRAFFIC_LOG` may contain private requests; training, validation and
evaluation slices must remain distinct. Labels must come from an independent
teacher, correction or verified tool.

Earlier adapter-vs-dense results and declined-adapter cases remain in the
[archive](MAINTENANCE.md), including distribution-dependent fallback behavior.
They do not establish universal quality/speed gains. For deep residual
adaptation, see [Lily](cce_lily.md); for capsule packages, see
[CAPSULE_CORE.md](CAPSULE_CORE.md).
