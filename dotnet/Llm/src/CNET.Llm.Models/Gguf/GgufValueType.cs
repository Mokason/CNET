namespace CNET.Llm.Models.Gguf;

/// <summary>
/// GGUF metadata value types. Values match the GGUF specification.
/// </summary>
public enum GgufValueType : uint
{
    /// <summary>Unsigned 8-bit integer.</summary>
    UInt8 = 0,

    /// <summary>Signed 8-bit integer.</summary>
    Int8 = 1,

    /// <summary>Unsigned 16-bit integer.</summary>
    UInt16 = 2,

    /// <summary>Signed 16-bit integer.</summary>
    Int16 = 3,

    /// <summary>Unsigned 32-bit integer.</summary>
    UInt32 = 4,

    /// <summary>Signed 32-bit integer.</summary>
    Int32 = 5,

    /// <summary>32-bit IEEE float.</summary>
    Float32 = 6,

    /// <summary>Boolean (1 byte).</summary>
    Bool = 7,

    /// <summary>UTF-8 string (length-prefixed).</summary>
    String = 8,

    /// <summary>Typed array (element type + count + elements).</summary>
    Array = 9,

    /// <summary>Unsigned 64-bit integer.</summary>
    UInt64 = 10,

    /// <summary>Signed 64-bit integer.</summary>
    Int64 = 11,

    /// <summary>64-bit IEEE float.</summary>
    Float64 = 12
}
