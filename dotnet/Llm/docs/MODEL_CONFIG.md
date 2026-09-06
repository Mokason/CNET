# Model configuration and support

[ModelConfig](../src/CNET.Llm.Core/Models/ModelConfig.cs) stores architecture,
dimensions, head counts, context and positional settings.
[GgufModelConfigExtractor](../src/CNET.Llm.Models/Gguf/GgufModelConfigExtractor.cs)
maps metadata into it.

Parsed metadata and recognized enums are not runtime support.
The common [TransformerModel](../src/CNET.Llm.Models/Architectures/TransformerModel.cs)
and its factory determine executable configurations.
The factory rejects DeepSeek/MLA; do not infer a working model from its
metadata being recognized.

RoPE scaling fields can be parsed/displayed without being applied.
The current forward uses dimension, theta and pairing type; see
[POSITION_ENCODING.md](POSITION_ENCODING.md).
Increasing a configured maximum does not prove a valid context extension.

Validate each model's tensor shapes, tokenizer/chat template and numerical
parity. Preserve model identity in benchmark results and fail visibly for
unsupported configurations.
