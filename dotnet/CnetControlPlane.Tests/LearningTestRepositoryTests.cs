using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningTestRepositoryTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-test-repository-").FullName;
    public void Dispose() => Directory.Delete(root, recursive: true);
    private string Repository(string name, params string[] nativeNames)
    {
        var path = Path.Combine(root, name);
        Directory.CreateDirectory(Path.Combine(path, "dotnet", "CnetControlPlane.Tests"));
        Directory.CreateDirectory(Path.Combine(path, "bin"));
        File.WriteAllText(Path.Combine(path, "dotnet", "CnetControlPlane.Tests", "CnetControlPlane.Tests.csproj"), "");
        foreach (var native in nativeNames) File.WriteAllText(Path.Combine(path, "bin", native), "");
        return path;
    }
    private string Caller(string repo) => Path.Combine(repo, "dotnet", "CnetControlPlane.Tests", "Fixture.cs");

    [Theory]
    [InlineData("cwd")]
    [InlineData("base")]
    [InlineData("caller")]
    public void FindsAncestorsFromEachExplicitOriginWithoutChangingProcessCwd(string origin)
    {
        var cwd = Directory.GetCurrentDirectory();
        var repo = Repository("checkout", "cnetd", "libcnet_capsule_core.so");
        var nested = Path.Combine(repo, "dotnet", "CnetControlPlane.Tests", "bin");
        Directory.CreateDirectory(nested);
        var result = LearningTestRepository.RequireBuiltFrom(["cnetd", "libcnet_capsule_core.so"],
            origin == "cwd" ? nested : root, origin == "base" ? nested : root,
            origin == "caller" ? Caller(repo) : Path.Combine(root, "Fixture.cs"));
        Assert.Equal(repo, result);
        Assert.Equal(cwd, Directory.GetCurrentDirectory());
    }

    [Fact]
    public void CurrentCheckoutPrecedesBaseAndCallerFallbacks()
    {
        var current = Repository("current", "cnetd");
        var other = Repository("other", "cnetd");
        Assert.Equal(current, LearningTestRepository.RequireBuiltFrom(["cnetd"], current, other, Caller(other)));
    }

    [Fact]
    public void MissingArtifactInSelectedCheckoutCannotBorrowFromAnotherCheckout()
    {
        var current = Repository("current", "cnetd");
        var other = Repository("other", "cnetd", "libcnet_capsule_core.so");
        Assert.Equal("learning_test_native_prerequisite_missing: libcnet_capsule_core.so",
            Assert.Throws<InvalidOperationException>(() => LearningTestRepository.RequireBuiltFrom(
                ["cnetd", "libcnet_capsule_core.so"], current, other, Caller(other))).Message);
    }

    [Fact]
    public void BinaryNamesAloneWithoutTheTestProjectMarkerAreNotACheckout()
    {
        Directory.CreateDirectory(Path.Combine(root, "bin"));
        File.WriteAllText(Path.Combine(root, "bin", "cnetd"), "");
        Assert.Equal("learning_test_repository_not_found", Assert.Throws<InvalidOperationException>(() =>
            LearningTestRepository.RequireBuiltFrom(["cnetd"], root, root, Path.Combine(root, "Fixture.cs"))).Message);
    }

    [Fact]
    public void MissingOriginsRefuseWithoutCreatingDirectories()
    {
        var absent = Path.Combine(root, "absent");
        Assert.Equal("learning_test_repository_not_found", Assert.Throws<InvalidOperationException>(() =>
            LearningTestRepository.RequireBuiltFrom(["cnetd"], absent, absent, Path.Combine(absent, "Fixture.cs"))).Message);
        Assert.Empty(Directory.EnumerateFileSystemEntries(root));
    }

    [Theory]
    [InlineData("")]
    [InlineData("../cnetd")]
    [InlineData("/usr/bin/true")]
    [InlineData("unapproved_binary")]
    public void RequiredArtifactsAreOnlyTheFixedNativeNames(string name)
    {
        Assert.Equal("learning_test_native_names", Assert.Throws<ArgumentException>(() =>
            LearningTestRepository.RequireBuiltFrom([name], root, root, Path.Combine(root, "Fixture.cs"))).Message);
        Assert.Empty(Directory.EnumerateFileSystemEntries(root));
    }

    [Fact]
    public void EmptyRequirementsCannotMistakeAnUnbuiltCheckoutForAReadyFixture()
    {
        Assert.Equal("learning_test_native_names", Assert.Throws<ArgumentException>(() =>
            LearningTestRepository.RequireBuiltFrom([], root, root, Path.Combine(root, "Fixture.cs"))).Message);
    }
}
