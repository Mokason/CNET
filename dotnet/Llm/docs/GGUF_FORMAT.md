# GGUF loader reference

The managed loading path is implemented by
[GgufReader](../src/CNET.Llm.Models/Gguf/GgufReader.cs),
[GgufFile](../src/CNET.Llm.Models/Gguf/GgufFile.cs),
[metadata extraction](../src/CNET.Llm.Models/Gguf/GgufModelConfigExtractor.cs)
and [ModelLoader](../src/CNET.Llm.Models/ModelLoader.cs).

Loading reads a header, typed metadata, tensor descriptors and aligned tensor
data. A recognized header/enum does not establish support for every model,
quantization, tokenizer or historical GGUF variant. Test the exact file
against the implemented parser and model factory.

Mapped weights depend on the file/view lifetime. Keep the GGUF owner alive
for models referencing it, then dispose in the documented ownership order.
Do not replace a mapped model file under active readers.

This guide is not a claim the loader is hardened against arbitrary hostile
binary input. Use trusted model artifacts and preserve parser error/refusal
tests. A digest identifies bytes; it does not authenticate their source.

The original format tables remain in the [archive](../../../docs/MAINTENANCE.md).
Use [MODEL_CONFIG.md](MODEL_CONFIG.md), [QUANTIZATION.md](QUANTIZATION.md)
and [TOKENIZERS.md](TOKENIZERS.md) for adjacent runtime boundaries.
