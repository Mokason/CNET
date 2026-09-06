# Managed inference architecture

Implemented flow: local GGUF → model/configuration/tokenizer → TextGenerator →
sampling/constraints → output. The CLI and optional server wrap these pieces;
the CNET bridge adds a separate shared-session contract.

| Component | Source |
| --- | --- |
| Loading | [ModelLoader](../src/CNET.Llm.Models/ModelLoader.cs) |
| Common model | [Architectures](../src/CNET.Llm.Models/Architectures/) |
| Generation | [TextGenerator](../src/CNET.Llm.Engine/TextGenerator.cs) |
| CPU kernels | [CpuBackend](../src/CNET.Llm.Cpu/CpuBackend.cs) |
| Tokenization | [Tokenizers](../src/CNET.Llm.Tokenizers/) |
| Caches | [KvCache](../src/CNET.Llm.Engine/KvCache/) and PromptCache |
| Server routes | [EndpointExtensions](../src/CNET.Llm.Server/EndpointExtensions.cs) |

The old diagram included unrealized features: a SafeTensors loader, continuous
scheduler, complete adapter manager, embeddings endpoint and concrete
diagnostic/telemetry systems. Interface names are not implementation evidence.
The common model is TransformerModel, not the old diagram's separate
InferenceEngine/DeepSeek classes.

The sample server serializes requests with a semaphore; it has no priority
preemption or production authentication. See [SERVER.md](SERVER.md).
The managed bridge is CPU-only, and the vendor CUDA assembly is not ROCm.
Read [ROADMAP.md](ROADMAP.md) and focused references before extending this layer.
