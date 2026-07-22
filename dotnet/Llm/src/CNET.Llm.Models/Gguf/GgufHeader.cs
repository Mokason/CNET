namespace CNET.Llm.Models.Gguf;

/// <summary>
/// Parsed GGUF file header. Contains format version and entry counts.
/// </summary>
/// <param name="Version">GGUF format version (2 or 3).</param>
/// <param name="TensorCount">Number of tensor entries in the file.</param>
/// <param name="MetadataKvCount">Number of metadata key-value pairs.</param>
public readonly record struct GgufHeader(uint Version, ulong TensorCount, ulong MetadataKvCount);
