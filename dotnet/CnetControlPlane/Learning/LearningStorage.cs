using Microsoft.Win32.SafeHandles;
using static CnetControlPlane.Learning.LearningLinux;

namespace CnetControlPlane.Learning;

internal sealed class LearningStorageUsage
{
    public long Bytes { get; }
    public int FileCount { get; }
    public int DirectoryCount { get; }
    internal LearningStorageUsage(long bytes, int files, int directories)
    { Bytes = bytes; FileCount = files; DirectoryCount = directories; }
}

/// <summary>
/// Metadata-only logical-byte accounting for serialized owner preflight and
/// post-reap checks around fixed bounded workers/copies. This does not reserve
/// space or enforce a kernel quota. Concurrent owner writes after a subtree
/// has been checked are not transactional; this is not hostile-owner isolation.
/// Keep runtime installation and daemon IPC outside the measured private root.
/// </summary>
internal static class LearningStorage
{
    // Compact stamps keep retained metadata bounded independently of file size.
    private readonly record struct Stamp(ulong Inode, ulong Size, uint DeviceMajor, uint DeviceMinor,
        uint Owner, uint Links, ushort Mode, long ChangeSeconds, uint ChangeNanos, long ModifySeconds, uint ModifyNanos)
    {
        internal static Stamp From(Stat value) => new(value.Inode, value.Size, value.DeviceMajor, value.DeviceMinor,
            value.Owner, value.Links, value.Mode, value.ChangeSeconds, value.ChangeNanos, value.ModifySeconds, value.ModifyNanos);
    }
    private sealed class Frame(SafeFileHandle directory, Stamp before, int depth) : IDisposable
    {
        internal SafeFileHandle Directory { get; } = directory;
        internal Stamp Before { get; } = before;
        internal int Depth { get; } = depth;
        internal List<(string Name, Stamp Before)> Observed { get; } = [];
        internal IEnumerator<string> Entries { get; } = System.IO.Directory.EnumerateFileSystemEntries(
            $"/proc/self/fd/{directory.DangerousGetHandle().ToInt64()}", "*",
            new EnumerationOptions { AttributesToSkip = 0, IgnoreInaccessible = false, RecurseSubdirectories = false }).GetEnumerator();
        public void Dispose() { try { Entries.Dispose(); } finally { Directory.Dispose(); } }
    }

    /// <summary>
    /// maxBytes is 1..4GiB. DirectoryCount includes root (depth zero); at most
    /// eight nested directory levels and 262144 entries excluding root are read.
    /// File bytes use st_size, including sparse holes; empty files are permitted.
    /// File inodes are never opened/closed, preserving live SQLite POSIX locks.
    /// </summary>
    internal static LearningStorageUsage Measure(string root, long maxBytes)
    {
        if (maxBytes is < 1 or > 4L * 1024 * 1024 * 1024)
            throw new ArgumentException("learning_storage_byte_limit_argument");
        using var files = LearningFiles.Open(root);
        var stack = new Stack<Frame>(9);
        long bytes = 0;
        var fileCount = 0; var directoryCount = 1; var entryCount = 0;
        try
        {
            files.AssertPathIdentity();
            Push(Handle(open(root, DirectoryFlag | NoFollow | CloseExec, 0)), 0, null);
            var origin = stack.Peek().Before;
            while (stack.Count != 0)
            {
                var frame = stack.Peek();
                if (frame.Entries.MoveNext())
                {
                    if (++entryCount > 262144) throw new InvalidOperationException("learning_storage_entry_limit");
                    var name = Path.GetFileName(frame.Entries.Current);
                    LearningFiles.Component(name);
                    var value = InspectAt(frame.Directory, name, false)!.Value;
                    if (value.DeviceMajor != origin.DeviceMajor || value.DeviceMinor != origin.DeviceMinor)
                        throw new InvalidOperationException("learning_storage_device");
                    var isDirectory = (value.Mode & 0xf000) == 0x4000;
                    if (isDirectory)
                    {
                        RequireDirectory(value);
                        if (frame.Depth == 8) throw new InvalidOperationException("learning_storage_depth_limit");
                        directoryCount = checked(directoryCount + 1);
                    }
                    else
                    {
                        if ((value.Mode & 0xf000) != 0x8000 || value.Owner != geteuid() || value.Links != 1
                            || (value.Mode & 0xfff) is not (0x100 or 0x140 or 0x180))
                            throw new InvalidOperationException("learning_storage_entry");
                        bytes = checked(bytes + checked((long)value.Size));
                        if (bytes > maxBytes) throw new InvalidOperationException("learning_storage_byte_limit");
                        fileCount = checked(fileCount + 1);
                    }
                    var stamp = Stamp.From(value);
                    frame.Observed.Add((name, stamp));
                    if (isDirectory)
                        Push(Handle(openat(frame.Directory, name, DirectoryFlag | NoFollow | CloseExec, 0)), frame.Depth + 1, stamp);
                }
                else
                {
                    foreach (var entry in frame.Observed)
                        if (Stamp.From(InspectAt(frame.Directory, entry.Name, false)!.Value) != entry.Before)
                            throw new InvalidOperationException("learning_storage_changed");
                    if (Stamp.From(Inspect(frame.Directory)) != frame.Before)
                        throw new InvalidOperationException("learning_storage_changed");
                    stack.Pop().Dispose();
                }
            }
            files.AssertPathIdentity();
            return new(bytes, fileCount, directoryCount);
        }
        catch (OverflowException) { throw new InvalidOperationException("learning_storage_byte_limit"); }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        { throw new InvalidOperationException("learning_storage_io_refused"); }
        finally { while (stack.Count != 0) stack.Pop().Dispose(); }

        void Push(SafeFileHandle directory, int depth, Stamp? expected)
        {
            try
            {
                var value = Inspect(directory);
                RequireDirectory(value);
                var before = Stamp.From(value);
                if (expected.HasValue && before != expected.Value) throw new InvalidOperationException("learning_storage_changed");
                stack.Push(new Frame(directory, before, depth));
            }
            catch { directory.Dispose(); throw; }
        }
    }

    private static void RequireDirectory(Stat value)
    {
        if ((value.Mode & 0xf000) != 0x4000 || value.Owner != geteuid() || (value.Mode & 0xfff) is not (0x140 or 0x1c0))
            throw new InvalidOperationException("learning_storage_entry");
    }
}
