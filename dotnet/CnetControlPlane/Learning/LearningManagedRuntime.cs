using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;
using static CnetControlPlane.Learning.LearningLinux;

namespace CnetControlPlane.Learning;

/// <summary>
/// Fixed linux-x64 framework-dependent installation identity. The manifest
/// lives outside this eight-file tree. The owner must launch through a trusted
/// dotnet host with a clean environment, before any SQLite use. Neither prior
/// startup hooks nor system host/framework/loader/libraries are attested here.
/// Installation files must remain unchanged from process launch to shutdown:
/// loaded assembly locations are checked, not retroactive in-memory image hashes.
/// Calls are owner-serialized, not hostile-same-UID isolation.
/// </summary>
internal sealed class LearningManagedRuntime : IDisposable
{
    private static readonly string[] Names = ["cnet-control.dll", "cnet-control.deps.json", "cnet-control.runtimeconfig.json",
        "Microsoft.Data.Sqlite.dll", "SQLitePCLRaw.core.dll", "SQLitePCLRaw.batteries_v2.dll",
        "SQLitePCLRaw.provider.e_sqlite3.dll", "runtimes/linux-x64/native/libe_sqlite3.so"];
    private static readonly string[] Directories = ["", "runtimes", "runtimes/linux-x64", "runtimes/linux-x64/native"];
    private readonly LearningFiles[] directories;
    private readonly Dictionary<string, string> hashes;
    private bool disposed;
    public string Sha256 { get; }
    private string Root => directories[0].FullPath;
    private LearningManagedRuntime(LearningFiles[] directories, Dictionary<string, string> hashes, string sha256)
    { this.directories = directories; this.hashes = hashes; Sha256 = sha256; }

    public static LearningManagedRuntime Load(string root, byte[] manifest)
    {
        if (manifest is null || manifest.Length is < 1 or > 4096)
            throw new ArgumentException("learning_managed_manifest_size");
        var snapshot = (byte[])manifest.Clone();
        var hashes = ParseManifest(snapshot);
        var opened = new List<LearningFiles>(4);
        try
        {
            var digest = Convert.ToHexString(SHA256.HashData(snapshot)).ToLowerInvariant();
            opened.Add(LearningFiles.Open(root));
            foreach (var name in Directories.Skip(1)) opened.Add(LearningFiles.Open(Path.Combine(root, name)));
            var runtime = new LearningManagedRuntime(opened.ToArray(), hashes, digest);
            runtime.Verify();
            return runtime;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or CryptographicException)
        {
            foreach (var directory in opened) directory.Dispose();
            throw new InvalidOperationException("learning_managed_io_refused");
        }
        catch { foreach (var directory in opened) directory.Dispose(); throw; }
    }

