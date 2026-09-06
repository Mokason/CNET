# Tokenizers and chat templates

The managed [ITokenizer](../src/CNET.Llm.Tokenizers/ITokenizer.cs) is
implemented through model-specific BPE/encoding and GGUF factory paths.
[Gpt2TiktokenEncoding](../src/CNET.Llm.Tokenizers/Bpe/Gpt2TiktokenEncoding.cs)
uses regex pretokenization; the
[SentencePiece encoding](../src/CNET.Llm.Tokenizers/Bpe/SentencePieceEncoding.cs)
has distinct normalization/space/score behavior.

A tokenizer family name is not a substitute for exact vocabulary, merges,
special-token and pretokenizer metadata. Compare token IDs, not only
round-tripped readable text. UTF-8 fragments can span token boundaries;
use the generation detokenizer appropriately.

[JinjaChatTemplate](../src/CNET.Llm.Tokenizers/ChatTemplates/JinjaChatTemplate.cs)
implements a local template-language subset. Match the selected model's
template and generation-prompt options; do not hardcode a universal ChatML
fallback or claim arbitrary Jinja/Hugging Face pipeline support.

The old speed ratios and full tokenizer.json/SafeTensors workflow were not
established merely by these interfaces. Original reference text remains in
the [archive](../../../docs/MAINTENANCE.md).
