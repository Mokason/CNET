using System.Text;
using BenchmarkDotNet.Attributes;
using CNET.Llm.Models.Gguf;

namespace CNET.Llm.Benchmarks;

/// <summary>
/// Benchmarks for GGUF file parsing stages using synthetic GGUF data.
/// For real model benchmarks, see <see cref="GgufRealModelBenchmarks"/>.
/// </summary>
[MemoryDiagnoser]
[SimpleJob(warmupCount: 3, iterationCount: 10)]
public class GgufLoadingBenchmarks
{
    private byte[] _syntheticData = null!;
    private string _syntheticFilePath = null!;

    [Params(10, 100)]
    public int MetadataCount { get; set; }

    [Params(50, 500)]
    public int TensorCount { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        _syntheticData = BuildSyntheticGguf(MetadataCount, TensorCount);
        _syntheticFilePath = Path.Combine(Path.GetTempPath(), $"cnet_llm_bench_{Guid.NewGuid():N}.gguf");
        File.WriteAllBytes(_syntheticFilePath, _syntheticData);
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        if (File.Exists(_syntheticFilePath))
            File.Delete(_syntheticFilePath);
    }

    [Benchmark]
    public GgufHeader ReadHeader()
    {
        using var stream = new MemoryStream(_syntheticData);
        using var reader = new BinaryReader(stream);
        return GgufReader.ReadHeader(reader);
    }

    [Benchmark]
    public Dictionary<string, GgufMetadataValue> ReadMetadata()
    {
        using var stream = new MemoryStream(_syntheticData);
        using var reader = new BinaryReader(stream);
        var header = GgufReader.ReadHeader(reader);
        return GgufReader.ReadMetadata(reader, header);
    }

    [Benchmark]
    public List<GgufTensorDescriptor> ReadTensorInfos()
    {
        using var stream = new MemoryStream(_syntheticData);
        using var reader = new BinaryReader(stream);
        var header = GgufReader.ReadHeader(reader);
        _ = GgufReader.ReadMetadata(reader, header);
        return GgufReader.ReadTensorInfos(reader, header);
    }

    [Benchmark]
    public void FullLoad_Synthetic()
    {
        using var file = GgufFile.Open(_syntheticFilePath);
    }

    private static byte[] BuildSyntheticGguf(int metadataCount, int tensorCount)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);

        // Header: magic, version 3, tensor count, metadata count
        writer.Write(GgufReader.GgufMagic);
        writer.Write((uint)3);
        writer.Write((ulong)tensorCount);
        writer.Write((ulong)metadataCount);

        // Metadata KV pairs
        for (int i = 0; i < metadataCount; i++)
        {
            WriteString(writer, $"meta.key.{i}");
            writer.Write((uint)GgufValueType.String); // type = String
            WriteString(writer, $"value_{i}_some_typical_metadata_content");
        }

        // Tensor infos
        ulong offset = 0;
        const int tensorDataSize = 256;
        for (int i = 0; i < tensorCount; i++)
        {
            WriteString(writer, $"blk.{i / 10}.attn.weight.{i % 10}");
            writer.Write((uint)2); // nDims = 2
            writer.Write((ulong)128);
            writer.Write((ulong)128);
            writer.Write((uint)0); // F32
            writer.Write(offset);
            offset += tensorDataSize;
        }

        writer.Flush();

        // Align to 32 bytes
        long dataStart = AlignUp(stream.Position, 32);
        while (stream.Position < dataStart)
            writer.Write((byte)0);

        // Tensor data (zeros)
        var tensorData = new byte[tensorCount * tensorDataSize];
        writer.Write(tensorData);

        writer.Flush();
        return stream.ToArray();
    }

    private static void WriteString(BinaryWriter writer, string value)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        writer.Write((ulong)bytes.Length);
        writer.Write(bytes);
    }

    private static long AlignUp(long value, uint alignment)
    {
        long mask = alignment - 1;
        return (value + mask) & ~mask;
    }
}
