using System.Text;
using System.Text.Json;

namespace CNET.Cce.Llm.Memory;

/// <summary>
/// What consolidation needs to see of a store — implemented by the live
/// <see cref="BlobStore"/> and by <see cref="GhostSnapshot"/>, the read-only
/// view an orchestrator takes while another process holds the writer lock.
/// </summary>
public interface IMemoryView
{
    IReadOnlyList<MemoryBlob> All();
    int UsageCount(long id);
    int UsageSessions(long id);
}

/// <summary>
/// A point-in-time, lock-free view of a store file. The data file is opened
/// FileShare.Read by writers, so snapshots work while a session is live; a
/// torn tail line parses as corrupt and is skipped, same as the loader.
/// </summary>
public sealed class GhostSnapshot : IMemoryView
{
    private readonly List<MemoryBlob> _blobs;
    private readonly Dictionary<long, int> _useCounts;
    private readonly Dictionary<long, HashSet<string>> _useSessions;

    /// <summary>Highest id seen, tombstoned included — the consolidation watermark.</summary>
    public long MaxSeenId { get; }
    public int CorruptLinesSkipped { get; }

    /// <summary>Total physical lines in the file.</summary>
    public int TotalLines { get; }

    /// <summary>Lines that are dead weight: tombstoned blobs, their tombstones,
    /// usage events referencing only dead ids, corrupt lines. The janitor's
    /// trigger signal.</summary>
    public int DeadLines { get; }

    internal GhostSnapshot(List<MemoryBlob> blobs, Dictionary<long, int> useCounts,
                           Dictionary<long, HashSet<string>> useSessions,
                           long maxSeenId, int corrupt, int totalLines, int deadLines)
    {
        _blobs = blobs;
        _useCounts = useCounts;
        _useSessions = useSessions;
        MaxSeenId = maxSeenId;
        CorruptLinesSkipped = corrupt;
        TotalLines = totalLines;
        DeadLines = deadLines;
    }

    public IReadOnlyList<MemoryBlob> All() => _blobs;
    public int UsageCount(long id) => _useCounts.GetValueOrDefault(id);
    public int UsageSessions(long id) =>
        _useSessions.TryGetValue(id, out var set) ? set.Count : 0;
}

/// <summary>
/// Append-only JSON-lines store of conversation blobs — the "ghost memory" that
/// persists across sessions.
/// </summary>
/// <remarks>
/// One blob per line, appended and flushed on every write, never rewritten. A
/// process that dies mid-write costs at most its final line: the loader keeps
/// every parseable line and counts the rest in <see cref="CorruptLinesSkipped"/>
/// instead of refusing the file. Ids continue monotonically across reopens, so
/// "newer" stays meaningful over the store's whole life.
/// <para>
/// Single-writer by design: the store holds its file exclusively for writing
/// (readers may tail it). A second session opening the same file gets a clear
/// <see cref="IOException"/> rather than interleaved writes and duplicate ids.
/// Sequential sessions — the ghost-memory pattern — share it naturally.
/// </para>
/// </remarks>
public sealed class BlobStore : IDisposable, IMemoryView
{
    /// <summary>A deletion event. The blob's original line stays in the file
    /// as history; this line masks it from loading and recall.</summary>
    private sealed record Tombstone
    {
        [System.Text.Json.Serialization.JsonPropertyName("del")]
        public required long Id { get; init; }

        [System.Text.Json.Serialization.JsonPropertyName("ts")]
        public required string TimestampUtc { get; init; }
    }

    /// <summary>A usage event: these blob ids were served into a prompt. The
    /// consolidation pass reads these to find memories worth teaching — "served
    /// into a prompt" is a stronger signal than "keyword-matched".</summary>
    private sealed record UsageEvent
    {
        [System.Text.Json.Serialization.JsonPropertyName("used")]
        public required long[] Ids { get; init; }

        [System.Text.Json.Serialization.JsonPropertyName("sid")]
        public required string SessionId { get; init; }

        [System.Text.Json.Serialization.JsonPropertyName("ts")]
        public required string TimestampUtc { get; init; }
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly FileStream _appendStream;
    private readonly FileStream _lockStream;
    private readonly Dictionary<long, MemoryBlob> _blobs = [];
    private readonly Dictionary<long, int> _useCounts = [];
    private readonly Dictionary<long, HashSet<string>> _useSessions = [];
    private readonly KeywordIndex _index;
    private readonly object _gate = new();
    private long _nextId;
    private bool _disposed;

