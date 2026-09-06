using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
[Collection(LearningResourceCollection.Name)]
public sealed class LearningManagedRuntimeTests : IDisposable
{
    private static readonly string[] Names = ["cnet-control.dll", "cnet-control.deps.json", "cnet-control.runtimeconfig.json",
        "Microsoft.Data.Sqlite.dll", "SQLitePCLRaw.core.dll", "SQLitePCLRaw.batteries_v2.dll",
        "SQLitePCLRaw.provider.e_sqlite3.dll", "runtimes/linux-x64/native/libe_sqlite3.so"];
    private static readonly string[] Directories = ["runtimes", "runtimes/linux-x64", "runtimes/linux-x64/native"];
    private const UnixFileMode Private = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly string root = Directory.CreateTempSubdirectory("cnet-managed-runtime-").FullName;
    private readonly Dictionary<string, string> hashes = new(StringComparer.Ordinal);
    [DllImport("libc", SetLastError = true)] private static extern int link(string oldpath, string newpath);
    [DllImport("libc", SetLastError = true)] private static extern int mkfifo(string path, uint mode);

    public LearningManagedRuntimeTests()
    {
        File.SetUnixFileMode(root, Private);
        foreach (var directory in Directories) Directory.CreateDirectory(Path.Combine(root, directory), Private);
        // Unit attestation bytes are data only, never loaded as code.
        foreach (var name in Names) Put(name, Encoding.ASCII.GetBytes("fixed-managed-fixture:" + name));
    }
    private void Put(string name, byte[] bytes)
    {
        var path = Path.Combine(root, name);
        if (File.Exists(path)) File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        File.WriteAllBytes(path, bytes); File.SetUnixFileMode(path, UnixFileMode.UserRead);
        hashes[name] = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }
    private byte[] Manifest() => JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, target = "linux-x64", files = hashes });
    public void Dispose() => Directory.Delete(root, true);

    [Fact]
    public void ExactPrivateEightFileInstallationCanBeVerifiedRepeatedly()
    {
        var manifest = Manifest();
        using var runtime = LearningManagedRuntime.Load(root, manifest);
        Assert.Equal(Convert.ToHexString(SHA256.HashData(manifest)).ToLowerInvariant(), runtime.Sha256);
        runtime.Verify(); runtime.Verify();
    }
    [Fact]
    public void ManifestInputMutationCannotChangeRetainedIdentity()
    {
        var manifest = Manifest(); var hash = Convert.ToHexString(SHA256.HashData(manifest)).ToLowerInvariant();
        using var runtime = LearningManagedRuntime.Load(root, manifest);
        Array.Fill(manifest, (byte)0);
        Assert.Equal(hash, runtime.Sha256); runtime.Verify();
    }
    [Fact]
    public void ChangedNestedPackagedSqliteInvalidatesWholeInstallation()
    {
        using var runtime = LearningManagedRuntime.Load(root, Manifest());
        Put(Names[^1], [1, 2, 3]);
        Assert.Throws<InvalidOperationException>(runtime.Verify);
    }
    [Fact]
    public void RunningBindingCannotAttestAnUnrelatedCopyFromTheTestHost()
    {
        using var runtime = LearningManagedRuntime.Load(root, Manifest());
        Assert.Equal("learning_managed_running_refused", Assert.Throws<InvalidOperationException>(() => runtime.BindRunning()).Message);
    }

    [Theory]
    [InlineData("version_string")]
    [InlineData("version_fraction")]
    [InlineData("version_exponent")]
    [InlineData("version_unknown")]
    [InlineData("target_missing")]
    [InlineData("target_wrong")]
    [InlineData("target_duplicate")]
    [InlineData("target_escaped_duplicate")]
    [InlineData("top_unknown")]
    [InlineData("files_duplicate")]
    [InlineData("file_duplicate")]
    [InlineData("file_unknown")]
    [InlineData("file_missing")]
    [InlineData("hash_uppercase")]
    [InlineData("hash_short")]
    [InlineData("hash_not_string")]
    [InlineData("hash_nonhex")]
    [InlineData("trailing_value")]
    [InlineData("trailing_comma")]
    [InlineData("comment")]
    [InlineData("depth")]
    [InlineData("nul")]
    [InlineData("invalid_utf8")]
    public void ManifestRejectsNoncontractFieldsBeforeFilesystemAccess(string mutation)
    {
        var json = Encoding.UTF8.GetString(Manifest());
        var hash = hashes[Names[0]];
        json = mutation switch
        {
            "version_string" => json.Replace("\"schema_version\":1", "\"schema_version\":\"1\""),
            "version_fraction" => json.Replace("\"schema_version\":1", "\"schema_version\":1.0"),
            "version_exponent" => json.Replace("\"schema_version\":1", "\"schema_version\":1e0"),
            "version_unknown" => json.Replace("\"schema_version\":1", "\"schema_version\":2"),
            "target_missing" => json.Replace("\"target\":\"linux-x64\",", ""),
            "target_wrong" => json.Replace("linux-x64\"", "linux-arm64\""),
            "target_duplicate" => json.Replace("\"target\":", "\"target\":\"linux-x64\",\"target\":"),
            "target_escaped_duplicate" => json.Replace("\"target\":", "\"ta\\u0072get\":\"linux-x64\",\"target\":"),
            "top_unknown" => json.Insert(1, "\"other\":0,"),
            "files_duplicate" => json.Insert(1, "\"files\":{},"),
            "file_duplicate" => json.Replace("\"files\":{", "\"files\":{\"cnet-control.dll\":\"" + hash + "\","),
            "file_unknown" => json.Replace("cnet-control.dll", "cnet-control.pdb"),
            "file_missing" => json.Replace("\"cnet-control.dll\":\"" + hash + "\",", ""),
            "hash_uppercase" => json.Replace(hash, hash.ToUpperInvariant()),
            "hash_short" => json.Replace(hash, hash[..63]),
            "hash_not_string" => json.Replace("\"" + hash + "\"", "1"),
            "hash_nonhex" => json.Replace(hash, new string('g', 64)),
            "trailing_value" => json + "{}",
            "trailing_comma" => json.Insert(json.Length - 1, ","),
            "comment" => json.Insert(1, "/*not allowed*/"),
            "depth" => json.Replace("\"" + hash + "\"", "{\"nested\":{\"more\":{}}}"),
            "nul" => json + "\0",
            "invalid_utf8" => json,
            _ => throw new InvalidOperationException()
        };
        var bytes = Encoding.UTF8.GetBytes(json);
        if (mutation == "invalid_utf8") bytes[Array.IndexOf(bytes, (byte)'l')] = 0xff;
        Assert.Throws<ArgumentException>(() => LearningManagedRuntime.Load("/missing-private-installation", bytes));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(4097)]
    public void ManifestIsBoundedBeforeParsing(int count)
        => Assert.Throws<ArgumentException>(() => LearningManagedRuntime.Load(root, new byte[count]));

    [Fact]
    public void ExactManifestByteLimitIsAccepted()
    {
        var bytes = Manifest();
        Array.Resize(ref bytes, 4096);
        Array.Fill(bytes, (byte)' ', Manifest().Length, 4096 - Manifest().Length);
        using var runtime = LearningManagedRuntime.Load(root, bytes);
        runtime.Verify();
    }

    [Theory]
    [InlineData("cnet-control.dll", 384)] // 0600
    [InlineData("cnet-control.dll", 320)] // 0500
    [InlineData("cnet-control.dll", 288)] // 0440
    [InlineData("cnet-control.dll", 2304)] // 04400
    [InlineData("runtimes/linux-x64/native/libe_sqlite3.so", 384)]
    public void EveryFileRequiresExact0400(string name, int mode)
    {
        File.SetUnixFileMode(Path.Combine(root, name), (UnixFileMode)mode);
        Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest()));
    }

    [Theory]
    [InlineData("")]
    [InlineData("runtimes")]
    [InlineData("runtimes/linux-x64")]
    [InlineData("runtimes/linux-x64/native")]
    public void EveryDirectoryRequiresExact0700(string name)
    {
        var path = Path.Combine(root, name);
        File.SetUnixFileMode(path, Private | UnixFileMode.GroupExecute);
        Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest()));
    }

    [Theory]
    [InlineData("cnet-control")]
    [InlineData("cnet-control.pdb")]
    [InlineData("owner-manifest.json")]
    [InlineData("runtimes/linux-arm64")]
    [InlineData("runtimes/linux-x64/extra")]
    [InlineData("runtimes/linux-x64/native/extra")]
    public void ExtraEntryAtAnyLevelIsRefused(string name)
    {
        File.WriteAllText(Path.Combine(root, name), "extra");
        Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest()));
    }

    [Theory]
    [InlineData("cnet-control.dll")]
    [InlineData("runtimes/linux-x64/native/libe_sqlite3.so")]
    public void MissingRequiredFileIsRefused(string name)
    {
        File.Delete(Path.Combine(root, name));
        Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest()));
    }

    [Theory]
    [InlineData("symlink")]
    [InlineData("hardlink")]
    [InlineData("fifo")]
    [InlineData("directory")]
    public void NonregularOrAliasedFileIsRefusedWithoutBlocking(string kind)
    {
        var path = Path.Combine(root, Names[^1]);
        File.Delete(path);
        switch (kind)
        {
            case "symlink": File.CreateSymbolicLink(path, Path.Combine(root, Names[0])); break;
            case "hardlink": Assert.Equal(0, link(Path.Combine(root, Names[0]), path)); break;
            case "fifo": Assert.Equal(0, mkfifo(path, 0x100)); break;
            case "directory": Directory.CreateDirectory(path, Private); break;
        }
        Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest()));
    }

    [Theory]
    [InlineData("runtimes")]
    [InlineData("runtimes/linux-x64")]
    [InlineData("runtimes/linux-x64/native")]
    public void SymlinkedNestedDirectoryCannotSubstituteForPrivateDirectory(string name)
    {
        var path = Path.Combine(root, name); var backup = path + "-saved";
        Directory.Move(path, backup);
        Directory.CreateSymbolicLink(path, backup);
        try { Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest())); }
        finally { Directory.Delete(path); Directory.Move(backup, path); }
    }

    [Theory]
    [InlineData("")]
    [InlineData("runtimes")]
    [InlineData("runtimes/linux-x64")]
    [InlineData("runtimes/linux-x64/native")]
    public void RetainedDirectoryCannotBeReplacedEvenWithAnIdenticalTree(string name)
    {
        using var runtime = LearningManagedRuntime.Load(root, Manifest());
        var path = name.Length == 0 ? root : Path.Combine(root, name); var backup = path + "-saved";
        Directory.Move(path, backup);
        Directory.CreateDirectory(path, Private);
        try
        {
            foreach (var child in Directory.EnumerateDirectories(backup, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(path, Path.GetRelativePath(backup, child)), Private);
            foreach (var file in Directory.EnumerateFiles(backup, "*", SearchOption.AllDirectories))
            {
                var copy = Path.Combine(path, Path.GetRelativePath(backup, file));
                File.Copy(file, copy); File.SetUnixFileMode(copy, UnixFileMode.UserRead);
            }
            Assert.Equal("learning_root_path_identity_changed", Assert.Throws<InvalidOperationException>(runtime.Verify).Message);
        }
        finally { Directory.Delete(path, true); Directory.Move(backup, path); }
    }

    private void Resize(string name, long length)
    {
        var path = Path.Combine(root, name);
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        using (var stream = File.OpenWrite(path)) stream.SetLength(length);
        File.SetUnixFileMode(path, UnixFileMode.UserRead);
    }
    [Theory]
    [InlineData(0)]
    [InlineData(67108865)]
    public void IndividualFileSizeMustBeOneThrough64MiB(long length)
    {
        Resize(Names[0], length);
        Assert.Equal("learning_managed_file", Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest())).Message);
    }
    [Fact]
    public void TotalAbove128MiBRefusesBeforeHashingOrAllocatingFileContents()
    {
        Resize(Names[0], 64L * 1024 * 1024); Resize(Names[1], 64L * 1024 * 1024);
        Assert.Equal("learning_managed_total_size", Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, Manifest())).Message);
    }
    [Fact]
    public void MaximumTotalStreamsWithBoundedManagedAllocation()
    {
        foreach (var name in Names) Resize(name, 1);
        Resize(Names[0], 64L * 1024 * 1024);
        Resize(Names[1], 64L * 1024 * 1024 - 6);
        foreach (var name in Names)
        {
            using var stream = File.OpenRead(Path.Combine(root, name));
            hashes[name] = Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
        }
        using var runtime = LearningManagedRuntime.Load(root, Manifest());
        var before = GC.GetAllocatedBytesForCurrentThread();
        runtime.Verify();
        Assert.InRange(GC.GetAllocatedBytesForCurrentThread() - before, 0, 2 * 1024 * 1024);
    }

    [Fact]
    public void FailedLoadsDoNotLeakRootOrNestedDescriptors()
    {
        var manifest = Manifest(); Put(Names[^1], [42]);
        for (var index = 0; index < 4; index++) Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, manifest));
        // Finalizers must not hide a missing deterministic Dispose in these
        // bounded failed loads. No collection is forced after the operation.
        Assert.True(GC.TryStartNoGCRegion(16 * 1024 * 1024));
        try
        {
            Assert.Equal(0, CountFixtureDescriptors());
            for (var index = 0; index < 40; index++)
            {
                Assert.Throws<InvalidOperationException>(() => LearningManagedRuntime.Load(root, manifest));
                Assert.Equal(0, CountFixtureDescriptors());
            }
        }
        finally { GC.EndNoGCRegion(); }
    }
    // This stable, uniquely named fixture is not renamed during measurement.
    // Count its root, nested directories and files, not unrelated process FDs
    // or shared ancestors. A prefix sibling is deliberately outside this scope.
    private int CountFixtureDescriptors()
    {
        var count = 0;
        foreach (var descriptor in Directory.EnumerateFileSystemEntries("/proc/self/fd"))
        {
            var target = new FileInfo(descriptor).LinkTarget;
            if (target == root || target?.StartsWith(root + "/", StringComparison.Ordinal) == true) count++;
        }
        return count;
    }

    [Fact]
    public void FixtureDescriptorAccountingIgnoresClosingAnUnrelatedPrefixSibling()
    {
        var sibling = root + "-unrelated";
        Directory.CreateDirectory(sibling, Private);
        try
        {
            using var unrelated = LearningFiles.Open(sibling);
            var before = CountFixtureDescriptors();
            unrelated.Dispose();
            Assert.Equal(before, CountFixtureDescriptors());
        }
        finally { Directory.Delete(sibling); }
    }

    [Fact]
    public void FixtureDescriptorAccountingDetectsEachHeldDirectoryAndFile()
    {
        var before = CountFixtureDescriptors();
        var held = new List<IDisposable>();
        try
        {
            foreach (var name in new[] { "" }.Concat(Directories))
            {
                held.Add(LearningFiles.Open(Path.Combine(root, name)));
                Assert.Equal(before + held.Count, CountFixtureDescriptors());
            }
            held.Add(File.OpenHandle(Path.Combine(root, Names[^1])));
            Assert.Equal(before + held.Count, CountFixtureDescriptors());
        }
        finally { foreach (var handle in held) handle.Dispose(); }
        Assert.Equal(before, CountFixtureDescriptors());
    }
    [Fact]
    public void DisposeRefusesVerificationAndBinding()
    {
        var runtime = LearningManagedRuntime.Load(root, Manifest()); runtime.Dispose(); runtime.Dispose();
        Assert.Equal("learning_managed_disposed", Assert.Throws<InvalidOperationException>(runtime.Verify).Message);
        Assert.Equal("learning_managed_disposed", Assert.Throws<InvalidOperationException>(() => runtime.BindRunning()).Message);
    }
    [Fact]
    public void RunningProofHasOnlyPrivateConstructorsAndNoWritableIdentity()
    {
        var type = typeof(LearningRunningManagedRuntime);
        Assert.True(type.IsSealed);
        Assert.All(type.GetConstructors(System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public
            | System.Reflection.BindingFlags.NonPublic), constructor => Assert.True(constructor.IsPrivate));
        Assert.False(type.GetProperty(nameof(LearningRunningManagedRuntime.Sha256))!.CanWrite);
    }
}
