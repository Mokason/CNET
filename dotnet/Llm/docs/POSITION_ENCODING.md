# Rotary position encoding

The implemented CPU path uses
[RoPEPositionEncoding](../src/CNET.Llm.Cpu/PositionEncoding/RoPEPositionEncoding.cs)
and [RoPE kernels](../src/CNET.Llm.Cpu/Kernels/RoPE.cs).
Dimension count, base frequency and element-pairing type must match the model's
Q/K weight convention.

[RoPEConfig](../src/CNET.Llm.Core/PositionEncoding/RoPEConfig.cs) also stores
scaling fields, but the current TransformerModel forward does not apply
YaRN, NTK, linear or LongRoPE extension from those fields.
ALiBi and absolute-position alternatives are not implemented here.

Do not infer longer-context correctness from accepted metadata or a larger
requested buffer. Verify positions and exact forward behavior within the
supported range. A context extension needs both kernel tests and an
identified long-context quality gate.

The old mathematical/design walkthrough remains in the
[archive](../../../docs/MAINTENANCE.md).