    /// <summary>Lines that failed to parse at load — nonzero means a past crash truncated a write.</summary>
    public int CorruptLinesSkipped { get; }

    /// <summary>Number of blobs currently in the store.</summary>
    public int Count
    {
        get { lock (_gate) return _blobs.Count; }
    }

    /// <summary>Path of the backing file.</summary>
    public string Path { get; }

    private BlobStore(string path, FileStream appendStream, FileStream lockStream,
                      List<MemoryBlob> loaded, long maxSeenId, int corruptLines,
                      MemoryOptions options,
                      Dictionary<long, int> useCounts,
                      Dictionary<long, HashSet<string>> useSessions)
    {
        _useCounts = useCounts;
        _useSessions = useSessions;
        Path = path;
        _appendStream = appendStream;
        _lockStream = lockStream;
        CorruptLinesSkipped = corruptLines;
        _index = new KeywordIndex(options);

        foreach (MemoryBlob blob in loaded)
        {
            _blobs[blob.Id] = blob;
            _index.Add(blob);
        }
        // maxSeenId covers tombstoned ids too: a forgotten id is never reused,
        // so a receipt from any point in history stays unambiguous forever.
        _nextId = maxSeenId + 1;
    }

    /// <summary>
    /// Opens (or creates) the store at <paramref name="path"/>, loading and
    /// indexing all existing blobs.
    /// </summary>
    public static BlobStore Open(string path, MemoryOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        options ??= new MemoryOptions();

        string? dir = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);

        // Single-writer enforcement needs its own lock handle: on Unix, .NET
        // only maps FileShare.None to a real (flock) exclusive lock — other
        // share modes are advisory-only there, so two writers on the data file
        // would NOT conflict. The sidecar lock is exclusive on every platform,
        // released by the OS if the process dies; the data file itself stays
        // readable for tailing. Acquired BEFORE the load so a session handoff
        // cannot read a stale tail and mint duplicate ids.
        FileStream lockStream;
        try
        {
            lockStream = new FileStream(path + ".lock", FileMode.OpenOrCreate,
                FileAccess.ReadWrite, FileShare.None);
        }
        catch (IOException ex)
        {
            throw new IOException(
                $"ghost-memory store '{path}' is already open for writing in another session", ex);
        }

        try
        {
            // Tombstones are applied IN FILE ORDER, so a hand-recovered file
            // with blob/tombstone/blob sequences resolves exactly as written.
            (List<MemoryBlob> loaded, var useCounts, var useSessions,
             long maxSeen, int corrupt, _) = LoadFile(path);

            // bufferSize: 1 disables user-space buffering, so a failed write
            // (disk full) cannot leave a half-line lurking in a buffer to be
            // flushed by a LATER append, resurrecting a blob the caller was
            // told failed. Throughput is unaffected at this write size
            // (measured 8 us/blob including durability).
            var stream = new FileStream(path, FileMode.Append, FileAccess.Write,
                FileShare.Read, bufferSize: 1);
            try
            {
                // Heal a crash-truncated tail: FileMode.Append positions after
                // the garbage, so without this the next blob would concatenate
                // onto the truncated line and BOTH would be unparseable on the
                // following load.
                if (stream.Length > 0)
                {
                    using var tailCheck = new FileStream(path, FileMode.Open,
                        FileAccess.Read, FileShare.ReadWrite);
                    tailCheck.Seek(-1, SeekOrigin.End);
                    if (tailCheck.ReadByte() != '\n')
                    {
                        stream.WriteByte((byte)'\n');
                        stream.Flush();
                    }
                }

                return new BlobStore(path, stream, lockStream, loaded, maxSeen, corrupt, options,
                                     useCounts, useSessions);
            }
            catch
            {
                stream.Dispose();
                throw;
            }
        }
        catch
        {
            // Any failure after the lock was acquired must release it, or every
            // later Open in this process reports a bogus "already open".
            lockStream.Dispose();
            throw;
        }
    }

