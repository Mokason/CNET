namespace CNET.Llm.Models.Gguf;

/// <summary>
/// A single GGUF metadata value with its type tag. The value is boxed for storage in the metadata dictionary.
/// This is metadata parsed once at load time — boxing overhead is irrelevant.
/// </summary>
/// <param name="Type">The GGUF value type.</param>
/// <param name="Value">The boxed value. Scalars are boxed primitives; strings are <see cref="string"/>; arrays are typed arrays.</param>
public readonly record struct GgufMetadataValue(GgufValueType Type, object Value);
