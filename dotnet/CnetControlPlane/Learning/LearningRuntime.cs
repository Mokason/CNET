using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;
using static CnetControlPlane.Learning.LearningLinux;

namespace CnetControlPlane.Learning;

internal enum LearningNativeCommand { BuildTable, VerifyTable, Snapshot, Control, Daemon }

/// <summary>
/// Attests the fixed private native installation against an owner manifest kept
/// outside its six-file directory. The owner approves the code and its no-fork
/// behavior; hashes do not establish code safety. Only the copied core library
/// is covered: the system loader and system libraries remain trusted, not
/// dynamically discovered or attested here. Clear the environment before exec.
/// Calls are serialized by the owner. Returned paths are not hostile-same-UID
/// TOCTOU isolation or an installation/deployment operation.
/// </summary>
internal sealed class LearningRuntime : IDisposable
{
    private static readonly string[] Names = ["cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so"];
    private readonly LearningFiles files;
    private readonly Dictionary<string, string> hashes;
    private bool disposed;
    public string Sha256 { get; }
    private LearningRuntime(LearningFiles files, Dictionary<string, string> hashes, string sha256)
    { this.files = files; this.hashes = hashes; Sha256 = sha256; }

    public static LearningRuntime Load(string root, byte[] manifest)
    {
        if (manifest is null || manifest.Length is < 1 or > 4096)
            throw new ArgumentException("learning_runtime_manifest_size");
        var snapshot = (byte[])manifest.Clone();
        var hashes = ParseManifest(snapshot);
        string sha256;
        try { sha256 = Convert.ToHexString(SHA256.HashData(snapshot)).ToLowerInvariant(); }
        catch (CryptographicException) { throw new InvalidOperationException("learning_runtime_io_refused"); }
        var files = LearningFiles.Open(root);
        try
        {
            var runtime = new LearningRuntime(files, hashes, sha256);
            runtime.Verify();
            return runtime;
        }
        catch { files.Dispose(); throw; }
    }