    /// <summary>
    /// A point-in-time read-only view — no writer lock taken, safe while a
    /// live session owns the store. This is how an orchestrator observes.
    /// </summary>
    public static GhostSnapshot Snapshot(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        (List<MemoryBlob> blobs, var useCounts, var useSessions, long maxSeen,
         int corrupt, int totalLines) = LoadFile(path);
        // Live weight: one line per live blob, plus usage lines that mention
        // at least one live id, plus at most one sentinel. Everything else in
        // the file is dead weight the janitor may archive.
        var liveIds = blobs.Select(b => b.Id).ToHashSet();
        bool hasSentinel = File.Exists(path) &&
            File.ReadLines(path).Any(l => l.Contains("\"maxseen\"", StringComparison.Ordinal));
        int liveLines = blobs.Count + CountLiveUsageLines(path, liveIds) + (hasSentinel ? 1 : 0);
        int deadLines = Math.Max(0, totalLines - liveLines);
        return new GhostSnapshot(blobs, useCounts, useSessions, maxSeen, corrupt,
                                 totalLines, deadLines);
    }

    private static int CountLiveUsageLines(string path, HashSet<long> liveIds)
    {
        int count = 0;
        if (!File.Exists(path)) return 0;
        foreach (string line in File.ReadLines(path))
        {
            if (!line.Contains("\"used\"", StringComparison.Ordinal)) continue;
            try
            {
                using JsonDocument doc = JsonDocument.Parse(line);
                if (doc.RootElement.TryGetProperty("used", out JsonElement used) &&
                    used.EnumerateArray().Any(e => liveIds.Contains(e.GetInt64())))
                    count++;
            }
            catch (JsonException) { }
        }
        return count;
    }

    /// <summary>
    /// The janitor: archives dead weight (tombstoned blobs and their
    /// tombstones, usage events for dead ids, corrupt lines) into
    /// <c>&lt;path&gt;.archive.jsonl</c> and rewrites the store lean.
    /// History is preserved — moved, never destroyed; deletion stays an
    /// event. A <c>maxseen</c> sentinel keeps the never-reuse-ids guarantee.
    /// Takes the writer lock itself; returns false without touching anything
    /// when a live session holds the store.
    /// </summary>
    /// <returns>Archived line count, or -1 when the store is busy.</returns>
    public static int Compact(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        if (!File.Exists(path)) return 0;

        FileStream lockStream;
        try
        {
            lockStream = new FileStream(path + ".lock", FileMode.OpenOrCreate,
                FileAccess.ReadWrite, FileShare.None);
        }
        catch (IOException)
        {
            return -1;   // a live session owns the store; the janitor yields
        }

        try
        {
            (List<MemoryBlob> blobs, _, _, long maxSeen, _, _) = LoadFile(path);
            var liveIds = blobs.Select(b => b.Id).ToHashSet();
            var byId = blobs.ToDictionary(b => b.Id);

            // Canonical rebuild: sentinel + live blobs re-serialized from the
            // authoritative loaded state (last-write-wins already applied) +
            // original usage lines that mention a live id. Everything else —
            // tombstones, dead/stale blob lines, dead usage, corrupt lines,
            // old sentinels — is archived. One path, no order ambiguity.
            var keep = new List<string>
            {
                JsonSerializer.Serialize(new Dictionary<string, object>
                    { ["maxseen"] = maxSeen,
                      ["ts"] = DateTime.UtcNow.ToString("o") }, JsonOptions),
            };
            keep.AddRange(blobs.Select(b => JsonSerializer.Serialize(b, JsonOptions)));

            var archive = new List<string>();
            foreach (string line in File.ReadLines(path))
            {
                if (string.IsNullOrWhiteSpace(line)) continue;
                bool skip = false, isLiveUsage = false;
                try
                {
                    using JsonDocument doc = JsonDocument.Parse(line);
                    JsonElement root = doc.RootElement;
                    if (root.TryGetProperty("maxseen", out _))
                        skip = true;   // folded into the new sentinel
                    else if (root.TryGetProperty("used", out JsonElement u))
                        isLiveUsage = u.EnumerateArray()
                                       .Any(e => liveIds.Contains(e.GetInt64()));
                    else if (root.TryGetProperty("id", out JsonElement idEl) &&
                             idEl.TryGetInt64(out long id) &&
                             byId.TryGetValue(id, out MemoryBlob? live))
                        // Identity comparison, not bytes: a foreign-written
                        // line with different spacing is still THIS live blob
                        // (kept in canonical form above), while a stale
                        // duplicate with different text is history to archive.
                        skip = root.TryGetProperty("text", out JsonElement t) &&
                               t.GetString() == live.Text;
                }
                catch (JsonException) { }

                if (skip) continue;
                if (isLiveUsage) keep.Add(line);
                else archive.Add(line);
            }

            if (archive.Count == 0) return 0;                   // nothing to do

            File.AppendAllLines(path + ".archive.jsonl", archive);
            string tmp = path + ".compact.tmp";
            File.WriteAllLines(tmp, keep);
            File.Move(tmp, path, overwrite: true);
            return archive.Count;
        }
        finally
        {
            lockStream.Dispose();
        }
    }

