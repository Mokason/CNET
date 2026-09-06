using Xunit;

namespace CnetControlPlane.Tests;

// /proc/self/fd is process-wide. These exact leak assertions must not overlap
// sibling tests that legitimately open or close native processes and sockets.
// Keep the equality assertions intact; do not hide growth with a tolerance.
[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class LearningResourceCollection
{
    public const string Name = "Learning process resource measurements";
}
