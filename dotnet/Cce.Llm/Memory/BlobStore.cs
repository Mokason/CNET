using System.Text;
using System.Text.Json;

namespace CNET.Cce.Llm.Memory;

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
public sealed class BlobStore : IDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly FileStream _appendStream;
    private readonly FileStream _lockStream;
    private readonly Dictionary<long, MemoryBlob> _blobs = [];
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
                      List<MemoryBlob> loaded, int corruptLines, MemoryOptions options)
    {
        Path = path;
        _appendStream = appendStream;
        _lockStream = lockStream;
        CorruptLinesSkipped = corruptLines;
        _index = new KeywordIndex(options);

        long maxId = 0;
        foreach (MemoryBlob blob in loaded)
        {
            _blobs[blob.Id] = blob;
            _index.Add(blob);
            if (blob.Id > maxId) maxId = blob.Id;
        }
        _nextId = maxId + 1;
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

        var loaded = new List<MemoryBlob>();
        int corrupt = 0;
        if (File.Exists(path))
        {
            foreach (string line in File.ReadLines(path))
            {
                if (string.IsNullOrWhiteSpace(line)) continue;
                try
                {
                    MemoryBlob? blob = JsonSerializer.Deserialize<MemoryBlob>(line, JsonOptions);
                    if (blob is not null && blob.Text.Length > 0)
                        loaded.Add(blob);
                    else
                        corrupt++;
                }
                catch (JsonException)
                {
                    corrupt++;
                }
            }
        }

        // Single-writer enforcement needs its own lock handle: on Unix, .NET
        // only maps FileShare.None to a real (flock) exclusive lock — other
        // share modes are advisory-only there, so two writers on the data file
        // would NOT conflict. The sidecar lock is exclusive on every platform;
        // the data file itself stays readable for tailing.
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

        var stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read);

        // Heal a crash-truncated tail: FileMode.Append positions after the
        // garbage, so without this the next blob would concatenate onto the
        // truncated line and BOTH would be unparseable on the following load.
        if (stream.Length > 0)
        {
            using var tailCheck = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            tailCheck.Seek(-1, SeekOrigin.End);
            if (tailCheck.ReadByte() != '\n')
            {
                stream.WriteByte((byte)'\n');
                stream.Flush();
            }
        }

        return new BlobStore(path, stream, lockStream, loaded, corrupt, options);
    }

    /// <summary>Appends one blob, assigning its id, and indexes it. Durable on return.</summary>
    public MemoryBlob Append(string sessionId, int turn, string role, string text, int tokenHint)
    {
        ArgumentException.ThrowIfNullOrEmpty(sessionId);
        ArgumentException.ThrowIfNullOrEmpty(role);
        ArgumentException.ThrowIfNullOrEmpty(text);

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
    public List<MemoryBlob> Recall(string query, int maxResults)
    {
        ArgumentNullException.ThrowIfNull(query);
        ArgumentOutOfRangeException.ThrowIfLessThan(maxResults, 1);

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            var hits = _index.Query(query, _nextId - 1);
            var result = new List<MemoryBlob>(Math.Min(maxResults, hits.Count));
            foreach ((long id, _) in hits)
            {
                result.Add(_blobs[id]);
                if (result.Count == maxResults) break;
            }
            return result;
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
