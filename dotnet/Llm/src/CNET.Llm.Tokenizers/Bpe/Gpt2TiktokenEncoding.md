# GPT-style encoding implementation

[Gpt2TiktokenEncoding.cs](Gpt2TiktokenEncoding.cs) owns this encoding's
pretokenization and byte/vocabulary behavior.
[TiktokenPreTokenizer.cs](TiktokenPreTokenizer.cs) provides the model-pattern
selection machinery used by this path.

Do not infer identical token IDs from the generic “BPE” name. Tests must
include whitespace, Unicode, punctuation, contractions and special-token
boundaries for the exact vocabulary/pattern.

Use the [tokenizer guide](../../../docs/TOKENIZERS.md) for integration.
The original walkthrough remains in the
[archive](../../../../../docs/MAINTENANCE.md), not a claim of unmeasured
upstream parity or a universal speedup.
