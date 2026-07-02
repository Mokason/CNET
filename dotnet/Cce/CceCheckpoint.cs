using System;
using System.IO;
using System.Linq;

namespace CNET.Cce;

/// <summary>Metadata for a managed CCE checkpoint file.</summary>
public sealed record CceCheckpointInfo(string FullPath, string Name, DateTimeOffset LastWriteTimeUtc);

/// <summary>
/// Managed checkpoint helpers over the native CCE model bundle format.
/// This standardizes path handling and latest-checkpoint discovery without adding a second serializer.
/// </summary>
public static class CceCheckpoint
{
    /// <summary>Saves <paramref name="model"/> as a .cce checkpoint under <paramref name="directory"/>.</summary>
    public static CceCheckpointInfo Save(CceModel model, string directory, string name = "checkpoint")
    {
        ArgumentNullException.ThrowIfNull(model);
        if (string.IsNullOrWhiteSpace(directory)) throw new ArgumentException("Directory is required", nameof(directory));
        if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("Checkpoint name is required", nameof(name));

        Directory.CreateDirectory(directory);
        string fileName = EnsureCceExtension(SanitizeFileName(name));
        string fullPath = Path.GetFullPath(Path.Combine(directory, fileName));

        model.Save(fullPath);
        return Describe(fullPath);
    }

    /// <summary>Loads a checkpoint created by <see cref="Save"/> or <see cref="CceModel.Save"/>.</summary>
    public static CceModel Load(string checkpointPath)
    {
        if (string.IsNullOrWhiteSpace(checkpointPath)) throw new ArgumentException("Checkpoint path is required", nameof(checkpointPath));
        return CceModel.Load(checkpointPath);
    }

    /// <summary>Returns the newest .cce checkpoint in <paramref name="directory"/>, or null when none exists.</summary>
    public static CceCheckpointInfo? Latest(string directory, string searchPattern = "*.cce")
    {
        if (string.IsNullOrWhiteSpace(directory)) throw new ArgumentException("Directory is required", nameof(directory));
        if (string.IsNullOrWhiteSpace(searchPattern)) throw new ArgumentException("Search pattern is required", nameof(searchPattern));
        if (!Directory.Exists(directory)) return null;

        string? newest = Directory
            .EnumerateFiles(directory, searchPattern, SearchOption.TopDirectoryOnly)
            .OrderByDescending(File.GetLastWriteTimeUtc)
            .FirstOrDefault();

        return newest is null ? null : Describe(newest);
    }

    private static CceCheckpointInfo Describe(string fullPath)
    {
        string resolved = Path.GetFullPath(fullPath);
        return new CceCheckpointInfo(
            resolved,
            Path.GetFileNameWithoutExtension(resolved),
            new DateTimeOffset(File.GetLastWriteTimeUtc(resolved), TimeSpan.Zero));
    }

    private static string EnsureCceExtension(string fileName) =>
        Path.GetExtension(fileName).Equals(".cce", StringComparison.OrdinalIgnoreCase)
            ? fileName
            : fileName + ".cce";

    private static string SanitizedFallback => "checkpoint";

    private static string SanitizeFileName(string name)
    {
        char[] invalid = Path.GetInvalidFileNameChars();
        string sanitized = new string(name
            .Select(ch => invalid.Contains(ch) || ch == Path.DirectorySeparatorChar || ch == Path.AltDirectorySeparatorChar ? '_' : ch)
            .ToArray()).Trim();
        while (sanitized.Contains("..", StringComparison.Ordinal))
            sanitized = sanitized.Replace("..", "_", StringComparison.Ordinal);
        sanitized = sanitized.Trim('.', ' ');
        return sanitized.Length == 0 ? SanitizedFallback : sanitized;
    }
}
