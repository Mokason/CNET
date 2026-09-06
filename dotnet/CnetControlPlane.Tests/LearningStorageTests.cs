using System.Diagnostics;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using CnetControlPlane.Learning;
using Microsoft.Data.Sqlite;
using Xunit;
using Xunit.Abstractions;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningStorageTests : IDisposable
{
    private const UnixFileMode PrivateDirectory = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private const UnixFileMode PrivateFile = UnixFileMode.UserRead | UnixFileMode.UserWrite;
    private readonly string root = Directory.CreateTempSubdirectory("cnet-storage-").FullName;
    private readonly ITestOutputHelper output;
    private int entryCapFixtureEntries;
    [DllImport("libc", SetLastError = true)] private static extern int link(string oldpath, string newpath);
    [DllImport("libc", SetLastError = true)] private static extern int mkfifo(string path, uint mode);

    public LearningStorageTests(ITestOutputHelper output)
    {
        this.output = output;
        File.SetUnixFileMode(root, PrivateDirectory);
    }
    public void Dispose()
    {
        // Some acceptance fixtures deliberately contain read-only directories.
        foreach (var path in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories))
            File.SetUnixFileMode(path, PrivateDirectory);
        Directory.Delete(root, recursive: true);
        if (entryCapFixtureEntries != 0)
            output.WriteLine($"Entry-cap fixture cleanup: entries created={entryCapFixtureEntries}; removed={!Directory.Exists(root)}; exact root={root}");
    }
    private static void Put(string path, long size, UnixFileMode mode = PrivateFile)
    {
        using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write)) stream.SetLength(size);
        File.SetUnixFileMode(path, mode);
    }
    private void Refuses(string reason, long maximum = 1024) => Assert.Equal(reason,
        Assert.Throws<InvalidOperationException>(() => LearningStorage.Measure(root, maximum)).Message);

    [Fact]
    public void EmptyRootCountsItselfAndUsagePropertiesCannotBeChanged()
    {
        var usage = LearningStorage.Measure(root, 1);
        Assert.Equal(0, usage.Bytes);
        Assert.Equal(0, usage.FileCount);
        Assert.Equal(1, usage.DirectoryCount);
        Assert.All(typeof(LearningStorageUsage).GetProperties(), property => Assert.Null(property.SetMethod));
    }

    [Fact]
    public void CountsLogicalBytesExactlyIncludingEmptyFilesAndReadOnlyDirectories()
    {
        var nested = Directory.CreateDirectory(Path.Combine(root, "nested"), PrivateDirectory).FullName;
        Put(Path.Combine(root, "owner.lock"), 0);
        Put(Path.Combine(root, "source"), 3, UnixFileMode.UserRead);
        Put(Path.Combine(nested, "executable"), 7, UnixFileMode.UserRead | UnixFileMode.UserExecute);
        File.SetUnixFileMode(nested, UnixFileMode.UserRead | UnixFileMode.UserExecute);
        var usage = LearningStorage.Measure(root, 10);
        Assert.Equal(10, usage.Bytes);
        Assert.Equal(3, usage.FileCount);
        Assert.Equal(2, usage.DirectoryCount);
        Refuses("learning_storage_byte_limit", 9);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    [InlineData(4294967297L)]
    [InlineData(long.MaxValue)]
    public void InvalidByteLimitRefusesBeforeOpeningAnyPath(long maximum) => Assert.Equal(
        "learning_storage_byte_limit_argument", Assert.Throws<ArgumentException>(() => LearningStorage.Measure("/missing", maximum)).Message);

    [Fact]
    public void SparseFourGiBBoundaryCountsLogicalRatherThanAllocatedBlocks()
    {
        Put(Path.Combine(root, "sparse"), 4L * 1024 * 1024 * 1024);
        var before = GC.GetAllocatedBytesForCurrentThread();
        Assert.Equal(4L * 1024 * 1024 * 1024, LearningStorage.Measure(root, 4L * 1024 * 1024 * 1024).Bytes);
        Assert.True(GC.GetAllocatedBytesForCurrentThread() - before < 1024 * 1024,
            "LEARNING_STORAGE_RED file content was allocated during metadata accounting");
        Refuses("learning_storage_byte_limit", 4L * 1024 * 1024 * 1024 - 1);
    }

    [Theory]
    [InlineData("symlink")]
    [InlineData("dangling")]
    [InlineData("hardlink")]
    [InlineData("fifo")]
    public void LinksAndNonregularFilesRefuse(string kind)
    {
        var path = Path.Combine(root, "invalid");
        switch (kind)
        {
            case "symlink": File.CreateSymbolicLink(path, root); break;
            case "dangling": File.CreateSymbolicLink(path, "missing"); break;
            case "hardlink":
                Put(Path.Combine(root, "source"), 1);
                Assert.Equal(0, link(Path.Combine(root, "source"), path)); break;
            case "fifo": Assert.Equal(0, mkfifo(path, 384)); break;
        }
        Refuses("learning_storage_entry");
    }

    [Fact]
    public void SocketMustRemainOutsideMeasuredRoot()
    {
        using var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        socket.Bind(new UnixDomainSocketEndPoint(Path.Combine(root, "daemon.sock")));
        Refuses("learning_storage_entry");
    }

    [Theory]
    [InlineData(false, 448)] // 0700 executable must not remain writable
    [InlineData(false, 420)] // 0644 shared file
    [InlineData(false, 2304)] // 04400 set-ID file
    [InlineData(false, 128)] // 0200 write-only file
    [InlineData(true, 493)] // 0755 shared directory
    [InlineData(true, 384)] // 0600 non-searchable directory
    [InlineData(true, 1472)] // 02700 set-GID directory
    public void UnsafeEntryModesRefuse(bool directory, int mode)
    {
        var path = Path.Combine(root, "entry");
        if (directory) Directory.CreateDirectory(path, PrivateDirectory);
        else Put(path, 0);
        File.SetUnixFileMode(path, (UnixFileMode)mode);
        try { Refuses("learning_storage_entry"); }
        finally { File.SetUnixFileMode(path, directory ? PrivateDirectory : PrivateFile); }
    }

    [Theory]
    [InlineData("space in_name")]
    [InlineData("bad\nname")]
    [InlineData("nonascii_é")]
    public void EveryEnumeratedComponentUsesTheExistingFixedNameValidator(string name)
    {
        Put(Path.Combine(root, name), 0);
        Assert.Equal("learning_file_component_required",
            Assert.Throws<ArgumentException>(() => LearningStorage.Measure(root, 1)).Message);
    }

    [Fact]
    public void ComponentLengthNinetySixIsAllowedButNinetySevenRefuses()
    {
        Put(Path.Combine(root, new string('a', 96)), 0);
        Assert.Equal(1, LearningStorage.Measure(root, 1).FileCount);
        Put(Path.Combine(root, new string('a', 97)), 0);
        Assert.Equal("learning_file_component_required",
            Assert.Throws<ArgumentException>(() => LearningStorage.Measure(root, 1)).Message);
    }

    [Fact]
    public void SharedRootOrRootSymlinkRefuses()
    {
        File.SetUnixFileMode(root, PrivateDirectory | UnixFileMode.GroupRead);
        try { Refuses("learning_private_directory_required"); }
        finally { File.SetUnixFileMode(root, PrivateDirectory); }
        var child = Directory.CreateDirectory(Path.Combine(root, "child"), PrivateDirectory).FullName;
        var alias = Path.Combine(root, "alias");
        Directory.CreateSymbolicLink(alias, child);
        Assert.Throws<InvalidOperationException>(() => LearningStorage.Measure(alias, 1));
    }

    [Fact]
    public void DepthEightIsAllowedButDepthNineRefuses()
    {
        var path = root;
        for (var depth = 0; depth < 8; depth++) path = Directory.CreateDirectory(Path.Combine(path, "child"), PrivateDirectory).FullName;
        Assert.Equal(9, LearningStorage.Measure(root, 1).DirectoryCount);
        Directory.CreateDirectory(Path.Combine(path, "too_deep"), PrivateDirectory);
        Refuses("learning_storage_depth_limit");
    }

    [Fact]
    public void ActualEntryCapIncludesDirectoriesAndRefusesTheNextEntry()
    {
        var drive = new DriveInfo(Path.GetPathRoot(root)!);
        Assert.True(drive.AvailableFreeSpace >= 2L * 1024 * 1024 * 1024, "Entry-cap fixture requires at least 2GiB free before creation");
        var clock = new LearningClock();
        var start = clock.Now;
        for (var group = 0; group < 256; group++)
        {
            Assert.True(drive.AvailableFreeSpace >= 1024L * 1024 * 1024, "Entry-cap fixture stopped because free space fell below 1GiB");
            var directory = Directory.CreateDirectory(Path.Combine(root, "group_" + group), PrivateDirectory).FullName;
            entryCapFixtureEntries++;
            for (var index = 0; index < 1023; index++)
            {
                if (index % 128 == 0)
                {
                    var now = clock.Now;
                    Assert.True(now.Boot == start.Boot && now.Nanoseconds - start.Nanoseconds < 60_000_000_000L,
                        "Entry-cap fixture creation exceeded its 60-second bound");
                }
                Put(Path.Combine(directory, "entry_" + index), 0);
                entryCapFixtureEntries++;
            }
        }
        var elapsed = clock.Now.Nanoseconds - start.Nanoseconds;
        Assert.True(elapsed < 60_000_000_000L, "Entry-cap fixture creation exceeded its 60-second bound");
        var usage = LearningStorage.Measure(root, 1);
        Assert.Equal(262144, entryCapFixtureEntries);
        Assert.Equal(261888, usage.FileCount);
        Assert.Equal(257, usage.DirectoryCount); // Includes root.
        Assert.Equal(0, usage.Bytes); // Logical content only, not physical filesystem usage.
        output.WriteLine($"Exact boundary: files={usage.FileCount}; directories including root={usage.DirectoryCount}; entries excluding root={entryCapFixtureEntries}; creation nanoseconds={elapsed}");
        Put(Path.Combine(root, "one_extra"), 0);
        entryCapFixtureEntries++;
        Refuses("learning_storage_entry_limit", 1);
        output.WriteLine($"Over-boundary refusal: actual entries excluding root={entryCapFixtureEntries}");
    }

    [Fact]
    public void FailureInNestedTreeReleasesEveryDirectoryDescriptor()
    {
        var child = Directory.CreateDirectory(Path.Combine(root, "child"), PrivateDirectory).FullName;
        Put(Path.Combine(child, "large"), 2);
        Refuses("learning_storage_byte_limit", 1); // Warm lazy runtime state.
        var before = Directory.GetFiles("/proc/self/fd").Length;
        for (var attempt = 0; attempt < 12; attempt++) Refuses("learning_storage_byte_limit", 1);
        Assert.Equal(before, Directory.GetFiles("/proc/self/fd").Length);
        Assert.Equal(2, LearningStorage.Measure(root, 2).Bytes);
    }

    [Fact]
    public async Task ObservedConcurrentMetadataMutationRefuses()
    {
        // Supplemental real-filesystem stress, not a transaction guarantee.
        // A directory-wide scan separates the two metadata observations.
        var changing = Path.Combine(root, "changing");
        Put(changing, 1);
        for (var index = 0; index < 4096; index++) Put(Path.Combine(root, "entry_" + index), 0);
        using var writer = File.OpenHandle(changing, FileMode.Open, FileAccess.Write);
        using var stop = new CancellationTokenSource();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var mutation = Task.Run(() =>
        {
            byte[] sameByte = [0];
            do
            {
                RandomAccess.Write(writer, sameByte, 0);
                started.TrySetResult();
                Thread.Yield();
            } while (!stop.IsCancellationRequested);
        });
        try
        {
            await started.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Refuses("learning_storage_changed", 1);
        }
        finally { stop.Cancel(); await mutation; }
    }

    [Fact]
    public void MetadataMeasurementDoesNotDropAnExistingSqlitePosixLock()
    {
        var path = Path.Combine(root, "ledger.sqlite");
        using var connection = new SqliteConnection(new SqliteConnectionStringBuilder
            { DataSource = path, Pooling = false }.ToString());
        connection.Open();
        File.SetUnixFileMode(path, PrivateFile);
        using (var schema = connection.CreateCommand())
        {
            schema.CommandText = "CREATE TABLE value(n INTEGER); INSERT INTO value VALUES(1);";
            schema.ExecuteNonQuery();
        }
        using var held = connection.BeginTransaction();
        AssertCompetingProcessLocked(path);
        var usage = LearningStorage.Measure(root, 1024 * 1024);
        Assert.True(usage.Bytes > 0);
        AssertCompetingProcessLocked(path);
    }

    private static void AssertCompetingProcessLocked(string path)
    {
        var start = new ProcessStartInfo("/usr/bin/sqlite3")
        { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        start.Environment.Clear();
        start.ArgumentList.Add(path);
        start.ArgumentList.Add("PRAGMA busy_timeout=0; BEGIN IMMEDIATE; ROLLBACK;");
        using var competitor = Process.Start(start)!;
        if (!competitor.WaitForExit(5000)) { competitor.Kill(); competitor.WaitForExit(); Assert.Fail("SQLite lock probe timed out"); }
        Assert.NotEqual(0, competitor.ExitCode);
        Assert.Contains("locked", competitor.StandardError.ReadToEnd(), StringComparison.OrdinalIgnoreCase);
    }
}
