using Microsoft.Win32.SafeHandles;
using static CnetControlPlane.Learning.LearningLinux;

namespace CnetControlPlane.Learning;

// Takes ownership of an already validated private descriptor, even on failure.
internal sealed class LearningOwnerLock : IDisposable
{
    private SafeFileHandle? descriptor;
    internal LearningOwnerLock(SafeFileHandle handle)
    {
        try
        {
            if (flock(handle, 2 | 4) != 0) // LOCK_EX | LOCK_NB
                throw new InvalidOperationException("learning_owner_already_running");
            descriptor = handle;
        }
        catch { handle.Dispose(); throw; }
    }
    public void Dispose()
    {
        var retained = Interlocked.Exchange(ref descriptor, null);
        if (retained is null) return;
        try
        {
            // Close-on-exec does not close a concurrent fork's pre-exec copy.
            // flock belongs to that shared open-file description: close alone
            // can retain ownership after this lease ends. Release it explicitly.
            if (flock(retained, 8) != 0) // LOCK_UN
                throw new InvalidOperationException("learning_owner_unlock_failed");
        }
        finally { retained.Dispose(); }
    }
}
