using System;
using System.IO;
using System.Security.Cryptography;

namespace CnetMcpServer;

internal readonly record struct CnbFileStamp(long Length, long LastWriteTicks);

internal readonly record struct CnbFileIdentity(
    long Length,
    long LastWriteTicks,
    string Sha256)
{
    internal CnbFileStamp Stamp => new(Length, LastWriteTicks);
}

internal enum CnbGenerationChange
{
    Unavailable,
    Initial,
    Current,
    MetadataOnly,
    ContentChanged
}

internal static class CnbFileIdentityReader
{
    private const int MaxStableReadAttempts = 3;

    internal static bool TryReadStamp(string path, out CnbFileStamp stamp)
    {
        stamp = default;
        try
        {
            var info = new FileInfo(path);
            info.Refresh();
            if (!info.Exists) return false;
            stamp = new CnbFileStamp(info.Length, info.LastWriteTimeUtc.Ticks);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    internal static bool TryReadStable(
        string path,
        out CnbFileIdentity identity,
        Action<int>? afterOpen = null)
    {
        identity = default;
        for (int attempt = 0; attempt < MaxStableReadAttempts; ++attempt)
        {
            if (!TryReadStamp(path, out CnbFileStamp before)) return false;
            try
            {
                using var stream = new FileStream(
                    path,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.Read | FileShare.Delete,
                    bufferSize: 1024 * 1024,
                    options: FileOptions.SequentialScan);
                afterOpen?.Invoke(attempt);
                string digest = Convert.ToHexString(SHA256.HashData(stream));
                if (!TryReadStamp(path, out CnbFileStamp after)) return false;
                if (before != after || stream.Length != before.Length) continue;
                identity = new CnbFileIdentity(
                    before.Length, before.LastWriteTicks, digest);
                return true;
            }
            catch (IOException)
            {
                /* Atomic replacement or a transient writer: retry boundedly. */
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }
        return false;
    }
}

internal sealed class CnbGenerationTracker
{
    private readonly string _path;
    private CnbFileIdentity? _observed;
    private CnbFileIdentity? _applied;

    internal CnbGenerationTracker(string path)
    {
        _path = path;
    }

    internal long? AppliedLength => _applied?.Length;

    internal CnbGenerationChange Observe(out CnbFileIdentity identity)
    {
        identity = default;
        if (_observed is CnbFileIdentity observed &&
            CnbFileIdentityReader.TryReadStamp(_path, out CnbFileStamp stamp) &&
            stamp == observed.Stamp)
        {
            identity = observed;
        }
        else
        {
            if (!CnbFileIdentityReader.TryReadStable(_path, out identity))
                return CnbGenerationChange.Unavailable;
            _observed = identity;
        }
        if (_applied is not CnbFileIdentity current)
            return CnbGenerationChange.Initial;
        if (current == identity)
            return CnbGenerationChange.Current;
        return string.Equals(current.Sha256, identity.Sha256,
                             StringComparison.Ordinal)
            ? CnbGenerationChange.MetadataOnly
            : CnbGenerationChange.ContentChanged;
    }

    internal void MarkApplied(CnbFileIdentity identity)
    {
        _observed = identity;
        _applied = identity;
    }
}
