using Microsoft.Win32.SafeHandles;
using static CnetControlPlane.Learning.LearningLinux;

namespace CnetControlPlane.Learning;

internal sealed class LearningFiles : IDisposable
{
    private readonly SafeFileHandle directory;
    // File helpers retain descriptor authority. Pathname-only APIs such as
    // SQLite must call AssertPathIdentity before use; live-root rename refuses.
    internal string FullPath { get; }
    private LearningFiles(string path, SafeFileHandle fd) { FullPath = path; directory = fd; }

    public static LearningFiles Open(string path)
    {
        RequireSupported();
        if (string.IsNullOrEmpty(path) || path[0] != '/' || path.Length > 4095 || path.Contains('\0')
            || path == "/" || path.Split('/').Skip(1).Any(p => p.Length == 0 || p is "." or ".."))
            throw new ArgumentException("learning_absolute_canonical_path_required");
        var fd = Handle(open("/", DirectoryFlag | CloseExec, 0));
        try
        {
            foreach (var part in path.Split('/').Skip(1))
            {
                var parent = Inspect(fd);
                if ((parent.Owner != 0 && parent.Owner != geteuid())
                    || ((parent.Mode & 0x12) != 0 && (parent.Owner != 0 || (parent.Mode & 0x200) == 0)))
                    throw new InvalidOperationException("learning_untrusted_path_ancestor");
                var next = Handle(openat(fd, part, DirectoryFlag | NoFollow | CloseExec, 0));
                fd.Dispose(); fd = next;
            }
            RequirePrivateDirectory(fd);
            return new LearningFiles(path, fd);
        }
        catch { fd.Dispose(); throw; }
    }

    private static void RequirePrivateDirectory(SafeFileHandle fd)
    {
        var value = Inspect(fd);
        if ((value.Mode & 0xf000) != 0x4000 || value.Owner != geteuid() || (value.Mode & 0xfff) != 0x1c0)
            throw new InvalidOperationException("learning_private_directory_required");
    }

    internal static void Component(string name)
    {
        if (string.IsNullOrEmpty(name) || name.Length > 96 || name is "." or ".."
            || !name.All(c => c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '_' or '-' or '.'))
            throw new ArgumentException("learning_file_component_required");
    }
    internal SafeFileHandle OpenFile(string name, bool write = false, bool createNew = false)
    {
        Component(name);
        var fd = Handle(openat(directory, name, NoFollow | NonBlock | CloseExec | (write ? ReadWrite : 0)
            | (createNew ? Create | Exclusive : 0), 0x180));
        try
        {
            RequirePrivateFile(Inspect(fd));
            return fd;
        }
        catch { fd.Dispose(); throw; }
    }
    private static void RequirePrivateFile(Stat value)
    {
        if ((value.Mode & 0xf000) != 0x8000 || value.Owner != geteuid()
            || (value.Mode & 0xfff) is not (0x100 or 0x180) || value.Links != 1)
            throw new InvalidOperationException("learning_private_single_link_file_required");
    }
    internal Stat? ValidateFile(string name, bool allowMissing = false)
    {
        Component(name);
        var value = InspectAt(directory, name, allowMissing);
        if (value.HasValue) RequirePrivateFile(value.Value);
        return value;
    }
    // SQLite canonicalizes /proc/self/fd paths. It must not continue after the
    // trusted owner renames/replaces a live root; this is not hostile-owner isolation.
    internal void AssertPathIdentity()
    {
        using var current = Open(FullPath);
        var retained = Inspect(directory);
        var fresh = Inspect(current.directory);
        if (retained.Inode != fresh.Inode || retained.DeviceMajor != fresh.DeviceMajor || retained.DeviceMinor != fresh.DeviceMinor)
            throw new InvalidOperationException("learning_root_path_identity_changed");
    }
    public byte[] Read(string name, int maximumBytes)
    {
        if (maximumBytes is < 1 or > 64 * 1024 * 1024) throw new ArgumentException("learning_file_size_limit");
        using var fd = OpenFile(name);
        var before = Inspect(fd);
        if (before.Size > (ulong)maximumBytes) throw new InvalidOperationException("learning_file_too_large");
        var bytes = new byte[(int)before.Size];
        var position = 0;
        while (position < bytes.Length)
        {
            var n = RandomAccess.Read(fd, bytes.AsSpan(position), position);
            if (n == 0) throw new InvalidOperationException("learning_file_changed");
            position += n;
        }
        Span<byte> extra = stackalloc byte[1];
        if (RandomAccess.Read(fd, extra, position) != 0 || !before.Equals(Inspect(fd)))
            throw new InvalidOperationException("learning_file_changed");
        return bytes;
    }
    // Publication is exclusive. A failed directory fsync after rename leaves
    // the named artifact intact and reports uncertainty; callers must reconcile.
    public void WriteNew(string name, ReadOnlySpan<byte> bytes)
    {
        Component(name);
        if (bytes.Length > 64 * 1024 * 1024) throw new ArgumentException("learning_file_size_limit");
        var pending = ".pending_" + Guid.NewGuid().ToString("N");
        var published = false;
        try
        {
            using (var fd = OpenFile(pending, write: true, createNew: true))
            {
                RandomAccess.Write(fd, bytes, 0);
                Sync(fd);
            }
            if (renameat2(directory, pending, directory, name, 1) != 0)
                throw new InvalidOperationException("learning_exclusive_publication_refused");
            published = true;
            Sync(directory);
        }
        finally
        {
            if (!published) _ = unlinkat(directory, pending, 0);
        }
    }
    public LearningFiles CreateDirectory(string name)
    {
        Component(name);
        if (mkdirat(directory, name, 0x1c0) != 0) throw new InvalidOperationException("learning_new_directory_required");
        Sync(directory);
        var child = Handle(openat(directory, name, DirectoryFlag | NoFollow | CloseExec, 0));
        try
        {
            RequirePrivateDirectory(child);
            return new LearningFiles(Path.Combine(FullPath, name), child);
        }
        catch { child.Dispose(); throw; }
    }
    public SafeFileHandle AcquireLock(string name)
    {
        var fd = OpenFile(name, write: true);
        if (flock(fd, 2 | 4) == 0) return fd; // LOCK_EX | LOCK_NB
        fd.Dispose();
        throw new InvalidOperationException("learning_owner_already_running");
    }
    public void Dispose() => directory.Dispose();
}
