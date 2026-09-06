# Quantized weights

Managed dispatch uses each tensor's quantization metadata; a mixed GGUF
need not use one format everywhere. Enum recognition, parsing, dequantization
and a usable matrix kernel are separate support checks.

Relevant code:
[QuantizationType](../src/CNET.Llm.Core/Configuration/QuantizationType.cs),
[Dequantize](../src/CNET.Llm.Cpu/Kernels/Dequantize.cs),
[K-quant dequantization](../src/CNET.Llm.Cpu/Kernels/DequantizeKQuants.cs)
and [K-quant matrix kernels](../src/CNET.Llm.Cpu/Kernels/MatMulKQuants.cs).
Preserve block layout, scale/minimum packing, row orientation and tail bounds
when modifying these kernels.

Decode vector products and prefill matrix products may choose different
paths. Speedups require exact-shape measurements and numerical comparison,
not only smaller file size. A quantization label does not prove maintained
model quality or capsule certification.

Original block-layout tables remain in the
[archive](../../../docs/MAINTENANCE.md). This page does not recommend the
vendor CUDA path; use native AMD guidance for supported device work.
