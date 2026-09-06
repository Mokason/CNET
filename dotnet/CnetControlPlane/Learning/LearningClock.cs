using System.Runtime.InteropServices;

namespace CnetControlPlane.Learning;

internal readonly record struct LearningInstant(string Boot, long Nanoseconds);
internal interface ILearningClock { LearningInstant Now { get; } }

internal sealed class LearningClock : ILearningClock
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Timespec { public long Seconds; public long Nanoseconds; }
    [DllImport("libc", SetLastError = true)] private static extern int clock_gettime(int clock, out Timespec time);
    private readonly string boot;
    public LearningClock()
    {
        LearningLinux.RequireSupported();
        boot = File.ReadAllText("/proc/sys/kernel/random/boot_id").TrimEnd('\n');
        if (!Guid.TryParseExact(boot, "D", out _)) throw new InvalidOperationException("learning_boot_identity_invalid");
    }
    public LearningInstant Now
    {
        get
        {
            // CLOCK_BOOTTIME includes suspend. UTC is never a budget authority.
            if (clock_gettime(7, out var time) != 0 || time.Seconds < 0 || time.Nanoseconds is < 0 or >= 1_000_000_000)
                throw new InvalidOperationException("learning_boot_clock_unavailable");
            return new(boot, checked(time.Seconds * 1_000_000_000 + time.Nanoseconds));
        }
    }
}
