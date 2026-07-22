using CNET.Llm.Models.Gguf;
using CNET.Llm.Tests.Integration.Fixtures;
using Xunit;

namespace CNET.Llm.Tests.Integration.Models.Gguf;

/// <summary>
/// GGUF parsing tests against a small auto-downloaded model (bge-small-en-v1.5, ~36 MB).
/// Validates header, metadata, and tensor parsing against a real GGUF file.
/// </summary>
[Collection("SmallModel")]
public class GgufParsingTests
{
    private readonly SmallModelFixture _fixture;

    public GgufParsingTests(SmallModelFixture fixture)
    {
        _fixture = fixture;
    }

    [Fact]
    public void Open_ReadsValidHeader()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        Assert.True(gguf.Header.Version is 2 or 3,
            $"Unexpected GGUF version: {gguf.Header.Version}");
        Assert.True(gguf.Header.TensorCount > 0, "Expected at least one tensor.");
        Assert.True(gguf.Header.MetadataKvCount > 0, "Expected at least one metadata entry.");
    }

    [Fact]
    public void Open_MetadataContainsArchitecture()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        Assert.True(gguf.Metadata.ContainsKey("general.architecture"),
            "Metadata should contain 'general.architecture'.");

        string arch = gguf.Metadata.GetString("general.architecture");
        Assert.False(string.IsNullOrEmpty(arch));
    }

    [Fact]
    public void Open_MetadataCountMatchesHeader()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        Assert.Equal((int)gguf.Header.MetadataKvCount, gguf.Metadata.Count);
    }

    [Fact]
    public void Open_TensorCountMatchesHeader()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        Assert.Equal((int)gguf.Header.TensorCount, gguf.Tensors.Count);
        Assert.Equal(gguf.Tensors.Count, gguf.TensorsByName.Count);
    }

    [Fact]
    public void Open_AllTensorsHaveValidDescriptors()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        foreach (var tensor in gguf.Tensors)
        {
            Assert.False(string.IsNullOrEmpty(tensor.Name), "Tensor name should not be empty.");
            Assert.True(tensor.Shape.Rank > 0, $"Tensor '{tensor.Name}' has rank 0.");
            Assert.True(tensor.Shape.ElementCount > 0, $"Tensor '{tensor.Name}' has 0 elements.");
        }
    }

    [Fact]
    public void Open_TensorsByNameLookupWorks()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        foreach (var tensor in gguf.Tensors)
        {
            Assert.True(gguf.TensorsByName.ContainsKey(tensor.Name),
                $"Tensor '{tensor.Name}' missing from TensorsByName dictionary.");
            Assert.Equal(tensor, gguf.TensorsByName[tensor.Name]);
        }
    }

    [Fact]
    public void Open_DataSectionOffsetIsAligned()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        // Default alignment is 32 bytes (may be overridden by general.alignment).
        uint alignment = gguf.Metadata.GetUInt32OrDefault("general.alignment", 32);
        Assert.Equal(0, gguf.DataSectionOffset % alignment);
    }

    [Fact]
    public void Open_DataBasePointerIsNonZero()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        if (gguf.Header.TensorCount > 0)
            Assert.NotEqual(nint.Zero, gguf.DataBasePointer);
    }

    [Fact]
    public void Open_FileSizeExceedsDataSectionOffset()
    {
        using var gguf = GgufFile.Open(_fixture.FilePath);

        long fileSize = new FileInfo(_fixture.FilePath).Length;
        Assert.True(fileSize > gguf.DataSectionOffset,
            "File size should exceed the data section offset.");
    }
}