    /// <summary>
    /// Parses a store file: blobs, tombstones, and usage events, in file order.
    /// Shared by the live open and the read-only snapshot so the two views can
    /// never diverge on semantics.
    /// </summary>
    private static (List<MemoryBlob> Loaded, Dictionary<long, int> UseCounts,
                    Dictionary<long, HashSet<string>> UseSessions,
                    long MaxSeen, int Corrupt, int TotalLines) LoadFile(string path)
    {
        var byId = new Dictionary<long, MemoryBlob>();
        var order = new List<long>();
        var useCounts = new Dictionary<long, int>();
        var useSessions = new Dictionary<long, HashSet<string>>();
        long maxSeen = 0;
        int corrupt = 0;
        int totalLines = 0;
        if (File.Exists(path))
        {
            foreach (string line in File.ReadLines(path))
            {
                if (string.IsNullOrWhiteSpace(line)) continue;
                totalLines++;
                try
                {
                    using JsonDocument doc = JsonDocument.Parse(line);
                    JsonElement root = doc.RootElement;

                    // Compaction sentinel: preserves the never-reuse-ids
                    // guarantee after dead ids are archived away.
                    if (root.TryGetProperty("maxseen", out JsonElement ms))
                    {
                        long v = ms.GetInt64();
                        if (v > maxSeen) maxSeen = v;
                        continue;
                    }

                    if (root.TryGetProperty("del", out JsonElement del))
                    {
                        long dead = del.GetInt64();
                        if (dead > maxSeen) maxSeen = dead;
                        if (byId.Remove(dead)) order.Remove(dead);
                        continue;
                    }

                    if (root.TryGetProperty("used", out JsonElement used))
                    {
                        string sid = root.TryGetProperty("sid", out JsonElement se)
                            ? se.GetString() ?? "" : "";
                        foreach (JsonElement e in used.EnumerateArray())
                        {
                            long uid = e.GetInt64();
                            useCounts[uid] = useCounts.GetValueOrDefault(uid) + 1;
                            if (sid.Length > 0)
                                (useSessions.TryGetValue(uid, out var set)
                                    ? set : useSessions[uid] = new HashSet<string>(StringComparer.Ordinal))
                                    .Add(sid);
                        }
                        continue;
                    }

                    MemoryBlob? blob = root.Deserialize<MemoryBlob>(JsonOptions);
                    if (blob is not null && blob.Text.Length > 0)
                    {
                        if (!byId.ContainsKey(blob.Id)) order.Add(blob.Id);
                        byId[blob.Id] = blob;
                        if (blob.Id > maxSeen) maxSeen = blob.Id;
                    }
                    else
                    {
                        corrupt++;
                    }
                }
                catch (JsonException)
                {
                    corrupt++;
                }
            }
        }
        var loaded = new List<MemoryBlob>(order.Count);
        foreach (long id in order) loaded.Add(byId[id]);
        return (loaded, useCounts, useSessions, maxSeen, corrupt, totalLines);
    }

    /// <summary>Appends one blob, assigning its id, and indexes it. Durable on return.</summary>
    public MemoryBlob Append(string sessionId, int turn, string role, string text, int tokenHint)
    {
        ArgumentException.ThrowIfNullOrEmpty(sessionId);
        ArgumentException.ThrowIfNullOrEmpty(role);
        ArgumentException.ThrowIfNullOrEmpty(text);

        // UTF-8 cannot represent unpaired UTF-16 surrogates; the JSON encoder
        // replaces them with U+FFFD on disk. Apply the same replacement to the
        // in-memory copy so recall is identical before and after a reopen —
        // the verbatim guarantee is over valid Unicode text.
        if (text.AsSpan().IndexOfAnyInRange('\ud800', '\udfff') >= 0)
            text = string.Concat(text.EnumerateRunes());   // valid pairs survive; lone -> U+FFFD

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            var blob = new MemoryBlob
            {
                Id = _nextId++,
                SessionId = sessionId,
                Turn = turn,
                Role = role,
                TimestampUtc = DateTime.UtcNow.ToString("o"),
                Text = text,
                TokenHint = tokenHint,
            };

            byte[] line = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(blob, JsonOptions) + "\n");
            _appendStream.Write(line);
            _appendStream.Flush();

