# SentencePiece-style encoding implementation

[SentencePieceEncoding.cs](SentencePieceEncoding.cs) defines this local
encoding's normalization, space representation and vocabulary/score behavior.
It is not interchangeable with GPT-style byte BPE.

Check exact token IDs and decoding behavior for the chosen GGUF metadata,
including leading/repeated spaces, Unicode and unknown-byte handling.
A readable round trip can hide an incompatible prompt token sequence.

See the [tokenizer guide](../../../docs/TOKENIZERS.md).
The original detailed walkthrough is preserved in the
[archive](../../../../../docs/MAINTENANCE.md); it does not establish support
for every external tokenizer pipeline.