    private static Dictionary<string, string> ParseManifest(byte[] bytes)
    {
        static Dictionary<string, JsonElement> Fields(JsonElement value, params string[] names)
        {
            if (value.ValueKind != JsonValueKind.Object) throw new ArgumentException("learning_managed_manifest_fields");
            var result = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
            foreach (var property in value.EnumerateObject())
                if (result.Count >= names.Length || !names.Contains(property.Name, StringComparer.Ordinal)
                    || !result.TryAdd(property.Name, property.Value))
                    throw new ArgumentException("learning_managed_manifest_fields");
            if (result.Count != names.Length) throw new ArgumentException("learning_managed_manifest_fields");
            return result;
        }
        try
        {
            using var document = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 3 });
            var top = Fields(document.RootElement, "schema_version", "target", "files");
            if (top["schema_version"].ValueKind != JsonValueKind.Number || top["schema_version"].GetRawText() != "1")
                throw new ArgumentException("learning_managed_manifest_version");
            if (top["target"].ValueKind != JsonValueKind.String || top["target"].GetString() != "linux-x64")
                throw new ArgumentException("learning_managed_manifest_target");
            var entries = Fields(top["files"], Names);
            var result = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var name in Names)
            {
                if (entries[name].ValueKind != JsonValueKind.String) throw new ArgumentException("learning_managed_manifest_hash");
                var hash = entries[name].GetString();
                if (hash is not { Length: 64 } || hash.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f')))
                    throw new ArgumentException("learning_managed_manifest_hash");
                result.Add(name, hash);
            }
            return result;
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException)
        { throw new ArgumentException("learning_managed_manifest_json"); }
    }

    private static string[] ExpectedEntries(int index) => index switch
    {
        0 => Names.Take(7).Append("runtimes").ToArray(),
        1 => ["linux-x64"], 2 => ["native"], 3 => ["libe_sqlite3.so"],
        _ => throw new InvalidOperationException("learning_managed_inventory")
    };
    private static void Inventory(SafeFileHandle directory, int index)
    {
        var expected = ExpectedEntries(index);
        var count = 0;
        var options = new EnumerationOptions { AttributesToSkip = 0, IgnoreInaccessible = false, RecurseSubdirectories = false };
        // At most nine root entries or two in each nested directory; never
        // recurse or follow an enumerated input name.
        foreach (var entry in Directory.EnumerateFileSystemEntries($"/proc/self/fd/{directory.DangerousGetHandle().ToInt64()}", "*", options))
            if (++count > expected.Length || !expected.Contains(Path.GetFileName(entry), StringComparer.Ordinal))
                throw new InvalidOperationException("learning_managed_inventory");
        if (count != expected.Length) throw new InvalidOperationException("learning_managed_inventory");
    }
    private SafeFileHandle[] OpenDirectories()
    {
        var opened = new List<SafeFileHandle>(4);
        try
        {
            foreach (var directory in directories) directory.AssertPathIdentity();
            opened.Add(Handle(open(Root, DirectoryFlag | NoFollow | CloseExec, 0)));
            foreach (var name in new[] { "runtimes", "linux-x64", "native" })
                opened.Add(Handle(openat(opened[^1], name, DirectoryFlag | NoFollow | CloseExec, 0)));
            foreach (var handle in opened)
            {
                var value = Inspect(handle);
                if ((value.Mode & 0xf000) != 0x4000 || (value.Mode & 0xfff) != 0x1c0 || value.Owner != geteuid())
                    throw new InvalidOperationException("learning_managed_directory");
            }
            return opened.ToArray();
        }
        catch { foreach (var handle in opened) handle.Dispose(); throw; }
    }
    private static void CheckFile(Stat value)
    {
        if ((value.Mode & 0xf000) != 0x8000 || (value.Mode & 0xfff) != 0x100 || value.Owner != geteuid()
            || value.Links != 1 || value.Size is < 1 or > 64UL * 1024 * 1024)
            throw new InvalidOperationException("learning_managed_file");
    }
    private void HashFile(SafeFileHandle handle, Stat before, int index, byte[] buffer)
    {
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long position = 0;
        while (position < (long)before.Size)
        {
            var count = RandomAccess.Read(handle, buffer.AsSpan(0, (int)Math.Min(buffer.Length, (long)before.Size - position)), position);
            if (count == 0) throw new InvalidOperationException("learning_managed_file_changed");
            digest.AppendData(buffer, 0, count); position += count;
        }
        if (RandomAccess.Read(handle, buffer.AsSpan(0, 1), position) != 0 || !before.Equals(Inspect(handle)))
            throw new InvalidOperationException("learning_managed_file_changed");
        if (Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant() != hashes[Names[index]])
            throw new InvalidOperationException("learning_managed_hash_mismatch");
    }

    public void Verify()
    {
        if (disposed) throw new InvalidOperationException("learning_managed_disposed");
        var opened = new List<SafeFileHandle>(8);
        SafeFileHandle[] dirs = [];
        try
        {
            dirs = OpenDirectories();
            var directoryMetadata = dirs.Select(Inspect).ToArray();
            for (var index = 0; index < dirs.Length; index++) Inventory(dirs[index], index);
            var metadata = new Stat[Names.Length];
            ulong total = 0;
            for (var index = 0; index < Names.Length; index++)
            {
                var handle = Handle(openat(dirs[index < 7 ? 0 : 3], Path.GetFileName(Names[index]), NoFollow | NonBlock | CloseExec, 0));
                opened.Add(handle);
                var value = metadata[index] = Inspect(handle);
                CheckFile(value);
                total += value.Size;
                if (total > 128UL * 1024 * 1024) throw new InvalidOperationException("learning_managed_total_size");
            }
            var buffer = new byte[64 * 1024];
            for (var index = 0; index < Names.Length; index++) HashFile(opened[index], metadata[index], index, buffer);
            for (var index = 0; index < Names.Length; index++)
                if (!metadata[index].Equals(Inspect(opened[index])) ||
                    InspectAt(dirs[index < 7 ? 0 : 3], Path.GetFileName(Names[index]), false) is not Stat named || !metadata[index].Equals(named))
                    throw new InvalidOperationException("learning_managed_file_changed");
            for (var index = 0; index < dirs.Length; index++)
                if (!directoryMetadata[index].Equals(Inspect(dirs[index]))) throw new InvalidOperationException("learning_managed_directory_changed");
            foreach (var directory in directories) directory.AssertPathIdentity();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or CryptographicException)
        { throw new InvalidOperationException("learning_managed_io_refused"); }
        finally
        {
            foreach (var handle in opened) handle.Dispose();
            foreach (var handle in dirs) handle.Dispose();
        }
    }

    public LearningRunningManagedRuntime BindRunning() => LearningRunningManagedRuntime.Bind(this);

    internal Assembly CheckRunningLocations()
    {
        var application = typeof(LearningManagedRuntime).Assembly;
        var assemblies = new[] { application, typeof(Microsoft.Data.Sqlite.SqliteConnection).Assembly, typeof(SQLitePCL.raw).Assembly,
            typeof(SQLitePCL.Batteries_V2).Assembly, typeof(SQLitePCL.SQLite3Provider_e_sqlite3).Assembly };
        var indices = new[] { 0, 3, 4, 5, 6 };
        if (RuntimeInformation.RuntimeIdentifier != "linux-x64" || Assembly.GetEntryAssembly() != application
            || AppContext.BaseDirectory != Root + Path.DirectorySeparatorChar)
            throw new InvalidOperationException("learning_managed_running_refused");
        for (var index = 0; index < assemblies.Length; index++)
            if (assemblies[index].Location != Path.Combine(Root, Names[indices[index]])
                || AssemblyLoadContext.GetLoadContext(assemblies[index]) != AssemblyLoadContext.Default || assemblies[index].IsCollectible)
                throw new InvalidOperationException("learning_managed_running_refused");
        return assemblies[^1];
    }

    internal nint LoadVerifiedSqlite()
    {
        SafeFileHandle[] dirs = [];
        nint loaded = 0;
        try
        {
            Verify();
            dirs = OpenDirectories();
            using var file = Handle(openat(dirs[3], "libe_sqlite3.so", NoFollow | NonBlock | CloseExec, 0));
            var before = Inspect(file); CheckFile(before);
            HashFile(file, before, 7, new byte[64 * 1024]);
            // Load the attested inode, not a freshly resolved owner pathname.
            // Its libc dependency remains explicitly trusted.
            loaded = NativeLibrary.Load($"/proc/self/fd/{file.DangerousGetHandle().ToInt64()}");
            if (!before.Equals(Inspect(file)) || InspectAt(dirs[3], "libe_sqlite3.so", false) is not Stat named || !before.Equals(named))
                throw new InvalidOperationException("learning_managed_file_changed");
            Verify();
            return loaded;
        }
        catch
        {
            if (loaded != 0) NativeLibrary.Free(loaded);
            throw new InvalidOperationException("learning_managed_sqlite_refused");
        }
        finally { foreach (var directory in dirs) directory.Dispose(); }
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        foreach (var directory in directories) directory.Dispose();
    }
}

