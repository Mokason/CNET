using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace CnetControlPlane.Learning;

// Linux UAPI statx has a fixed 256-byte layout; do not marshal architecture-
// dependent libc stat. This supervisor explicitly supports Linux x86-64 only.
internal static class LearningLinux
{
    internal const int ReadWrite = 2, Create = 0x40, Exclusive = 0x80,
        NonBlock = 0x800, DirectoryFlag = 0x10000, NoFollow = 0x20000, CloseExec = 0x80000;
    [StructLayout(LayoutKind.Explicit, Size = 256)]
    internal struct Stat
    {
        [FieldOffset(0)] public uint Mask;
        [FieldOffset(16)] public uint Links;
        [FieldOffset(20)] public uint Owner;
        [FieldOffset(28)] public ushort Mode;
        [FieldOffset(32)] public ulong Inode;
        [FieldOffset(40)] public ulong Size;
        [FieldOffset(96)] public long ChangeSeconds;
        [FieldOffset(104)] public uint ChangeNanos;
        [FieldOffset(112)] public long ModifySeconds;
        [FieldOffset(120)] public uint ModifyNanos;
        [FieldOffset(136)] public uint DeviceMajor;
        [FieldOffset(140)] public uint DeviceMinor;
    }
    [DllImport("libc", SetLastError = true)] internal static extern int open(string path, int flags, uint mode);
    [DllImport("libc", SetLastError = true)] internal static extern int openat(SafeFileHandle dir, string path, int flags, uint mode);
    [DllImport("libc", SetLastError = true)] private static extern int statx(SafeFileHandle fd, string path, int flags, uint mask, out Stat value);
    [DllImport("libc", SetLastError = true)] internal static extern int fsync(SafeFileHandle fd);
    [DllImport("libc", SetLastError = true)] internal static extern int mkdirat(SafeFileHandle dir, string path, uint mode);
    [DllImport("libc", SetLastError = true)] internal static extern int unlinkat(SafeFileHandle dir, string path, int flags);
    [DllImport("libc", SetLastError = true)] internal static extern int renameat2(SafeFileHandle olddir, string oldpath, SafeFileHandle newdir, string newpath, uint flags);
    [DllImport("libc", SetLastError = true)] internal static extern int flock(SafeFileHandle fd, int operation);
    [DllImport("libc")] internal static extern uint geteuid();
    [DllImport("libc")] internal static extern uint getuid();

    internal static void RequireSupported()
    {
        if (!OperatingSystem.IsLinux() || RuntimeInformation.ProcessArchitecture != Architecture.X64
            || geteuid() == 0 || geteuid() != getuid())
            throw new InvalidOperationException("learning_linux_unprivileged_x64_required");
    }
    internal static SafeFileHandle Handle(int fd) => fd >= 0 ? new SafeFileHandle((IntPtr)fd, true)
        : throw new InvalidOperationException("learning_file_open_refused");
    internal static Stat Inspect(SafeFileHandle fd)
    {
        const uint required = 0x3cf; // type, mode, nlink, uid, mtime, ctime, inode, size
        if (statx(fd, "", 0x1000, required, out var value) != 0 || (value.Mask & required) != required)
            throw new InvalidOperationException("learning_file_stat_refused");
        return value;
    }
    internal static Stat? InspectAt(SafeFileHandle directory, string name, bool allowMissing)
    {
        const uint required = 0x3cf;
        // AT_SYMLINK_NOFOLLOW: inspect the fixed directory entry without
        // opening or closing its inode (which could release SQLite POSIX locks).
        if (statx(directory, name, 0x100, required, out var value) != 0)
        {
            if (allowMissing && Marshal.GetLastPInvokeError() == 2) return null; // ENOENT only
            throw new InvalidOperationException("learning_file_stat_refused");
        }
        if ((value.Mask & required) != required)
            throw new InvalidOperationException("learning_file_stat_refused");
        return value;
    }
    internal static void Sync(SafeFileHandle fd)
    {
        if (fsync(fd) != 0) throw new InvalidOperationException("learning_file_sync_uncertain");
    }
}
