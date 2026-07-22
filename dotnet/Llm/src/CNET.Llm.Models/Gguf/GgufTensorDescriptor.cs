using CNET.Llm.Core.Configuration;
using CNET.Llm.Core.Tensors;

namespace CNET.Llm.Models.Gguf;

/// <summary>
/// Describes a single tensor entry in a GGUF file: its name, shape, quantization type, and byte offset within the data section.
/// </summary>
/// <param name="Name">Tensor name (e.g., "blk.0.attn_q.weight").</param>
/// <param name="Shape">Tensor dimensions.</param>
/// <param name="QuantizationType">Storage format / quantization scheme.</param>
/// <param name="DataOffset">Byte offset from the start of the data section.</param>
public readonly record struct GgufTensorDescriptor(
    string Name,
    TensorShape Shape,
    QuantizationType QuantizationType,
    ulong DataOffset);
