# Managed adapter interface: not implemented end to end

[IAdapterManager](../src/CNET.Llm.Engine/IAdapterManager.cs) and
[LoraAdapter](../src/CNET.Llm.Engine/LoraAdapter.cs) define proposed data/API
shapes. There is no concrete manager or inference integration establishing
SafeTensors adapter loading, per-request selection, hot unload or
multi-adapter batching.

The old walkthrough and overhead/size estimates are design history in the
[archive](../../../docs/MAINTENANCE.md), not supported runtime behavior.
Do not call an interface declaration a working hot-swap implementation.

Native CNET has separate [low-rank adapter](../../../docs/cce_lora.md) and
[MTK/capsule](../../../docs/CAPSULE_CORE.md) boundaries. Those mechanisms
do not implicitly implement this managed interface, and their formats are
not interchangeable. New integration needs identity, cache invalidation,
held-out acceptance and lifetime tests before activation.
