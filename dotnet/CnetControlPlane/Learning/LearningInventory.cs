using System.Text;
using Microsoft.Win32.SafeHandles;
using static CnetControlPlane.Learning.LearningLinux;

namespace CnetControlPlane.Learning;

// Copy measurements only: never an admission receipt or canonical snapshot hash.
internal readonly record struct LearningInventoryCopy(string SetName, string FullPath, int UnitCount, long ArtifactBytes);

internal enum LearningInventoryTestBoundary { AfterPreflight, AfterFirstFile, BeforeFinalSync }

/// <summary>
/// Trusted-parent byte copy after the producer has been sealed and reaped.
/// Does not parse capsules, authenticate sources, freeze, stage or approve.
/// The owner must serialize calls and keep all roots stable. Metadata checks
/// detect observed mutation; this is not isolation from a hostile same-UID owner.
/// Failure after creation retains the task-owned candidate for accounting.
/// </summary>
internal static class LearningInventory
{
    private static readonly string[] Artifacts = ["manifest.cknow", "unit.cnb", "frontend.cvfa"];
    private readonly record struct Identity(uint Major, uint Minor, ulong Inode)
    {
        internal static Identity Of(Stat value) => new(value.DeviceMajor, value.DeviceMinor, value.Inode);
    }
    private sealed record FileEntry(string Name, Stat Metadata);
    private sealed record UnitEntry(string Name, Stat Metadata, FileEntry[] Files);
    private sealed class Root(string path, SafeFileHandle fd, bool writable, Identity[] chain) : IDisposable
    {
        internal string Path { get; } = path;
        internal SafeFileHandle Fd { get; } = fd;
        internal Identity[] Chain { get; } = chain;
        internal Stat Before { get; } = Inspect(fd);
        internal void AssertPathIdentity()
        {
            using var current = OpenRoot(Path, writable);
            if (!Chain.SequenceEqual(current.Chain)) throw Refused("root_path_changed");
        }
        public void Dispose() => Fd.Dispose();
    }

    internal static LearningInventoryCopy CopyAndAppend(string activeSnapshotRoot, string workerOutputRoot,
        string setsRoot, string newSetName, string appendedUnitName, long maxCopyBytes)
        => Copy(activeSnapshotRoot, workerOutputRoot, setsRoot, newSetName, appendedUnitName, maxCopyBytes, null);

    // Friend-test seam only; production entry above never accepts a callback.
    internal static LearningInventoryCopy CopyAndAppendForTest(string activeSnapshotRoot, string workerOutputRoot,
        string setsRoot, string newSetName, string appendedUnitName, long maxCopyBytes,
        Action<LearningInventoryTestBoundary> boundary)
        => Copy(activeSnapshotRoot, workerOutputRoot, setsRoot, newSetName, appendedUnitName, maxCopyBytes, boundary);

