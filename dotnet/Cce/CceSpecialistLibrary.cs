using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace CNET.Cce;

/// <summary>
/// High-level manager for a "library" of many small CCE specialists (forests).
/// Makes it easy to load dozens/hundreds of .cce archives, compose them,
/// seal for fast inference, and get rough memory characteristics.
///
/// This directly supports the "many small specialists" use case where PyTorch
/// would be heavy (loading many separate state_dicts + modules).
/// </summary>
public sealed class CceSpecialistLibrary : IDisposable
{
    private readonly Dictionary<string, CceForest> _byName = new(StringComparer.OrdinalIgnoreCase);
    private readonly List<CceForest> _all = new();
    private bool _disposed;

    /// <summary>
    /// Load a forest from a .cce file under a logical name.
    /// </summary>
    /// <param name="path">Path to .cce archive.</param>
    /// <param name="name">Logical name for lookup.</param>
    /// <param name="maxBranches">Max branches expected.</param>
    /// <param name="sealImmediately">Seal right after load for zero-copy inference (recommended for prod).</param>
    public CceForest LoadForest(string path, string name, int maxBranches = 128, bool sealImmediately = true)
    {
        ThrowIfDisposed();
        if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("Name required", nameof(name));
        if (_byName.ContainsKey(name)) throw new InvalidOperationException($"A forest named '{name}' is already loaded.");

        var forest = CceForest.Open(path, maxBranches);
        if (sealImmediately)
        {
            try { forest.Seal(); } catch { /* may already be or no-op */ }
        }

        _byName[name] = forest;
        _all.Add(forest);
        return forest;
    }

    /// <summary>
    /// Load all .cce files from a directory. Names default to file name without extension.
    /// </summary>
    public void LoadDirectory(string directory, string? prefix = null, bool sealImmediately = true)
    {
        ThrowIfDisposed();
        foreach (var file in Directory.EnumerateFiles(directory, "*.cce", SearchOption.TopDirectoryOnly))
        {
            string logical = Path.GetFileNameWithoutExtension(file);
            if (!string.IsNullOrEmpty(prefix)) logical = prefix + logical;
            LoadForest(file, logical, sealImmediately: sealImmediately);
        }
    }

    public CceForest this[string name] => Get(name);

    public CceForest Get(string name)
    {
        ThrowIfDisposed();
        if (!_byName.TryGetValue(name, out var f))
            throw new KeyNotFoundException($"No forest named '{name}' in library.");
        return f;
    }

    public bool TryGet(string name, out CceForest? forest) => _byName.TryGetValue(name, out forest);

    public IReadOnlyCollection<string> Names => _byName.Keys;

    public int Count => _byName.Count;

    /// <summary>
    /// Seal every loaded forest (enables zero-copy views everywhere).
    /// </summary>
    public void SealAll()
    {
        ThrowIfDisposed();
        foreach (var f in _all)
        {
            if (!f.IsInvalid)
            {
                try { f.Seal(); } catch { }
            }
        }
    }

    /// <summary>
    /// Rough memory estimate in bytes based on parameter counts of all branches.
    /// This is a lower bound (does not account for overhead, tensors, etc.).
    /// For more accurate, combine with process working set or add native stats.
    /// </summary>
    public long GetApproximateParameterBytes()
    {
        ThrowIfDisposed();
        long total = 0;
        foreach (var f in _all)
        {
            if (f.IsInvalid) continue;
            try
            {
                var summary = f.Describe();
                total += summary.Branches.Sum(b => b.ParameterCount) * sizeof(float); // weights + bias approx
            }
            catch { }
        }
        return total;
    }

    /// <summary>
    /// Returns a summary string useful for logging startup cost of the library.
    /// </summary>
    public string GetMemoryReport()
    {
        long bytes = GetApproximateParameterBytes();
        return $"CceSpecialistLibrary: {Count} forests, ~{bytes / 1024.0:F1} KiB parameters (float32). " +
               "Use sealed zero-copy for lowest RSS on many specialists.";
    }

    public void Dispose()
    {
        if (_disposed) return;
        foreach (var f in _all)
            f.Dispose();
        _byName.Clear();
        _all.Clear();
        _disposed = true;
    }

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed, this);
}