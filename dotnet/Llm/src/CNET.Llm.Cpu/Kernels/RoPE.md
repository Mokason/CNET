# RoPE kernel reference

[RoPE.cs](RoPE.cs) is the implementation authority for dimension pairing,
frequency calculation and in-place tensor updates. Its pairing convention
must match the loaded Q/K weights, not an assumed model-family default.

The managed [position guide](../../../docs/POSITION_ENCODING.md) identifies
the supported configuration boundary. Parsed scaling metadata is not an
implemented context extension.

The original mathematical walkthrough is retained in the repository
[documentation archive](../../../../../docs/MAINTENANCE.md).
When changing the kernel, compare exact positions, shapes and pairing modes
against an independent reference before judging generated text.
