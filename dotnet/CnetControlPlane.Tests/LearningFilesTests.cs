using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Microsoft.Win32.SafeHandles;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningFilesTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-learning-files-").FullName;
    private const UnixFileMode PrivateFile = UnixFileMode.UserRead | UnixFileMode.UserWrite;
    private const UnixFileMode PrivateDir = PrivateFile | UnixFileMode.UserExecute;
    [DllImport("libc", SetLastError = true)] private static extern int link(string oldpath, string newpath);
    [DllImport("libc", SetLastError = true)] private static extern int mkfifo(string path, uint mode);
    [DllImport("libc", SetLastError = true)] private static extern int fcntl(SafeFileHandle fd, int command, int argument);
    public LearningFilesTests() => File.SetUnixFileMode(root, PrivateDir);
    public void Dispose() => Directory.Delete(root, true);
    private void Put(string name, byte[] data)
    {
        File.WriteAllBytes(Path.Combine(root, name), data);
        File.SetUnixFileMode(Path.Combine(root, name), PrivateFile);
    }

    [Fact]
    public void ReadBindsPrivateRegularFileAndEnforcesByteLimit()
    {
        Put("source", [1, 2, 3]);
        using var files = LearningFiles.Open(root);
        Assert.Equal(new byte[] { 1, 2, 3 }, files.Read("source", 3));
        Assert.Throws<InvalidOperationException>(() => files.Read("source", 2));
        Assert.Throws<ArgumentException>(() => files.Read("source", 0));
    }

    [Theory]
    [InlineData("../source")]
    [InlineData("/source")]
    [InlineData("nested/source")]
    [InlineData(".")]
    [InlineData("a\0b")]
    public void ChildPathCannotEscapePinnedDirectory(string name)
    {
        using var files = LearningFiles.Open(root);
        Assert.Throws<ArgumentException>(() => files.Read(name, 10));
    }

    [Fact]
    public void SymlinkHardlinkFifoDirectoryAndNonprivateFileRefuse()
    {
        Put("source", [1]);
        File.CreateSymbolicLink(Path.Combine(root, "alias"), "source");
        Assert.Equal(0, link(Path.Combine(root, "source"), Path.Combine(root, "hard")));
        Assert.Equal(0, mkfifo(Path.Combine(root, "fifo"), 384));
        using var files = LearningFiles.Open(root);
        foreach (var name in new[] { "alias", "hard", "source", "fifo" })
            Assert.Throws<InvalidOperationException>(() => files.Read(name, 10));
        Put("public", [1]);
        File.SetUnixFileMode(Path.Combine(root, "public"), PrivateFile | UnixFileMode.GroupRead);
        Assert.Throws<InvalidOperationException>(() => files.Read("public", 10));
        Directory.CreateDirectory(Path.Combine(root, "dir"), PrivateDir);
        Assert.Throws<InvalidOperationException>(() => files.Read("dir", 10));
    }

    [Fact]
    public void ParentSymlinkAndSharedRootRefuse()
    {
        var child = Path.Combine(root, "child");
        Directory.CreateDirectory(child, PrivateDir);
        Directory.CreateSymbolicLink(Path.Combine(root, "alias"), child);
        Assert.Throws<InvalidOperationException>(() => LearningFiles.Open(Path.Combine(root, "alias")));
        File.SetUnixFileMode(child, PrivateDir | UnixFileMode.GroupRead);
        Assert.Throws<InvalidOperationException>(() => LearningFiles.Open(child));
        Assert.Throws<ArgumentException>(() => LearningFiles.Open(root + "/child/.."));
    }

    [Fact]
    public void PublicationIsPrivateDurableAndNeverOverwrites()
    {
        using var files = LearningFiles.Open(root);
        files.WriteNew("policy.json", [1, 2, 3]);
        Assert.Equal(new byte[] { 1, 2, 3 }, files.Read("policy.json", 3));
        Assert.Equal(PrivateFile, File.GetUnixFileMode(Path.Combine(root, "policy.json")));
        Assert.Throws<InvalidOperationException>(() => files.WriteNew("policy.json", [9]));
        Assert.Equal(new byte[] { 1, 2, 3 }, files.Read("policy.json", 3));
        Assert.Single(Directory.EnumerateFileSystemEntries(root));
    }

    [Fact]
    public void ChildCreationAndExclusiveOwnershipArePinned()
    {
        using var files = LearningFiles.Open(root);
        using var child = files.CreateDirectory("state");
        child.WriteNew("owner.lock", []);
        using (var owner = child.AcquireLock("owner.lock"))
            Assert.Throws<InvalidOperationException>(() => child.AcquireLock("owner.lock"));
        using var replacement = child.AcquireLock("owner.lock");
        Assert.Throws<InvalidOperationException>(() => files.CreateDirectory("state"));
    }

    [Fact]
    public void DisposedOwnershipDoesNotStayLockedThroughAnInheritedOpenDescription()
    {
        using var files = LearningFiles.Open(root);
        files.WriteNew("owner.lock", []);
        using var descriptor = files.OpenFile("owner.lock", write: true);
        using var owner = new LearningOwnerLock(descriptor); // Same acquisition used by AcquireLock.
        // dup and fork retain the same open-file description. This widens the
        // pre-exec inheritance window deterministically, without forking CLR threads.
        using var inherited = new SafeFileHandle((IntPtr)fcntl(descriptor, 1030, 0), ownsHandle: true); // F_DUPFD_CLOEXEC
        Assert.False(inherited.IsInvalid);
        Assert.Throws<InvalidOperationException>(() => files.AcquireLock("owner.lock"));
        owner.Dispose();
        LearningOwnerLock? replacement = null;
        Exception? refused = Record.Exception(() => replacement = files.AcquireLock("owner.lock"));
        using (replacement)
        {
            Assert.True(refused is null, "LEARNING_LOCK_RELEASE_RED inherited description retained disposed ownership: " + refused?.Message);
            owner.Dispose(); // Must not unlock the replacement generation.
            inherited.Dispose();
            Assert.Throws<InvalidOperationException>(() => files.AcquireLock("owner.lock"));
        }
        using var finalOwner = files.AcquireLock("owner.lock");
    }

    [Fact]
    public void ChildCreationRetainsPinnedParentAfterPathReplacement()
    {
        var original = Path.Combine(root, "original");
        var moved = Path.Combine(root, "moved");
        Directory.CreateDirectory(original, PrivateDir);
        using var files = LearningFiles.Open(original);
        Directory.Move(original, moved);
        Directory.CreateDirectory(original, PrivateDir);
        Directory.CreateDirectory(Path.Combine(original, "state"), PrivateDir);

        using var child = files.CreateDirectory("state");
        child.WriteNew("marker", [7]);
        Assert.True(File.Exists(Path.Combine(moved, "state", "marker")),
            "LEARNING_FILES_RED child reopened through replacement parent pathname");
        Assert.False(File.Exists(Path.Combine(original, "state", "marker")));
        Assert.Equal(new byte[] { 7 }, child.Read("marker", 1));
    }

    [Fact]
    public void MetadataValidationAcceptsPrivateFilesAndOnlyExplicitMissingNames()
    {
        Put("source", [1, 2, 3]);
        using var files = LearningFiles.Open(root);
        Assert.Equal(3UL, files.ValidateFile("source")!.Value.Size);
        File.SetUnixFileMode(Path.Combine(root, "source"), UnixFileMode.UserRead);
        Assert.Equal(3UL, files.ValidateFile("source")!.Value.Size);
        Assert.Null(files.ValidateFile("missing", allowMissing: true));
        Assert.Throws<InvalidOperationException>(() => files.ValidateFile("missing"));
        Assert.Throws<ArgumentException>(() => files.ValidateFile("../source", allowMissing: true));
    }

    [Fact]
    public void MetadataValidationNeverFollowsLinksOrAcceptsNonregularFiles()
    {
        Put("source", [1]);
        File.CreateSymbolicLink(Path.Combine(root, "dangling"), "missing");
        File.CreateSymbolicLink(Path.Combine(root, "alias"), "source");
        Assert.Equal(0, link(Path.Combine(root, "source"), Path.Combine(root, "hard")));
        Assert.Equal(0, mkfifo(Path.Combine(root, "fifo"), 384));
        Directory.CreateDirectory(Path.Combine(root, "dir"), PrivateDir);
        Put("public", [1]);
        File.SetUnixFileMode(Path.Combine(root, "public"), PrivateFile | UnixFileMode.GroupRead);
        using var files = LearningFiles.Open(root);
        foreach (var name in new[] { "dangling", "alias", "hard", "source", "fifo", "dir", "public" })
            Assert.Throws<InvalidOperationException>(() => files.ValidateFile(name, allowMissing: true));
    }

    [Fact]
    public void OptionalMissingMetadataDoesNotHideSearchPermissionFailure()
    {
        using var files = LearningFiles.Open(root);
        File.SetUnixFileMode(root, PrivateFile);
        try
        {
            Assert.Throws<InvalidOperationException>(() => files.ValidateFile("missing", allowMissing: true));
        }
        finally { File.SetUnixFileMode(root, PrivateDir); }
    }

    [Fact]
    public void PathIdentityRefusesRenamedReplacedOrNonprivateRoot()
    {
        var original = Path.Combine(root, "original");
        var moved = Path.Combine(root, "moved");
        Directory.CreateDirectory(original, PrivateDir);
        using var files = LearningFiles.Open(original);
        files.AssertPathIdentity();
        File.SetUnixFileMode(original, PrivateDir | UnixFileMode.GroupRead);
        Assert.Throws<InvalidOperationException>(files.AssertPathIdentity);
        File.SetUnixFileMode(original, PrivateDir);
        Directory.Move(original, moved);
        Assert.Throws<InvalidOperationException>(files.AssertPathIdentity);
        Directory.CreateDirectory(original, PrivateDir);
        Assert.Throws<InvalidOperationException>(files.AssertPathIdentity);
    }
}
