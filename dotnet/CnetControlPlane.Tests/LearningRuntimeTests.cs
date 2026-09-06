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
public sealed class LearningRuntimeTests : IDisposable
{
    private static readonly string[] Names = ["cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so"];
    private const UnixFileMode PrivateDirectory = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private const UnixFileMode Executable = UnixFileMode.UserRead | UnixFileMode.UserExecute;
    private readonly string root = Directory.CreateTempSubdirectory("cnet-runtime-").FullName;
    private readonly Dictionary<string, string> hashes = new(StringComparer.Ordinal);
    [DllImport("libc", SetLastError = true)] private static extern int link(string oldpath, string newpath);
    [DllImport("libc", SetLastError = true)] private static extern int mkfifo(string path, uint mode);

    public LearningRuntimeTests()
    {
        File.SetUnixFileMode(root, PrivateDirectory);
        // Hash-attestation fixture bytes are never executed or installed.
        foreach (var name in Names) Put(name, Encoding.ASCII.GetBytes("fixed-test-artifact:" + name));
    }
    private void Put(string name, byte[] bytes)
    {
        var path = Path.Combine(root, name);
        if (File.Exists(path)) File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        File.WriteAllBytes(path, bytes);
        File.SetUnixFileMode(path, name.EndsWith(".so", StringComparison.Ordinal) ? UnixFileMode.UserRead : Executable);
        hashes[name] = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }
    private byte[] Manifest() => JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, files = hashes });
    public void Dispose() => Directory.Delete(root, recursive: true);

    [Fact]
    public void FixedCommandsAndLibraryRequireAnIntactWholeRuntime()
    {
        var manifest = Manifest();
        using var runtime = LearningRuntime.Load(root, manifest);
        Assert.Equal(Convert.ToHexString(SHA256.HashData(manifest)).ToLowerInvariant(), runtime.Sha256);
        var commands = Enum.GetValues<LearningNativeCommand>();
        Assert.Equal(5, commands.Length);
        for (var index = 0; index < commands.Length; index++)
            Assert.Equal(Path.Combine(root, Names[index]), runtime.PathFor(commands[index]));
        Assert.Equal(Path.Combine(root, Names[5]), runtime.LibraryPath);
        runtime.Verify();
    }

    [Fact]
    public void ChangedLibraryRefusesBeforeReturningAnyExecutablePath()
    {
        using var runtime = LearningRuntime.Load(root, Manifest());
        Put(Names[5], [1, 2, 3]);
        Assert.Throws<InvalidOperationException>(() => runtime.PathFor(LearningNativeCommand.BuildTable));
    }

    [Fact]
    public void ManifestBytesArePrivatelyCloned()
    {
        var manifest = Manifest();
        var expected = Convert.ToHexString(SHA256.HashData(manifest)).ToLowerInvariant();
        using var runtime = LearningRuntime.Load(root, manifest);
        Array.Fill(manifest, (byte)0);
        Assert.Equal(expected, runtime.Sha256);
        runtime.Verify();
    }

    [Fact]
    public void ExtraInstalledEntryRefuses()
    {
        var manifest = Manifest();
        File.WriteAllBytes(Path.Combine(root, "unexpected"), [1]);
        Assert.Throws<InvalidOperationException>(() => LearningRuntime.Load(root, manifest));
    }

    [Theory]
    [InlineData("\"schema_version\":1", "\"schema_version\":2")]
    [InlineData("\"schema_version\":1", "\"schema_version\":1.0")]
    [InlineData("\"schema_version\":1", "\"schema_version\":1e0")]
    [InlineData("\"schema_version\":1", "\"schema_version\":\"1\"")]
    [InlineData("\"schema_version\":1", "\"schema_version\":1,\"schema_version\":1")]
    [InlineData("\"schema_version\":1", "\"schema_version\":1,\"unknown\":0")]
    [InlineData("\"schema_version\":1,", "")]
    [InlineData("cnetd", "arbitrary")]
    [InlineData("cnetd", "../cnetd")]
    public void UnknownMissingDuplicateAndNoncanonicalManifestFieldsRefuse(string before, string after)
    {
        var malformed = Encoding.UTF8.GetString(Manifest()).Replace(before, after);
        Assert.Throws<ArgumentException>(() => LearningRuntime.Load(root, Encoding.UTF8.GetBytes(malformed)));
    }

    [Fact]
    public void AllAndOnlySixFileHashesAreRequired()
    {
        var manifest = Encoding.UTF8.GetString(Manifest());
        var pair = "\"cnetd\":\"" + hashes["cnetd"] + "\"";
        foreach (var malformed in new[]
        {
            manifest.Replace(pair, pair + "," + pair),
            manifest.Replace(pair + ",", ""),
            manifest.Replace(pair, pair + ",\"seventh\":\"" + hashes["cnetd"] + "\""),
            manifest.Replace(pair, "\"cnetd\":null"),
            manifest.Replace(pair, "\"cnetd\":1"),
            manifest.Replace(hashes["cnetd"], hashes["cnetd"].ToUpperInvariant()),
            manifest.Replace(hashes["cnetd"], hashes["cnetd"][..63]),
            manifest.Replace(hashes["cnetd"], new string('x', 64)),
            manifest + "{}", manifest.Replace("\"files\":{", "\"files\":[")
        }) Assert.Throws<ArgumentException>(() => LearningRuntime.Load(root, Encoding.UTF8.GetBytes(malformed)));
    }

    [Fact]
    public void ManifestBoundsRefuseBeforeDecoding()
    {
        foreach (var manifest in new byte[][] { [], new byte[4097], null! })
            Assert.Equal("learning_runtime_manifest_size", Assert.Throws<ArgumentException>(() => LearningRuntime.Load(root, manifest)).Message);
        Assert.Throws<ArgumentException>(() => LearningRuntime.Load(root, [255, 0, 1]));
    }

    [Theory]
    [InlineData("cnetd", 384)] // 0600, writable and not executable
    [InlineData("cnetd", 448)] // 0700, writable executable
    [InlineData("cnetd", 365)] // 0555, shared executable
    [InlineData("cnetd", 2368)] // 04500, set-ID executable
    [InlineData("libcnet_capsule_core.so", 384)] // 0600, writable library
    [InlineData("libcnet_capsule_core.so", 320)] // 0500, executable library
    public void ExactInstalledModesAreRequired(string name, int mode)
    {
        File.SetUnixFileMode(Path.Combine(root, name), (UnixFileMode)mode);
        Assert.Equal("learning_runtime_file", Assert.Throws<InvalidOperationException>(() => LearningRuntime.Load(root, Manifest())).Message);
    }

    [Theory]
    [InlineData("symlink")]
    [InlineData("hardlink")]
    [InlineData("fifo")]
    [InlineData("directory")]
    public void FixedNamesCannotBeLinksOrNonregularFiles(string kind)
    {
        var path = Path.Combine(root, "cnetd");
        File.Delete(path);
        switch (kind)
        {
            case "symlink": File.CreateSymbolicLink(path, "cnet_table_capsule"); break;
            case "hardlink": Assert.Equal(0, link(Path.Combine(root, "cnet_table_capsule"), path)); break;
            case "fifo": Assert.Equal(0, mkfifo(path, 320)); break;
            case "directory": Directory.CreateDirectory(path, PrivateDirectory); break;
        }
        Assert.Throws<InvalidOperationException>(() => LearningRuntime.Load(root, Manifest()));
    }

    [Fact]
    public void MissingFilesUnknownCommandsAndDisposedInstancesRefuse()
    {
        using var runtime = LearningRuntime.Load(root, Manifest());
        Assert.Throws<ArgumentException>(() => runtime.PathFor((LearningNativeCommand)999));
        File.Delete(Path.Combine(root, "cnetd"));
        Assert.Throws<InvalidOperationException>(runtime.Verify);
        Assert.Throws<InvalidOperationException>(() => runtime.LibraryPath);
        runtime.Dispose();
        Assert.Equal("learning_runtime_disposed", Assert.Throws<InvalidOperationException>(runtime.Verify).Message);
    }

    [Fact]
    public void RootIdentityAndOwnerPrivateModeAreRechecked()
    {
        using var runtime = LearningRuntime.Load(root, Manifest());
        File.SetUnixFileMode(root, PrivateDirectory | UnixFileMode.GroupRead);
        Assert.Throws<InvalidOperationException>(runtime.Verify);
        File.SetUnixFileMode(root, PrivateDirectory);
        var moved = root + "-moved";
        try
        {
            Directory.Move(root, moved);
            Directory.CreateDirectory(root, PrivateDirectory);
            Assert.Throws<InvalidOperationException>(runtime.Verify);
            Assert.Empty(Directory.EnumerateFileSystemEntries(root));
        }
        finally { Directory.Delete(moved, recursive: true); }
    }

    private void Sparse(string name, long size, bool updateHash)
    {
        var path = Path.Combine(root, name);
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        using (var stream = new FileStream(path, FileMode.Create, FileAccess.Write)) stream.SetLength(size);
        File.SetUnixFileMode(path, name.EndsWith(".so", StringComparison.Ordinal) ? UnixFileMode.UserRead : Executable);
        if (!updateHash) return;
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var zeros = new byte[64 * 1024];
        for (long count = 0; count < size; count += zeros.Length)
            digest.AppendData(zeros, 0, (int)Math.Min(zeros.Length, size - count));
        hashes[name] = Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant();
    }

    [Theory]
    [InlineData(0)]
    [InlineData(67108865)]
    public void PerFileSizeBoundsPrecedeHashing(long size)
    {
        Sparse("cnetd", size, false);
        Assert.Equal("learning_runtime_file", Assert.Throws<InvalidOperationException>(() => LearningRuntime.Load(root, Manifest())).Message);
    }

    [Fact]
    public void TotalSizeBoundPrecedesHashing()
    {
        foreach (var name in Names.Take(3)) Sparse(name, 43L * 1024 * 1024, false);
        Assert.Equal("learning_runtime_total_size", Assert.Throws<InvalidOperationException>(() => LearningRuntime.Load(root, Manifest())).Message);
    }

    [Fact]
    public void Exact128MiBTotalUsesBoundedStreamingHashBuffers()
    {
        Sparse(Names[0], 64L * 1024 * 1024, true);
        Sparse(Names[1], 64L * 1024 * 1024 - 4, true);
        foreach (var name in Names.Skip(2)) Put(name, [0]);
        var manifest = Manifest();
        var before = GC.GetAllocatedBytesForCurrentThread();
        using var runtime = LearningRuntime.Load(root, manifest);
        Assert.True(GC.GetAllocatedBytesForCurrentThread() - before < 2 * 1024 * 1024,
            "LEARNING_RUNTIME_RED whole artifact bytes were allocated instead of streamed");
    }

    [Fact]
    public void FailedLoadsAndDisposeDoNotLeakDescriptors()
    {
        var manifest = Manifest();
        using (LearningRuntime.Load(root, manifest)) { }
        Put(Names[5], [9]);
        var before = Directory.GetFiles("/proc/self/fd").Length;
        for (var index = 0; index < 12; index++) Assert.Throws<InvalidOperationException>(() => LearningRuntime.Load(root, manifest));
        Assert.Equal(before, Directory.GetFiles("/proc/self/fd").Length);
    }

    [Fact]
    public async Task ConcurrentWritesOfIdenticalBytesStillRefuseMetadataMutation()
    {
        Sparse(Names[0], 64L * 1024 * 1024, true);
        var manifest = Manifest();
        var path = Path.Combine(root, Names[0]);
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        using var writer = File.OpenHandle(path, FileMode.Open, FileAccess.Write);
        File.SetUnixFileMode(path, Executable);
        using var stop = new CancellationTokenSource();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var mutation = Task.Run(() =>
        {
            byte[] sameBytes = [0];
            do
            {
                RandomAccess.Write(writer, sameBytes, 0);
                started.TrySetResult();
                Thread.Yield();
            } while (!stop.IsCancellationRequested);
        });
        try
        {
            await started.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.Equal("learning_runtime_file_changed", Assert.Throws<InvalidOperationException>(
                () => LearningRuntime.Load(root, manifest)).Message);
        }
        finally { stop.Cancel(); await mutation; }
    }
}
