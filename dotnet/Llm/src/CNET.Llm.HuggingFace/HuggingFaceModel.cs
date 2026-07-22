using System.Text.Json.Serialization;

namespace CNET.Llm.HuggingFace;

/// <summary>
/// Model info returned by the HuggingFace API.
/// </summary>
public sealed class HuggingFaceModelInfo
{
    /// <summary>Repository ID (e.g. "TheBloke/Llama-2-7B-GGUF").</summary>
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    /// <summary>Repository author/organization.</summary>
    [JsonPropertyName("author")]
    public string? Author { get; set; }

    /// <summary>Model ID (may differ from repository ID).</summary>
    [JsonPropertyName("modelId")]
    public string? ModelId { get; set; }

    /// <summary>Latest commit SHA.</summary>
    [JsonPropertyName("sha")]
    public string? Sha { get; set; }

    /// <summary>Last modification timestamp.</summary>
    [JsonPropertyName("lastModified")]
    public DateTimeOffset? LastModified { get; set; }

    /// <summary>Whether the repository is private.</summary>
    [JsonPropertyName("private")]
    public bool IsPrivate { get; set; }

    /// <summary>Total download count.</summary>
    [JsonPropertyName("downloads")]
    public long Downloads { get; set; }

    /// <summary>Total like count.</summary>
    [JsonPropertyName("likes")]
    public long Likes { get; set; }

    /// <summary>Tags associated with the model.</summary>
    [JsonPropertyName("tags")]
    public List<string> Tags { get; set; } = [];

    /// <summary>Pipeline tag (e.g. "text-generation").</summary>
    [JsonPropertyName("pipeline_tag")]
    public string? PipelineTag { get; set; }

    /// <summary>Files in the repository.</summary>
    [JsonPropertyName("siblings")]
    public List<HuggingFaceSibling>? Siblings { get; set; }
}

/// <summary>
/// A file entry within a HuggingFace repository.
/// </summary>
public sealed class HuggingFaceSibling
{
    /// <summary>Relative filename within the repository.</summary>
    [JsonPropertyName("rfilename")]
    public string Filename { get; set; } = string.Empty;

    /// <summary>File size in bytes, if available.</summary>
    [JsonPropertyName("size")]
    public long? Size { get; set; }
}

/// <summary>
/// A file entry from the repository tree API (<c>/api/models/{id}/tree/main</c>).
/// </summary>
public sealed class RepoFileEntry
{
    /// <summary>File type ("file", "directory").</summary>
    [JsonPropertyName("type")]
    public string Type { get; set; } = string.Empty;

    /// <summary>Relative path within the repository.</summary>
    [JsonPropertyName("path")]
    public string Path { get; set; } = string.Empty;

    /// <summary>File size in bytes.</summary>
    [JsonPropertyName("size")]
    public long Size { get; set; }

    /// <summary>Git object ID.</summary>
    [JsonPropertyName("oid")]
    public string? Oid { get; set; }
}

/// <summary>
/// Represents a locally downloaded model.
/// </summary>
/// <param name="RepoId">HuggingFace repository ID.</param>
/// <param name="Filename">GGUF filename.</param>
/// <param name="FullPath">Full local file path.</param>
/// <param name="SizeBytes">File size in bytes.</param>
/// <param name="DownloadedAt">When the file was downloaded.</param>
public sealed record LocalModel(string RepoId, string Filename, string FullPath, long SizeBytes, DateTimeOffset DownloadedAt);
