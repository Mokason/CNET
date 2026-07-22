using System.Text.Json.Serialization;

namespace CNET.Cce.Llm.Memory;

/// <summary>
/// One verbatim excerpt of past conversation — the unit of storage and recall.
/// </summary>
/// <remarks>
/// A blob is one message, or one part of a message that was split at paragraph
/// boundaries to stay under <see cref="MemoryOptions.MaxBlobTokens"/>. Blob
/// granularity (rather than sentence granularity) is deliberate: a sentence
/// fragment loses its referents, while a message-sized excerpt stays
/// self-contained, which is what makes recalled text quotable without
/// reinterpretation.
/// </remarks>
public sealed record MemoryBlob
{
    /// <summary>Monotonic store-wide id. Higher is newer. Assigned by the store.</summary>
    [JsonPropertyName("id")]
    public required long Id { get; init; }

    /// <summary>Groups the blobs of one conversation run (one process lifetime).</summary>
    [JsonPropertyName("sid")]
    public required string SessionId { get; init; }

    /// <summary>Turn number within the session; a user/assistant exchange shares one turn.</summary>
    [JsonPropertyName("turn")]
    public required int Turn { get; init; }

    /// <summary>"user" or "assistant".</summary>
    [JsonPropertyName("role")]
    public required string Role { get; init; }

    /// <summary>UTC timestamp, ISO-8601, assigned when stored.</summary>
    [JsonPropertyName("ts")]
    public required string TimestampUtc { get; init; }

    /// <summary>The verbatim text. Never summarized, never rewritten.</summary>
    [JsonPropertyName("text")]
    public required string Text { get; init; }

    /// <summary>
    /// Token count measured by the tokenizer of the session that stored it.
    /// Advisory only — a different model tokenizes differently, so budget
    /// decisions re-count with the live tokenizer.
    /// </summary>
    [JsonPropertyName("tok")]
    public required int TokenHint { get; init; }

    /// <summary>Schema version for forward migration.</summary>
    [JsonPropertyName("v")]
    public int Version { get; init; } = 1;
}
