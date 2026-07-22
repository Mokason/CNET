using BenchmarkDotNet.Attributes;
using CNET.Llm.HuggingFace;
using CNET.Llm.Models.Gguf;

namespace CNET.Llm.Benchmarks;

/// <summary>
/// Benchmarks GGUF loading stages against a real model (SmolLM-135M Q8_0, ~145 MB).
/// Auto-downloads on first run, cached in <c>~/.cnet-llm/test-cache/</c>.
/// </summary>
[MemoryDiagnoser]
[SimpleJob(warmupCount: 3, iterationCount: 10)]
public class GgufRealModelBenchmarks
{
    private const string RepoId = "QuantFactory/SmolLM-135M-GGUF";
    private const string Filename = "SmolLM-135M.Q8_0.gguf";

    private string _modelPath = null!;

    [GlobalSetup]
    public void Setup()
    {
        string cacheDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            ".cnet-llm", "test-cache");

        string cachedPath = Path.Combine(cacheDir, RepoId.Replace('/', Path.DirectorySeparatorChar), Filename);

        if (File.Exists(cachedPath))
        {
            _modelPath = cachedPath;
            return;
        }

        Console.WriteLine($"Downloading {RepoId}/{Filename} (~145 MB)...");
        using var downloader = new HuggingFaceDownloader();
        _modelPath = downloader.DownloadFileAsync(RepoId, Filename, cacheDir).GetAwaiter().GetResult();
    }

    [Benchmark(Description = "Full GgufFile.Open (real model)")]
    public void FullLoad()
    {
        using var file = GgufFile.Open(_modelPath);
    }

    [Benchmark(Description = "Header + Metadata (real model)")]
    public GgufMetadata ReadHeaderAndMetadata()
    {
        using var fs = new FileStream(_modelPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var reader = new BinaryReader(fs);
        var header = GgufReader.ReadHeader(reader);
        var raw = GgufReader.ReadMetadata(reader, header);
        return new GgufMetadata(raw);
    }

    [Benchmark(Description = "Header + Metadata + TensorInfos (real model)")]
    public List<GgufTensorDescriptor> ReadAllInfos()
    {
        using var fs = new FileStream(_modelPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var reader = new BinaryReader(fs);
        var header = GgufReader.ReadHeader(reader);
        _ = GgufReader.ReadMetadata(reader, header);
        return GgufReader.ReadTensorInfos(reader, header);
    }

    [Benchmark(Description = "ModelConfig extraction (real model)")]
    public Core.Models.ModelConfig ExtractConfig()
    {
        using var file = GgufFile.Open(_modelPath);
        return GgufModelConfigExtractor.Extract(file.Metadata);
    }
}
