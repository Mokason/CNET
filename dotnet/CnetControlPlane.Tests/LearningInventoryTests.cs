using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningInventoryTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-learning-inventory-").FullName;
    private const UnixFileMode PrivateFile = UnixFileMode.UserRead | UnixFileMode.UserWrite;
    private const UnixFileMode PrivateDir = PrivateFile | UnixFileMode.UserExecute;
    private string Active => Path.Combine(root, "active");
    private string Worker => Path.Combine(root, "worker");
    private string Sets => Path.Combine(root, "sets");
    [DllImport("libc", SetLastError = true)] private static extern int link(string oldpath, string newpath);
    [DllImport("libc", SetLastError = true)] private static extern int mkfifo(string path, uint mode);
    public LearningInventoryTests()
    {
        File.SetUnixFileMode(root, PrivateDir);
        foreach (var directory in new[] { Active, Worker, Sets }) Directory.CreateDirectory(directory, PrivateDir);
    }
    public void Dispose()
    {
        foreach (var path in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories))
            File.SetUnixFileMode(path, PrivateDir);
        Directory.Delete(root, true);
    }
    private static void Unit(string parent, string name, bool frontend = false)
    {
        var path = Path.Combine(parent, name);
        Directory.CreateDirectory(path, PrivateDir);
        foreach (var file in frontend ? new[] { "manifest.cknow", "unit.cnb", "frontend.cvfa" }
                                     : new[] { "manifest.cknow", "unit.cnb" })
        {
            File.WriteAllBytes(Path.Combine(path, file), [0, 1, 2, 255]);
            File.SetUnixFileMode(Path.Combine(path, file), PrivateFile);
        }
    }
    private LearningInventoryCopy Copy(long limit = 1024) => LearningInventory.CopyAndAppend(Active, Worker, Sets, "candidate", "learned_1", limit);
    private LearningInventoryCopy At(Action<LearningInventoryTestBoundary> hook) => LearningInventory.CopyAndAppendForTest(
        Active, Worker, Sets, "candidate", "learned_1", 1024, hook);
    private void NoCandidate() => Assert.Empty(Directory.EnumerateFileSystemEntries(Sets));

    [Fact]
    public void CopyPreservesEveryIncumbentAndAppendsOpaqueWorkerBytes()
    {
        Unit(Active, "incumbent", frontend: true);
        Unit(Worker, "capsule");
        File.SetUnixFileMode(Active, UnixFileMode.UserRead | UnixFileMode.UserExecute);
        File.SetUnixFileMode(Path.Combine(Active, "incumbent"), UnixFileMode.UserRead | UnixFileMode.UserExecute);
        var copied = LearningInventory.CopyAndAppend(Active, Worker, Sets, "candidate_1", "learned_1", 20);
        Assert.Equal(20, copied.ArtifactBytes);
        Assert.Equal(2, copied.UnitCount);
        Assert.Equal("candidate_1", copied.SetName);
        Assert.Equal(Path.Combine(Sets, "candidate_1"), copied.FullPath);
        Assert.Equal(new[] { "incumbent", "learned_1" }, Directory.GetDirectories(copied.FullPath).Select(Path.GetFileName).Order());
        foreach (var unit in new[] { "incumbent", "learned_1" })
        {
            Assert.Equal(PrivateDir, File.GetUnixFileMode(Path.Combine(copied.FullPath, unit)));
            foreach (var file in Directory.GetFiles(Path.Combine(copied.FullPath, unit)))
            {
                Assert.Equal(new byte[] { 0, 1, 2, 255 }, File.ReadAllBytes(file));
                Assert.Equal(UnixFileMode.UserRead, File.GetUnixFileMode(file));
            }
        }
        Assert.True(File.Exists(Path.Combine(Active, "incumbent", "frontend.cvfa")));
        Assert.True(File.Exists(Path.Combine(Worker, "capsule", "unit.cnb")));
    }

    [Fact]
    public void EmptyExistingActiveAndExactLimitsAreAccepted()
    {
        Unit(Worker, "capsule");
        var copied = LearningInventory.CopyAndAppend(Active, Worker, Sets, new string('s', 63), new string('u', 96), 8);
        Assert.Equal(1, copied.UnitCount);
        Assert.Equal(8, copied.ArtifactBytes);
        Assert.Equal(PrivateDir, File.GetUnixFileMode(copied.FullPath));
        Assert.Equal(8, LearningInventory.CopyAndAppend(Active, Worker, Sets, "max_limit", "unit", 4L * 1024 * 1024 * 1024).ArtifactBytes);
    }

    [Fact]
    public void CopyStreamsAcrossMultiple64KiBChunks()
    {
        Unit(Worker, "capsule");
        var bytes = Enumerable.Range(0, 3 * 65536 + 7).Select(i => (byte)(i * 37)).ToArray();
        File.WriteAllBytes(Path.Combine(Worker, "capsule", "unit.cnb"), bytes);
        var copied = Copy(bytes.Length + 4);
        Assert.Equal(bytes, File.ReadAllBytes(Path.Combine(copied.FullPath, "learned_1", "unit.cnb")));
    }

    [Theory]
    [InlineData("wrong_worker_leaf")]
    [InlineData("extra_worker_leaf")]
    [InlineData("unknown_artifact")]
    [InlineData("missing_manifest")]
    [InlineData("empty_payload")]
    [InlineData("hardlink")]
    [InlineData("symlink")]
    [InlineData("fifo")]
    [InlineData("public_file")]
    [InlineData("public_unit")]
    [InlineData("public_root")]
    [InlineData("nested_directory")]
    public void InvalidWorkerInventoryRefusesBeforeCreatingCandidate(string fault)
    {
        Unit(Worker, "capsule");
        var payload = Path.Combine(Worker, "capsule", "unit.cnb");
        switch (fault)
        {
            case "wrong_worker_leaf": Directory.Move(Path.Combine(Worker, "capsule"), Path.Combine(Worker, "other")); break;
            case "extra_worker_leaf": Unit(Worker, "other"); break;
            case "unknown_artifact": File.WriteAllText(Path.Combine(Worker, "capsule", "unexpected"), "x"); break;
            case "missing_manifest": File.Delete(Path.Combine(Worker, "capsule", "manifest.cknow")); break;
            case "empty_payload": File.WriteAllBytes(payload, []); break;
            case "hardlink": Assert.Equal(0, link(payload, Path.Combine(root, "second_link"))); break;
            case "symlink": File.Delete(payload); File.CreateSymbolicLink(payload, "manifest.cknow"); break;
            case "fifo": File.Delete(payload); Assert.Equal(0, mkfifo(payload, 384)); break;
            case "public_file": File.SetUnixFileMode(payload, PrivateFile | UnixFileMode.GroupRead); break;
            case "public_unit": File.SetUnixFileMode(Path.Combine(Worker, "capsule"), PrivateDir | UnixFileMode.GroupRead); break;
            case "public_root": File.SetUnixFileMode(Worker, PrivateDir | UnixFileMode.GroupRead); break;
            case "nested_directory": File.Delete(payload); Directory.CreateDirectory(payload, PrivateDir); break;
        }
        Assert.ThrowsAny<InvalidOperationException>(() => Copy());
        NoCandidate();
    }

    [Fact]
    public void MissingActiveUnknownActiveAndReadonlyOutputRefuse()
    {
        Unit(Worker, "capsule");
        Assert.Throws<InvalidOperationException>(() => LearningInventory.CopyAndAppend(Path.Combine(root, "missing"), Worker, Sets, "candidate", "unit", 100));
        File.WriteAllText(Path.Combine(Active, "unexpected"), "x");
        Assert.Throws<InvalidOperationException>(() => Copy());
        File.Delete(Path.Combine(Active, "unexpected"));
        File.SetUnixFileMode(Sets, UnixFileMode.UserRead | UnixFileMode.UserExecute);
        Assert.Throws<InvalidOperationException>(() => Copy());
        NoCandidate();
    }

    [Theory]
    [InlineData("unit.cnb", 67108865L)]
    [InlineData("manifest.cknow", 16777217L)]
    [InlineData("frontend.cvfa", 16777217L)]
    public void NativePerArtifactLimitsArePreservedWithoutReadingOversizeBytes(string name, long length)
    {
        Unit(Worker, "capsule", frontend: true);
        using (var file = File.OpenWrite(Path.Combine(Worker, "capsule", name))) file.SetLength(length);
        Assert.Throws<InvalidOperationException>(() => Copy(4L * 1024 * 1024 * 1024));
        NoCandidate();
    }

    [Fact]
    public void AggregateAndUnitCountPreflightHappensBeforeAnyOutput()
    {
        Unit(Worker, "capsule");
        Assert.Throws<InvalidOperationException>(() => Copy(7));
        NoCandidate();
        for (var i = 0; i < 64; i++)
        {
            Unit(Active, "unit_" + i);
            using var file = File.OpenWrite(Path.Combine(Active, "unit_" + i, "unit.cnb"));
            file.SetLength(64L * 1024 * 1024);
        }
        Assert.Throws<InvalidOperationException>(() => Copy(4L * 1024 * 1024 * 1024));
        NoCandidate();
        for (var i = 64; i < 4096; i++) Directory.CreateDirectory(Path.Combine(Active, "unit_" + i), PrivateDir);
        Assert.Throws<InvalidOperationException>(() => Copy(4L * 1024 * 1024 * 1024));
        NoCandidate();
    }

    [Theory]
    [InlineData(0L)]
    [InlineData(-1L)]
    [InlineData(4294967297L)]
    public void CopyLimitMustBeBoundedPositiveOwnerInput(long limit)
    {
        Assert.Throws<ArgumentException>(() => Copy(limit));
        NoCandidate();
    }

    [Theory]
    [InlineData("bad.name", "unit")]
    [InlineData("../escape", "unit")]
    [InlineData("candidate", ".hidden")]
    [InlineData("candidate", "../escape")]
    [InlineData("candidate", "nested/unit")]
    public void NativeNamesCannotEscapeOrCreateHiddenCandidates(string setName, string unitName)
    {
        Assert.Throws<ArgumentException>(() => LearningInventory.CopyAndAppend(Active, Worker, Sets, setName, unitName, 100));
        NoCandidate();
    }

    [Fact]
    public void ExistingSetUnitCollisionAndRootOverlapNeverOverwriteInputs()
    {
        Unit(Active, "learned_1"); Unit(Worker, "capsule");
        Assert.Throws<InvalidOperationException>(() => Copy());
        NoCandidate();
        Directory.Move(Path.Combine(Active, "learned_1"), Path.Combine(Active, "old"));
        Directory.CreateDirectory(Path.Combine(Sets, "candidate"), PrivateDir);
        Assert.Throws<InvalidOperationException>(() => Copy());
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(Sets, "candidate")));
        Assert.Throws<InvalidOperationException>(() => LearningInventory.CopyAndAppend(Active, Worker, Active, "new", "unit", 100));
        Assert.Throws<InvalidOperationException>(() => LearningInventory.CopyAndAppend(root, Worker, Sets, "new", "unit", 100));
        Directory.CreateSymbolicLink(Path.Combine(root, "alias"), Active);
        Assert.Throws<InvalidOperationException>(() => LearningInventory.CopyAndAppend(Path.Combine(root, "alias"), Worker, Sets, "new", "unit", 100));
        Assert.Equal(new byte[] { 0, 1, 2, 255 }, File.ReadAllBytes(Path.Combine(Active, "old", "unit.cnb")));
    }

    [Fact]
    public void SourceReplacementAfterPreflightRefusesBeforeCreation()
    {
        Unit(Worker, "capsule");
        Assert.Throws<InvalidOperationException>(() => At(boundary =>
        {
            if (boundary != LearningInventoryTestBoundary.AfterPreflight) return;
            Directory.Move(Worker, Path.Combine(root, "old_worker"));
            Directory.CreateDirectory(Worker, PrivateDir); Unit(Worker, "capsule");
        }));
        NoCandidate();
    }

    [Fact]
    public void EarlierSourceMutationAfterFirstCopyRetainsPartialCandidateAndRefuses()
    {
        Unit(Active, "old"); Unit(Worker, "capsule");
        var exception = Assert.Throws<InvalidOperationException>(() => At(boundary =>
        {
            if (boundary == LearningInventoryTestBoundary.AfterFirstFile)
                File.WriteAllBytes(Path.Combine(Active, "old", "manifest.cknow"), [9, 9, 9, 9]);
        }));
        Assert.Equal("learning_inventory_copy_incomplete", exception.Message);
        Assert.True(Directory.Exists(Path.Combine(Sets, "candidate")));
        Assert.True(File.Exists(Path.Combine(Worker, "capsule", "unit.cnb")));
    }

    [Theory]
    [InlineData(1)] // AfterFirstFile
    [InlineData(2)] // BeforeFinalSync
    public void InjectedInterruptionAtNamedBoundaryIsNotReportedAsCommitted(int interrupted)
    {
        Unit(Worker, "capsule");
        var exception = Assert.Throws<InvalidOperationException>(() => At(boundary =>
        {
            if ((int)boundary == interrupted) throw new IOException("injected_interruption_not_a_real_fsync_failure");
        }));
        Assert.Equal("learning_inventory_copy_incomplete", exception.Message);
        Assert.True(Directory.Exists(Path.Combine(Sets, "candidate")));
        Assert.True(File.Exists(Path.Combine(Worker, "capsule", "unit.cnb")));
    }

    [Fact]
    public void OutputRootReplacementAtFinalSyncBoundaryMustRefuseWithoutRedirectingWrites()
    {
        Unit(Worker, "capsule");
        var moved = Path.Combine(root, "moved_sets");
        var exception = Assert.Throws<InvalidOperationException>(() => At(boundary =>
        {
            if (boundary != LearningInventoryTestBoundary.BeforeFinalSync) return;
            Directory.Move(Sets, moved);
            Directory.CreateDirectory(Sets, PrivateDir);
        }));
        Assert.Equal("learning_inventory_copy_incomplete", exception.Message);
        Assert.Empty(Directory.EnumerateFileSystemEntries(Sets));
        Assert.True(File.Exists(Path.Combine(moved, "candidate", "learned_1", "unit.cnb")),
            "LEARNING_INVENTORY_RED output fd must remain pinned while replacement pathname refuses");
    }

    [Fact]
    public void OutputFileMutationAtFinalBoundaryMustRefuse()
    {
        Unit(Worker, "capsule");
        var exception = Assert.Throws<InvalidOperationException>(() => At(boundary =>
        {
            if (boundary != LearningInventoryTestBoundary.BeforeFinalSync) return;
            var path = Path.Combine(Sets, "candidate", "learned_1", "manifest.cknow");
            File.SetUnixFileMode(path, PrivateFile);
            File.WriteAllBytes(path, [8, 8, 8, 8]);
            File.SetUnixFileMode(path, UnixFileMode.UserRead);
        }));
        Assert.Equal("learning_inventory_copy_incomplete", exception.Message);
        Assert.True(File.Exists(Path.Combine(Sets, "candidate", "learned_1", "manifest.cknow")));
    }

    [Theory]
    [InlineData(".unknown")]
    [InlineData("bad name")]
    [InlineData("é")]
    public void UntrustedInvalidLeafIsAFixedOperationalRefusal(string leaf)
    {
        Unit(Worker, "capsule");
        File.WriteAllBytes(Path.Combine(Worker, "capsule", leaf), [9]);
        var exception = Assert.Throws<InvalidOperationException>(() => Copy());
        Assert.Equal("learning_inventory_invalid_entry", exception.Message);
        NoCandidate();
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void UnusableCombinedCandidatePathRefusesBeforeCreation(bool multibyte)
    {
        Unit(Worker, "capsule");
        var deep = Sets;
        while (Encoding.UTF8.GetByteCount(deep) < 4059)
        {
            var remaining = 4060 - Encoding.UTF8.GetByteCount(deep) - 1;
            var length = Math.Min(100, remaining / (multibyte ? 2 : 1));
            deep = Path.Combine(deep, new string(multibyte ? 'é' : 'p', length));
            Directory.CreateDirectory(deep, PrivateDir);
        }
        Assert.InRange(Encoding.UTF8.GetByteCount(deep), 4059, 4060);
        Assert.True(Encoding.UTF8.GetByteCount(Path.Combine(deep, new string('s', 63))) > 4095);
        try
        {
            var exception = Assert.Throws<ArgumentException>(() => LearningInventory.CopyAndAppend(
                Active, Worker, deep, new string('s', 63), "learned", 100));
            Assert.Equal("learning_inventory_candidate_path_length", exception.Message);
            Assert.Empty(Directory.EnumerateFileSystemEntries(deep));
        }
        finally
        {
            // Shorten this test-owned root so cleanup never relies on a
            // candidate pathname longer than Linux/native PATH_MAX.
            Directory.Move(deep, Path.Combine(root, "shortened_sets"));
        }
    }
}