            _blobs[blob.Id] = blob;
            _index.Add(blob);
            return blob;
        }
    }

    /// <summary>
    /// Recalls the blobs most relevant to <paramref name="query"/>, best first.
    /// Empty when nothing in the store shares a discriminative keyword with the
    /// query — by design, never "closest anyway" matches.
    /// </summary>
    public List<MemoryBlob> Recall(string query, int maxResults, bool relaxed = false)
    {
        ArgumentNullException.ThrowIfNull(query);
        ArgumentOutOfRangeException.ThrowIfLessThan(maxResults, 1);

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            var hits = _index.Query(query, _nextId - 1, relaxed);
            var result = new List<MemoryBlob>(Math.Min(maxResults, hits.Count));
            foreach ((long id, _) in hits)
            {
                result.Add(_blobs[id]);
                if (result.Count == maxResults) break;
            }
            return result;
        }
    }

    /// <summary>
    /// Forgets one blob: recall and provenance lookups stop serving it, and a
    /// tombstone line makes the deletion durable. The original line remains in
    /// the file as history — deletion is an event, not an erasure — and the id
    /// is never reused.
    /// </summary>
    /// <returns>False when no such blob exists (or it was already forgotten).</returns>
    public bool Forget(long id)
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (!_blobs.Remove(id, out MemoryBlob? blob)) return false;

            var tombstone = new Tombstone
            {
                Id = id,
                TimestampUtc = DateTime.UtcNow.ToString("o"),
            };
            byte[] line = Encoding.UTF8.GetBytes(
                JsonSerializer.Serialize(tombstone, JsonOptions) + "\n");
            _appendStream.Write(line);
            _appendStream.Flush();

            _index.Remove(blob);
            return true;
        }
    }

    /// <summary>
    /// The earliest blobs of one session, in order. This is how temporal
    /// questions ("what was the first thing I said?") get answered: keyword
    /// recall cannot see ordering, but the store can.
    /// </summary>
    public List<MemoryBlob> SessionStart(string sessionId, int maxResults)
    {
        ArgumentException.ThrowIfNullOrEmpty(sessionId);
        ArgumentOutOfRangeException.ThrowIfLessThan(maxResults, 1);
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return _blobs.Values
                .Where(b => b.SessionId == sessionId)
                .OrderBy(b => b.Id)
                .Take(maxResults)
                .ToList();
        }
    }

    /// <summary>
    /// Durably records that these blobs were served into a prompt. Feeds the
    /// consolidation pass: memories that keep earning a slot in real prompts —
    /// across distinct sessions — are the ones worth teaching into weights.
    /// </summary>
    public void RecordUsage(IReadOnlyCollection<long> ids, string sessionId)
    {
        ArgumentNullException.ThrowIfNull(ids);
        ArgumentException.ThrowIfNullOrEmpty(sessionId);
        if (ids.Count == 0) return;

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            var evt = new UsageEvent
            {
                Ids = ids.ToArray(),
                SessionId = sessionId,
                TimestampUtc = DateTime.UtcNow.ToString("o"),
            };
            byte[] line = Encoding.UTF8.GetBytes(
                JsonSerializer.Serialize(evt, JsonOptions) + "\n");
            _appendStream.Write(line);
            _appendStream.Flush();

            foreach (long id in ids)
            {
                _useCounts[id] = _useCounts.GetValueOrDefault(id) + 1;
                (_useSessions.TryGetValue(id, out var set)
                    ? set : _useSessions[id] = new HashSet<string>(StringComparer.Ordinal))
                    .Add(sessionId);
            }
        }
    }

    /// <summary>How many prompts this blob has been served into, over the store's life.</summary>
    public int UsageCount(long id)
    {
        lock (_gate) return _useCounts.GetValueOrDefault(id);
    }

    /// <summary>How many distinct sessions served this blob into a prompt.</summary>
    public int UsageSessions(long id)
    {
        lock (_gate) return _useSessions.TryGetValue(id, out var set) ? set.Count : 0;
    }

    /// <summary>All live (non-forgotten) blobs, oldest first. A snapshot.</summary>
    public IReadOnlyList<MemoryBlob> All()
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return _blobs.Values.OrderBy(b => b.Id).ToList();
        }
    }

    /// <summary>Fetches a blob by id, for provenance display.</summary>
    public MemoryBlob? Get(long id)
    {
        lock (_gate)
            return _blobs.GetValueOrDefault(id);
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            _appendStream.Dispose();
            _lockStream.Dispose();
        }
    }
}
