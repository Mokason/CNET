# CNET documentation

> **CNET builds ASI — Artificial Specialized Intelligence. In CNET, ASI never
> means Artificial Superintelligence. CNET is not claiming AGI. Broad semantic
> grounding exists to understand intent; deep competence comes from isolated,
> certified, portable Micro Tensor Kernels. Wikipedia-like accumulation may
> broaden coverage and composition over time, but unit count alone is not
> intelligence and all broader claims remain benchmark-gated.**

Guides describe source interfaces and limits, not the current state of a running
host. For measured claims, follow dated evidence and check its bound source.

## Start here

| Need | Guide |
| --- | --- |
| Understand CNET | [README](../README.md), [architecture](ARCHITECTURE.md) |
| Build and choose tests | [Build and verification](BUILD_AND_TEST.md) |
| Teach, compose and inspect capsules | [Capsule core](CAPSULE_CORE.md) |
| Export a verified workflow's capsule subset for offline reuse | [Composition reuse](COMPOSITION_REUSE.md) |
| Acquire bounded source facts | [Source evidence](CNET_SOURCE_EVIDENCE.md) |
| Retrieve untrusted public evidence through MCP | [MCP read brick](MCP_READ_BRICK.md) |
| Operate private table learning | [Policy-bounded learning](AUTONOMOUS_LEARNING.md) |
| Observe a natural-language task and approve external learning evidence | [Verified task core](VERIFIED_TASK_CORE.md) |
| Inspect recurring gaps and link captured tasks | [Captured task inbox](CAPTURE_TASK_INBOX.md) |
| Use approved learning through the owner Discord DM | [Verified learning ingress](DISCORD_VERIFIED_LEARNING.md) |
| Collect owner-only Discord demand | [Private capture](../tools/discord_capture/README.md), [rollout evidence](../result/cnet_discord_capture_20260907.md) |
| Train on AMD GPUs or test activation | [GPU training](GPU_TRAINING.md) |
| Understand selection authority | [Dispatch](dispatch.md), [execution tiers](EXECUTION_TIERS.md) |
| Operate local services | [Daemon](CNETD.md), [24/7 services](CNET_MARBLE_24_7.md), [web](CNET_WEB.md) |
| Review trust and release constraints | [Security](SECURITY.md), [release policy](RELEASE_POLICY.md) |
| Find remaining work | [Current plan](../tasks/plan.md), [checklist](../tasks/todo.md) |
| Recover retired documentation | [Maintenance and history](MAINTENANCE.md) |

## Runtime references

- Learning: [teacher path](TEACH_PATH.md), [coverage harvest](CERT_COVERAGE_HARVEST.md),
  [curriculum harvest](GOLD_CURRICULUM_HARVEST.md), [distillation](CNET_DISTILL.md),
  [improvement integration](improvement_engine_integration.md).
- Memory: [memory runtime](MEM_RUNTIME.md), [STM/LTM bridge](STM_LTM_BRIDGE.md),
  [KV index](KV_STREAM_INDEX.md), [attention integration](STREAM_INDEX_ATTEND.md),
  [weight epochs](WEIGHT_EPOCH.md), [continuity](CONTINUITY_WORKSPACE.md).
- Text surfaces: [ROE](ROE_ASI.md), [domain routing](DOMAIN_ROUTE.md),
  [aliases and context](QUERY_ALIAS_DIALOG_CTX.md), [utterances](UTTERANCE.md),
  [residual chat](OPEN_CHAT.md), [English pack](PACK_ENGLISH_BASIC.md),
  [speech pack](SPEECH_CAPSULE.md).
- Control: [autonomy charter](AUTONOMY_CHARTER.md), [inventory self-model](ROE_SELF_MODEL.md),
  [exploration](EXPLORE_TICK.md), [personality controls](NEUROMOD_PERSONALITY.md),
  [peer boundary](THIRD_WAY_MARBLE_PEER.md), [execution trace](THOUGHT_PROCESS.md),
  [trace format](CHAIN_OF_THOUGHT.md), [reply diagnostics](REPLY_THINK.md).
- Specialists: [Brain import](BRAIN_CNET_CAPSULE.md), [LoRA](cce_lora.md),
  [Lily](cce_lily.md), [MoE trainer](moe_train.md), [transformer-MoE](moe_xf.md),
  [improvement experiments](ASI_IMPROVE.md), [FIFO generation](GENERATE_FIFO.md),
  [tokenizer benchmark](gigatok_bench.md).
- Managed inference: [native/.NET harness](cnet_dotnet_inference_harness.md),
  [GPU offload](cnet_bounded_gpu_offload.md), [async context](cnet_async_context_pipeline.md),
  [Hermes](hermes_hosting.md), [managed library](../dotnet/Llm/README.md).

## Evidence and history

[Changelog](CHANGELOG.md) summarizes changes. [Results](../result/) retain dated
observations and [benchmarks](../benchmarks/) preserve frozen protocols.
[Plans](../plans/) include active contracts and historical decisions; they are
not automatically today's task queue. [Phase 1–3 taxonomy](phase123_benchmark_closure.md)
separates mechanism tests from external benchmark measurements.

[Generated claims](verified-today.generated.md) must come from `make claims`.
Missing prerequisites, WITHHELD outcomes and failed precision experiments are
not passes. A descriptor or routing score never grants trust.

[The documentation inventory](DOCUMENTATION_INVENTORY.tsv) records every original
Markdown/text path, its disposition, replacement and recovery hash.