/// <summary>
/// Non-forgeable proof of the running installation and its single bound SQLite
/// handle. Obtain before SQLite initialization in a clean-env owner launch.
/// The native handle intentionally survives for process lifetime; disposing
/// the installation invalidates Verify, but cannot invalidate P/Invokes.
/// </summary>
internal sealed class LearningRunningManagedRuntime
{
    private static readonly object Gate = new();
    private static nint sqliteHandle;
    private readonly LearningManagedRuntime installation;
    public string Sha256 => installation.Sha256;
    private LearningRunningManagedRuntime(LearningManagedRuntime installation) { this.installation = installation; }
    internal static LearningRunningManagedRuntime Bind(LearningManagedRuntime installation)
    {
        lock (Gate)
        {
            installation.Verify();
            var provider = installation.CheckRunningLocations();
            if (sqliteHandle != 0) throw new InvalidOperationException("learning_managed_sqlite_already_bound");
            var handle = installation.LoadVerifiedSqlite();
            try
            {
                NativeLibrary.SetDllImportResolver(provider, (name, _, _) => name == "e_sqlite3"
                    ? handle : throw new DllNotFoundException("learning_managed_sqlite_name_refused"));
            }
            catch
            {
                NativeLibrary.Free(handle);
                throw new InvalidOperationException("learning_managed_sqlite_resolver_refused");
            }
            sqliteHandle = handle;
            return new LearningRunningManagedRuntime(installation);
        }
    }
    public void Verify()
    {
        installation.Verify();
        _ = installation.CheckRunningLocations();
    }
}