    private static Dictionary<string, string> ParseManifest(byte[] bytes)
    {
        static Dictionary<string, JsonElement> Fields(JsonElement element, params string[] names)
        {
            if (element.ValueKind != JsonValueKind.Object) throw new ArgumentException("learning_runtime_manifest_fields");
            var result = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
            foreach (var property in element.EnumerateObject())
                if (!result.TryAdd(property.Name, property.Value)) throw new ArgumentException("learning_runtime_manifest_fields");
            if (!result.Keys.ToHashSet(StringComparer.Ordinal).SetEquals(names))
                throw new ArgumentException("learning_runtime_manifest_fields");
            return result;
        }
        try
        {
            using var document = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 3 });
            var top = Fields(document.RootElement, "schema_version", "files");
            if (top["schema_version"].ValueKind != JsonValueKind.Number || top["schema_version"].GetRawText() != "1")
                throw new ArgumentException("learning_runtime_manifest_version");
            var entries = Fields(top["files"], Names);
            var hashes = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var name in Names)
            {
                if (entries[name].ValueKind != JsonValueKind.String)
                    throw new ArgumentException("learning_runtime_manifest_hash");
                var hash = entries[name].GetString();
                if (hash is not { Length: 64 } || hash.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f')))
                    throw new ArgumentException("learning_runtime_manifest_hash");
                hashes.Add(name, hash);
            }
            return hashes;
        }
        catch (Exception exception) when (exception is JsonException or InvalidOperationException)
        {
            throw new ArgumentException("learning_runtime_manifest_json");
        }
    }

    private void Inventory(SafeFileHandle root)
    {
        var count = 0;
        // Enumeration stops at the seventh entry; never materialize an
        // unbounded directory or follow a child name supplied by the inventory.
        var options = new EnumerationOptions { AttributesToSkip = 0, IgnoreInaccessible = false, RecurseSubdirectories = false };
        foreach (var entry in Directory.EnumerateFileSystemEntries($"/proc/self/fd/{root.DangerousGetHandle().ToInt64()}", "*", options))
            if (++count > Names.Length || !hashes.ContainsKey(Path.GetFileName(entry)))
                throw new InvalidOperationException("learning_runtime_inventory");
        if (count != Names.Length) throw new InvalidOperationException("learning_runtime_inventory");
    }

    public void Verify()
    {
        if (disposed) throw new InvalidOperationException("learning_runtime_disposed");
        try
        {
            files.AssertPathIdentity();
            using var root = Handle(open(files.FullPath, DirectoryFlag | NoFollow | CloseExec, 0));
            var rootBefore = Inspect(root);
            if ((rootBefore.Mode & 0xf000) != 0x4000 || (rootBefore.Mode & 0xfff) != 0x1c0 || rootBefore.Owner != geteuid())
                throw new InvalidOperationException("learning_runtime_root");
            Inventory(root);
            var handles = new List<SafeFileHandle>(Names.Length);
            try
            {
                var metadata = new Stat[Names.Length];
                ulong total = 0;
                for (var index = 0; index < Names.Length; index++)
                {
                    var handle = Handle(openat(root, Names[index], NoFollow | NonBlock | CloseExec, 0));
                    handles.Add(handle);
                    var value = metadata[index] = Inspect(handle);
                    var mode = index < 5 ? 0x140 : 0x100; // exact 0500 executables / 0400 library
                    if ((value.Mode & 0xf000) != 0x8000 || (value.Mode & 0xfff) != mode || value.Owner != geteuid()
                        || value.Links != 1 || value.Size is < 1 or > 64UL * 1024 * 1024)
                        throw new InvalidOperationException("learning_runtime_file");
                    total += value.Size;
                    if (total > 128UL * 1024 * 1024) throw new InvalidOperationException("learning_runtime_total_size");
                }
                var buffer = new byte[64 * 1024];
                for (var index = 0; index < Names.Length; index++)
                {
                    using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                    long position = 0;
                    var length = (long)metadata[index].Size;
                    while (position < length)
                    {
                        var count = RandomAccess.Read(handles[index], buffer.AsSpan(0, (int)Math.Min(buffer.Length, length - position)), position);
                        if (count == 0) throw new InvalidOperationException("learning_runtime_file_changed");
                        digest.AppendData(buffer, 0, count);
                        position += count;
                    }
                    if (RandomAccess.Read(handles[index], buffer.AsSpan(0, 1), position) != 0 || !metadata[index].Equals(Inspect(handles[index])))
                        throw new InvalidOperationException("learning_runtime_file_changed");
                    if (Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant() != hashes[Names[index]])
                        throw new InvalidOperationException("learning_runtime_hash_mismatch");
                }
                // Earlier files must still be unchanged after later files are
                // hashed, and each fixed name must still identify the hashed FD.
                for (var index = 0; index < Names.Length; index++)
                    if (!metadata[index].Equals(Inspect(handles[index])) ||
                        InspectAt(root, Names[index], false) is not Stat named || !metadata[index].Equals(named))
                        throw new InvalidOperationException("learning_runtime_file_changed");
                if (!rootBefore.Equals(Inspect(root))) throw new InvalidOperationException("learning_runtime_root_changed");
                files.AssertPathIdentity();
            }
            finally { foreach (var handle in handles) handle.Dispose(); }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or CryptographicException)
        {
            throw new InvalidOperationException("learning_runtime_io_refused");
        }
    }

    public string PathFor(LearningNativeCommand command)
    {
        var name = command switch
        {
            LearningNativeCommand.BuildTable => Names[0], LearningNativeCommand.VerifyTable => Names[1],
            LearningNativeCommand.Snapshot => Names[2], LearningNativeCommand.Control => Names[3],
            LearningNativeCommand.Daemon => Names[4], _ => throw new ArgumentException("learning_runtime_command")
        };
        Verify();
        return Path.Combine(files.FullPath, name);
    }
    public string LibraryPath { get { Verify(); return Path.Combine(files.FullPath, Names[5]); } }
    public void Dispose() { if (!disposed) { disposed = true; files.Dispose(); } }
}