    private static InvalidOperationException Refused(string reason) => new("learning_inventory_" + reason);
    private static void UnitName(string name)
    {
        LearningFiles.Component(name);
        if (name[0] == '.') throw new ArgumentException("learning_inventory_unit_name");
    }
    private static void RequireDirectory(Stat value, bool writable = false)
    {
        if ((value.Mode & 0xf000) != 0x4000 || value.Owner != geteuid()
            || (value.Mode & 0xfff) != 0x1c0 && (writable || (value.Mode & 0xfff) != 0x140))
            throw Refused("private_directory_required");
    }
    private static Root OpenRoot(string path, bool writable)
    {
        if (string.IsNullOrEmpty(path) || path[0] != '/' || path.Length > 4095 || path.Contains('\0')
            || path == "/" || path.Split('/').Skip(1).Any(p => p.Length == 0 || p is "." or ".."))
            throw new ArgumentException("learning_inventory_canonical_path_required");
        var chain = new List<Identity>();
        var fd = Handle(open("/", DirectoryFlag | CloseExec, 0));
        try
        {
            chain.Add(Identity.Of(Inspect(fd)));
            foreach (var part in path.Split('/').Skip(1))
            {
                var parent = Inspect(fd);
                if ((parent.Owner != 0 && parent.Owner != geteuid())
                    || ((parent.Mode & 0x12) != 0 && (parent.Owner != 0 || (parent.Mode & 0x200) == 0)))
                    throw Refused("untrusted_ancestor");
                var next = Handle(openat(fd, part, DirectoryFlag | NoFollow | CloseExec, 0));
                fd.Dispose(); fd = next;
                chain.Add(Identity.Of(Inspect(fd)));
            }
            RequireDirectory(Inspect(fd), writable);
            return new Root(path, fd, writable, chain.ToArray());
        }
        catch { fd.Dispose(); throw; }
    }
    private static void Nonoverlap(Root first, Root second)
    {
        if (first.Path == second.Path || first.Path.StartsWith(second.Path + "/", StringComparison.Ordinal)
            || second.Path.StartsWith(first.Path + "/", StringComparison.Ordinal)
            || first.Chain.Contains(Identity.Of(second.Before)) || second.Chain.Contains(Identity.Of(first.Before)))
            throw Refused("overlapping_roots");
    }
    private static string[] Entries(SafeFileHandle directory, int maximum)
    {
        var names = new List<string>();
        // The retained descriptor outlives lazy enumeration. /proc and the
        // kernel are trusted; only returned leaf names enter openat/no-follow.
        var options = new EnumerationOptions { AttributesToSkip = 0, IgnoreInaccessible = false, RecurseSubdirectories = false };
        foreach (var path in Directory.EnumerateFileSystemEntries($"/proc/self/fd/{directory.DangerousGetHandle().ToInt64()}", "*", options))
        {
            if (names.Count == maximum) throw Refused("entry_limit");
            var name = System.IO.Path.GetFileName(path);
            try { UnitName(name); }
            catch (ArgumentException) { throw Refused("invalid_entry"); }
            names.Add(name);
        }
        names.Sort(StringComparer.Ordinal);
        if (names.Distinct(StringComparer.Ordinal).Count() != names.Count) throw Refused("duplicate_entry");
        return names.ToArray();
    }
    private static void RequireFile(string name, Stat value)
    {
        var limit = name == "unit.cnb" ? 64UL * 1024 * 1024 : 16UL * 1024 * 1024;
        if (!Artifacts.Contains(name, StringComparer.Ordinal) || (value.Mode & 0xf000) != 0x8000
            || value.Owner != geteuid() || value.Links != 1 || (value.Mode & 0xfff) is not (0x100 or 0x180)
            || value.Size < 1 || value.Size > limit) throw Refused("private_artifact_required");
    }
    private static UnitEntry[] Inventory(Root root, int maximumUnits, ref long total, long maximumBytes)
    {
        var units = new List<UnitEntry>();
        foreach (var name in Entries(root.Fd, maximumUnits))
        {
            using var fd = Handle(openat(root.Fd, name, DirectoryFlag | NoFollow | CloseExec, 0));
            var before = Inspect(fd);
            RequireDirectory(before);
            var names = Entries(fd, 3);
            if (!names.Contains("manifest.cknow") || !names.Contains("unit.cnb")) throw Refused("required_artifacts");
            var files = new List<FileEntry>();
            foreach (var file in names)
            {
                var value = InspectAt(fd, file, false)!.Value;
                RequireFile(file, value);
                if (value.Size > (ulong)(maximumBytes - total)) throw Refused("copy_byte_limit");
                total += (long)value.Size;
                files.Add(new FileEntry(file, value));
            }
            if (!before.Equals(Inspect(fd))) throw Refused("source_changed");
            units.Add(new UnitEntry(name, before, files.ToArray()));
        }
        if (!root.Before.Equals(Inspect(root.Fd))) throw Refused("source_changed");
        return units.ToArray();
    }
    private static void ValidateSource(Root root, UnitEntry[] units)
    {
        root.AssertPathIdentity();
        if (!root.Before.Equals(Inspect(root.Fd))) throw Refused("source_changed");
        ValidateUnits(root.Fd, units);
        if (!root.Before.Equals(Inspect(root.Fd))) throw Refused("source_changed");
    }
    private static void ValidateUnits(SafeFileHandle directory, UnitEntry[] units)
    {
        if (!Entries(directory, units.Length).SequenceEqual(units.Select(u => u.Name).Order(StringComparer.Ordinal)))
            throw Refused("source_changed");
        foreach (var unit in units)
        {
            using var fd = Handle(openat(directory, unit.Name, DirectoryFlag | NoFollow | CloseExec, 0));
            if (!unit.Metadata.Equals(Inspect(fd)) || !Entries(fd, 3).SequenceEqual(unit.Files.Select(f => f.Name)))
                throw Refused("source_changed");
            foreach (var file in unit.Files)
                if (InspectAt(fd, file.Name, false) is not Stat named || !file.Metadata.Equals(named))
                    throw Refused("source_changed");
        }
    }
    private static UnitEntry CopyUnit(Root source, UnitEntry unit, SafeFileHandle target, string name,
        byte[] buffer, Action? afterFile)
    {
        using var input = Handle(openat(source.Fd, unit.Name, DirectoryFlag | NoFollow | CloseExec, 0));
        if (!unit.Metadata.Equals(Inspect(input))) throw Refused("source_changed");
        if (mkdirat(target, name, 0x1c0) != 0) throw Refused("new_unit_required");
        using var output = Handle(openat(target, name, DirectoryFlag | NoFollow | CloseExec, 0));
        RequireDirectory(Inspect(output), writable: true);
        var outputFiles = new List<FileEntry>();
        foreach (var file in unit.Files)
        {
            using var from = Handle(openat(input, file.Name, NoFollow | NonBlock | CloseExec, 0));
            if (!file.Metadata.Equals(Inspect(from))) throw Refused("source_changed");
            using var to = Handle(openat(output, file.Name, 1 | Create | Exclusive | NoFollow | CloseExec, 0x100));
            var created = Inspect(to);
            if ((created.Mode & 0xfff) != 0x100 || created.Owner != geteuid() || created.Links != 1)
                throw Refused("private_output_required");
            long position = 0, length = (long)file.Metadata.Size;
            while (position < length)
            {
                var count = RandomAccess.Read(from, buffer.AsSpan(0, (int)Math.Min(buffer.Length, length - position)), position);
                if (count == 0) throw Refused("source_changed");
                RandomAccess.Write(to, buffer.AsSpan(0, count), position);
                position += count;
            }
            if (RandomAccess.Read(from, buffer.AsSpan(0, 1), position) != 0 || !file.Metadata.Equals(Inspect(from))
                || InspectAt(input, file.Name, false) is not Stat named || !file.Metadata.Equals(named))
                throw Refused("source_changed");
            Sync(to);
            var written = Inspect(to);
            RequireFile(file.Name, written);
            if ((written.Mode & 0xfff) != 0x100 || written.Size != file.Metadata.Size
                || InspectAt(output, file.Name, false) is not Stat outputName || !written.Equals(outputName))
                throw Refused("output_changed");
            outputFiles.Add(new FileEntry(file.Name, written));
            afterFile?.Invoke();
        }
        if (!unit.Metadata.Equals(Inspect(input))) throw Refused("source_changed");
        Sync(output);
        return new UnitEntry(name, Inspect(output), outputFiles.ToArray());
    }
    private static LearningInventoryCopy Copy(string activeSnapshotRoot, string workerOutputRoot,
        string setsRoot, string newSetName, string appendedUnitName, long maxCopyBytes,
        Action<LearningInventoryTestBoundary>? boundary)
    {
        RequireSupported();
        if (string.IsNullOrEmpty(newSetName) || newSetName.Length > 63
            || !newSetName.All(c => c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '_' or '-'))
            throw new ArgumentException("learning_inventory_set_name");
        UnitName(appendedUnitName);
        if (maxCopyBytes is < 1 or > 4L * 1024 * 1024 * 1024) throw new ArgumentException("learning_inventory_copy_limit");
        var created = false;
        try
        {
            using var active = OpenRoot(activeSnapshotRoot, false);
            using var worker = OpenRoot(workerOutputRoot, false);
            using var sets = OpenRoot(setsRoot, true);
            var candidatePath = System.IO.Path.Combine(setsRoot, newSetName);
            if (Encoding.UTF8.GetByteCount(candidatePath) > 4095)
                throw new ArgumentException("learning_inventory_candidate_path_length");
            Nonoverlap(active, worker); Nonoverlap(active, sets); Nonoverlap(worker, sets);
            long total = 0;
            var incumbent = Inventory(active, 4095, ref total, maxCopyBytes);
            var addition = Inventory(worker, 1, ref total, maxCopyBytes);
            if (addition.Length != 1 || addition[0].Name != "capsule") throw Refused("worker_capsule_required");
            if (incumbent.Any(u => u.Name == appendedUnitName)) throw Refused("unit_collision");
            if (InspectAt(sets.Fd, newSetName, true).HasValue) throw Refused("new_set_required");
            boundary?.Invoke(LearningInventoryTestBoundary.AfterPreflight);
            ValidateSource(active, incumbent); ValidateSource(worker, addition); sets.AssertPathIdentity();
            if (mkdirat(sets.Fd, newSetName, 0x1c0) != 0) throw Refused("new_set_required");
            created = true;
            using var output = Handle(openat(sets.Fd, newSetName, DirectoryFlag | NoFollow | CloseExec, 0));
            RequireDirectory(Inspect(output), writable: true);
            var outputIdentity = Identity.Of(Inspect(output));
            var buffer = new byte[64 * 1024];
            var firstFile = true;
            void AfterFile()
            {
                if (!firstFile) return;
                firstFile = false;
                boundary?.Invoke(LearningInventoryTestBoundary.AfterFirstFile);
            }
            var copied = new List<UnitEntry>();
            foreach (var unit in incumbent) copied.Add(CopyUnit(active, unit, output, unit.Name, buffer, AfterFile));
            copied.Add(CopyUnit(worker, addition[0], output, appendedUnitName, buffer, AfterFile));
            var outputBefore = Inspect(output);
            boundary?.Invoke(LearningInventoryTestBoundary.BeforeFinalSync);
            Sync(output); Sync(sets.Fd);
            ValidateSource(active, incumbent); ValidateSource(worker, addition); sets.AssertPathIdentity();
            ValidateUnits(output, copied.ToArray());
            if (!outputBefore.Equals(Inspect(output)) || InspectAt(sets.Fd, newSetName, false) is not Stat named
                || outputIdentity != Identity.Of(named) || !outputBefore.Equals(named))
                throw Refused("output_path_changed");
            return new LearningInventoryCopy(newSetName, candidatePath, incumbent.Length + 1, total);
        }
        catch (Exception exception) when (created)
        {
            // No cleanup: the named set may be partial or durably complete.
            throw new InvalidOperationException("learning_inventory_copy_incomplete", exception);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        { throw Refused("io_refused"); }
    }
}
