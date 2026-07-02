using System;
using System.IO;
using BenchmarkDotNet.Attributes;
using BenchmarkDotNet.Running;
using CNET.Cce;

namespace CNET.Cce.Benchmarks;

[MemoryDiagnoser]
[SimpleJob(launchCount: 1, warmupCount: 2, iterationCount: 5)]
public class CceApiBenchmarks
{
    private float[] _inputs = null!;
    private float[] _targets = null!;
    private CceDataset? _dataset;
    private CceForest? _forest;
    private CceModel? _model;
    private CceScheduler? _scheduler;
    private string? _forestPath;

    [Params(256, 1024)]
    public int N { get; set; }

    [Params(8, 32)]
    public int BatchSize { get; set; }

    private const int InDim = 16;
    private const int OutDim = 4;

    [GlobalSetup]
    public void GlobalSetup()
    {
        var rnd = new Random(123);
        _inputs = new float[N * InDim];
        _targets = new float[N * OutDim];

        for (int i = 0; i < _inputs.Length; i++) _inputs[i] = (float)(rnd.NextDouble() * 2 - 1);
        for (int i = 0; i < N; i++)
        {
            int cls = i % OutDim;
            _targets[i * OutDim + cls] = 1f;
        }

        _dataset = CceDataset.FromArrays(_inputs, _targets, N, InDim, OutDim, BatchSize);

        _forestPath = Path.Combine(Path.GetTempPath(), $"cce_api_bench_{Environment.ProcessId}_{Guid.NewGuid():N}.cce");
        _forest = CceForest.Open(_forestPath, maxBranches: 4);
        _forest.AddLinearBranch("bench", inputDim: InDim, hiddenDim: 32, outputDim: OutDim);

        _model = new CceModel("bench-model");
        _model.Add(_forest, "bench");
        _model.SetDiffMode(CceDiffMode.Local);
        _scheduler = new CceScheduler(new CceSchedulerConfig(CceSchedulerType.Cosine, 0.01f));
        _model.SetScheduler(_scheduler);
    }

    [GlobalCleanup]
    public void GlobalCleanup()
    {
        _model?.Dispose();
        _forest?.Dispose();
        _dataset?.Dispose();
        _scheduler?.Dispose();
        if (_forestPath is not null && File.Exists(_forestPath))
        {
            try { File.Delete(_forestPath); }
            catch (IOException) { }
        }
    }

    [Benchmark]
    public void Dataset_NextBatch()
    {
        if (!_dataset!.NextBatch(out var b))
        {
            _dataset.Reset();
            _dataset.NextBatch(out b);
        }
    }

    [Benchmark]
    public double Model_TrainBatch()
    {
        _dataset!.Reset();
        _dataset!.NextBatch(out var b);
        return _model!.TrainBatch(b);
    }

    [Benchmark]
    public void Model_InferBatch()
    {
        _dataset!.Reset();
        _dataset!.NextBatch(out var b);
        var labels = new int[b.BatchSize];
        var confs = new float[b.BatchSize];
        _model!.InferBatch(b, labels, confs);
    }

    [Benchmark]
    public float Scheduler_GetLearningRate()
    {
        // Simulate a few epochs
        float sum = 0;
        for (int e = 0; e < 10; e++)
            sum += _scheduler!.GetLearningRate(e, 0.4f);
        return sum;
    }

    [Benchmark]
    public double Full_Epoch_Simulation()
    {
        // One full pass with small data - measures dispatch overhead end-to-end
        _dataset!.Reset();
        double loss = 0;
        int batches = 0;

        while (_dataset.NextBatch(out var b))
        {
            loss += _model!.TrainBatch(b);
            batches++;
        }
        return batches > 0 ? loss / batches : 0;
    }
}

/// <summary>
/// Benchmarks that highlight CCE strengths vs monolithic frameworks:
/// - Fast load of many small specialists (mmap + lazy)
/// - Low-overhead online adaptation
/// - CPU inference latency for embedded scenarios
/// </summary>
[MemoryDiagnoser]
public class CceNicheWinBenchmarks
{
    private CceForest? _forest;
    private string _tmpForest = "";

    [Params(4, 32)]
    public int NumSpecialists { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        _tmpForest = Path.Combine(Path.GetTempPath(), $"niche_bench_{Guid.NewGuid():N}.cce");
        if (File.Exists(_tmpForest)) File.Delete(_tmpForest);

        _forest = CceForest.Open(_tmpForest, Math.Max(64, NumSpecialists + 4));

        // Create many tiny specialists (the "many small" story)
        for (int i = 0; i < NumSpecialists; i++)
        {
            _forest.AddLinearBranch($"spec_{i}", inputDim: 8, hiddenDim: 8, outputDim: 4);
        }
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        _forest?.Dispose();
        if (File.Exists(_tmpForest)) File.Delete(_tmpForest);
    }

    [Benchmark]
    public int LoadAndCountBranches()
    {
        // Measures cost of opening a forest with N specialists (partial load advantage)
        using var f = CceForest.Open(_tmpForest, 128);
        return f.BranchCount;
    }

    [Benchmark]
    public int ManySpecialists_BranchEnumeration()
    {
        // Demonstrates low cost of working with "many small specialists"
        int sum = 0;
        for (int i = 0; i < _forest!.BranchCount; i++)
            sum += _forest.BranchName(i).Length;
        return sum;
    }
}

[MemoryDiagnoser]
[SimpleJob(launchCount: 1, warmupCount: 1, iterationCount: 5)]
public class CceInferenceLatencyBenchmarks
{
    private CceForest? _forest;
    private CceModel? _model;
    private float[] _input = null!;
    private string _tmp = "";

    [Params(1, 16)]
    public int NumSpecialists { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        _tmp = Path.Combine(Path.GetTempPath(), $"infer_lat_{Guid.NewGuid():N}.cce");
        if (File.Exists(_tmp)) File.Delete(_tmp);

        _forest = CceForest.Open(_tmp, 64);
        for (int i = 0; i < NumSpecialists; i++)
        {
            _forest.AddLinearBranch($"s{i}", 16, 16, 4);
        }
        _forest.Seal();  // enable zero-copy fast path

        _model = new CceModel("lat-model");
        _model.Add(_forest, "main");

        _input = new float[16];
        var r = new Random(42);
        for (int i = 0; i < _input.Length; i++) _input[i] = (float)(r.NextDouble() - 0.5);
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        _model?.Dispose();
        _forest?.Dispose();
        if (File.Exists(_tmp)) File.Delete(_tmp);
    }

    [Benchmark]
    public int SingleInfer_Latency()
    {
        var (label, conf) = _model!.Infer(_input);
        return label;
    }

    [Benchmark]
    public float[] Forward_Latency()
    {
        return _model!.Forward(_input, 4);
    }

    [Benchmark]
    public float[] BranchForward_ZeroCopy()
    {
        // Direct sealed branch forward - the fast path
        return _forest!.ForwardBranch(0, _input, 4);
    }
}

[MemoryDiagnoser]
public class CceAutogradBenchmarks
{
    private CceAutogradContext? _ag;
    private CceAutogradTensor? _x, _w, _tgt, _y, _loss;

    [GlobalSetup]
    public void Setup()
    {
        _ag = new CceAutogradContext(512);
        _x = _ag.CreateTensor(new float[8], 2, 4, false);
        _w = _ag.CreateTensor(new float[12], 4, 3, true);
        _tgt = _ag.CreateTensor(new float[6], 2, 3, false);

        _y = _ag.MatMul(_x, _w);
        _loss = _ag.MseLoss(_y, _tgt);
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        _ag?.Dispose();
    }

    [Benchmark]
    public void Autograd_ForwardOnly()
    {
        // Re-create minimal
        using var ag = new CceAutogradContext(128);
        var x = ag.CreateTensor(new float[8], 2, 4, false);
        var w = ag.CreateTensor(new float[12], 4, 3, true);
        var y = ag.MatMul(x, w);
        _ = ag.MseLoss(y, ag.CreateTensor(new float[6], 2, 3, false));
    }

    [Benchmark]
    public void Autograd_ForwardBackwardSgd()
    {
        using var ag = new CceAutogradContext(128);
        var x = ag.CreateTensor(new float[8], 2, 4, false);
        var w = ag.CreateTensor(new float[12], 4, 3, true);
        var tgt = ag.CreateTensor(new float[6], 2, 3, false);
        var y = ag.MatMul(x, w);
        var loss = ag.MseLoss(y, tgt);
        ag.Backward(loss);
        ag.SgdStep(new[] { w }, 0.01f);
    }
}

/// <summary>
/// Tiny pure-managed C# MLP reference ( "PyTorch style on CPU" without deps) for comparison.
/// Same small task as ag head benchmarks.
/// </summary>
public static class TinyReferenceMlp
{
    public static (double loss, long ms, long mem) TrainSmallHead(int steps = 10)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        long startMem = GC.GetTotalMemory(true);

        // 4 feat -> 3 class linear + relu + linear , mse
        float[] w1 = new float[4*8]; // feat->hidden
        float[] b1 = new float[8];
        float[] w2 = new float[8*3];
        float[] b2 = new float[3];
        var rnd = new Random(123);
        for (int i=0; i<w1.Length; i++) w1[i] = (float)(rnd.NextDouble()-0.5)*0.1f;
        // ... init others zero for simplicity

        float[] x = {0.1f,0.2f,-0.1f,0.3f}; // one sample
        float[] t = {0f,1f,0f};

        double lastLoss = 0;
        for (int s=0; s<steps; s++) {
            // forward
            float[] h = new float[8];
            for (int j=0; j<8; j++) {
                h[j] = 0;
                for (int i=0; i<4; i++) h[j] += x[i]*w1[i*8+j];
                h[j] += b1[j];
                h[j] = Math.Max(0, h[j]); // relu
            }
            float[] o = new float[3];
            for (int j=0; j<3; j++) {
                o[j] = 0;
                for (int i=0; i<8; i++) o[j] += h[i]*w2[i*3+j];
                o[j] += b2[j];
            }
            double loss = 0;
            for (int j=0; j<3; j++) { double d = o[j]-t[j]; loss += d*d; }
            lastLoss = loss / 3;

            // dummy backward (sgd on w2)
            float lr = 0.01f;
            for (int j=0; j<3; j++) {
                for (int i=0; i<8; i++) {
                    w2[i*3+j] -= lr * (o[j]-t[j]) * h[i];
                }
                b2[j] -= lr * (o[j]-t[j]);
            }
        }
        sw.Stop();
        long endMem = GC.GetTotalMemory(true);
        return (lastLoss, sw.ElapsedMilliseconds, endMem - startMem);
    }
}

public class Program
{
    public static void Main(string[] args)
    {
        Console.WriteLine("CCE .NET 10 Benchmarks");
        Console.WriteLine("Make sure cce.dll is discoverable (copied to output or in PATH).");
        AddNativeSearchPath();

        // Run micro-benchmarks
        BenchmarkRunner.Run<CceApiBenchmarks>();

        // Additionally run a small "real task" quality + throughput report.
        // This serves as the starting point for external PyTorch-style baseline comparisons.
        Console.WriteLine();
        Console.WriteLine("=== CCE End-to-End Training Report (synthetic blobs, for baseline comparison) ===");
        RunQualityThroughputReport();

        // Fast direct ag head demo (canonical frozen features + ag head, no full model overhead)
        {
            using var ag = new CceAutogradContext(32);
            var x = ag.CreateTensor(new float[4], 1, 4, false); // "features" from frozen CCE
            var w = ag.CreateTensor(new float[8], 4, 2, true);
            var b = ag.CreateTensor(new float[2], 1, 2, true);
            var tgt = ag.CreateTensor(new float[2], 1, 2, false);
            double start = 0;
            for (int i = 0; i < 3; i++) {
                var y = ag.AddBias(ag.MatMul(x, w), b);
                var l = ag.MseLoss(y, tgt);
                if (i == 0) start = l.GetData()[0];
                ag.Backward(l);
                ag.SgdStep(new[] { w, b }, 0.1f);
                ag.ZeroGrad();
            }
            Console.WriteLine($"Autograd head (direct): started ~{start:F2}, trained 3 steps with tape only for head");
        }

        // Demonstrate the canonical "frozen CCE feature + ag exact head"
        Console.WriteLine();
        Console.WriteLine("=== Autograd exact head (UseAutogradExactTail) on CCE features ===");
        string agHeadForest = Path.Combine(Path.GetTempPath(), $"ag_head_demo_{Guid.NewGuid():N}.cce");
        try
        {
            using var agF = CceForest.Open(agHeadForest, 4);
            agF.AddLinearBranch("feat_head", 8, 16, 4);  // simple linear as "head" for demo
            using var agM = new CceModel("ag-head");
            agM.Add(agF, "f");
            agM.SetDiffMode(CceDiffMode.Exact);
            var agCfg = new CceTrainingConfig
            {
                MaxEpochs = 1,
                DiffMode = CceDiffMode.Exact,
                UseAutogradExactTail = true,
                Loss = CceLossType.CrossEntropy
            };
            using var agDs = CceDataset.FromGaussianBlobs(16, 8, 4, 8, seed: 99);
            var swA = System.Diagnostics.Stopwatch.StartNew();
            var h = agM.FitHistory(agDs, agCfg);
            swA.Stop();
            Console.WriteLine($"Ag exact head: finalLoss={h.FinalLoss:F4}, time={swA.ElapsedMilliseconds}ms");
            Console.WriteLine("Note vs TF: CCE ag is narrow (exact tails only); TF has full autograd breadth + distributed/XLA/production scale for large programs.");
        }
        finally
        {
            if (File.Exists(agHeadForest)) File.Delete(agHeadForest);
        }

        Console.WriteLine();
        Console.WriteLine("=== Tiny ag head vs pure C# reference MLP (CPU) ===");
        var agRef = TinyReferenceMlp.TrainSmallHead(20);
        Console.WriteLine($"Reference C# head: loss={agRef.loss:F4}, time={agRef.ms}ms, deltaMem={agRef.mem}B");
    }

    /// <summary>
    /// Runs a small but realistic training job and prints key numbers.
    /// Compare wall-time / final accuracy / loss curve to an equivalent PyTorch (or TorchSharp) script
    /// using similar width/depth + Adam vs CCE Local/Hybrid.
    /// 
    /// CCE's value is not in raw throughput on a single head (it will usually lose),
    /// but in the ability to compose many small frozen specialists with contracts + router.
    /// </summary>
    public static void RunQualityThroughputReport(int nSamples = 4096, int inDim = 16, int classes = 6, int epochs = 12)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();

        using var dataset = CceDataset.FromGaussianBlobs(nSamples, inDim, classes, batchSize: 64, seed: 2026);

        string forestPath = Path.Combine(Path.GetTempPath(), $"cce_quality_bench_{Environment.ProcessId}_{Guid.NewGuid():N}.cce");
        using var forest = CceForest.Open(forestPath, maxBranches: 4);
        forest.AddLinearBranch("blobs", inputDim: inDim, hiddenDim: 32, outputDim: classes);

        using var model = new CceModel("bench-report");
        model.Add(forest, "blobs");
        model.SetDiffMode(CceDiffMode.Hybrid);

        var sched = new CceScheduler(new CceSchedulerConfig(CceSchedulerType.Cosine, 0.015f, WarmupEpochs: 2));
        model.SetScheduler(sched);

        var history = model.FitHistory(dataset, new CceTrainingConfig
        {
            MaxEpochs = epochs,
            TargetLoss = 1e-4f,
            Loss = CceLossType.CrossEntropy,
            DiffMode = CceDiffMode.Hybrid
        });

        sw.Stop();

        float finalAcc = CceMetrics.Accuracy(model, dataset);
        double finalLoss = history.FinalLoss;

        Console.WriteLine($"Samples={nSamples}  InDim={inDim}  Classes={classes}  Epochs={epochs}");
        Console.WriteLine($"Elapsed: {sw.Elapsed.TotalMilliseconds:F0} ms");
        Console.WriteLine($"Final loss: {finalLoss:F5}");
        Console.WriteLine($"Train accuracy (full pass): {finalAcc:P2}");
        Console.WriteLine($"Epochs/sec (avg): {(epochs / sw.Elapsed.TotalSeconds):F2}");
        Console.WriteLine();
        Console.WriteLine("PyTorch-style baseline comparison notes:");
        Console.WriteLine("  - Create an equivalent small MLP (Linear->ReLU->Linear) + CrossEntropy + Adam/Cosine.");
        Console.WriteLine("  - Measure same metrics on same synthetic generator (fixed seed).");
        Console.WriteLine("  - Record: wall time, final acc, loss@epoch curves, memory.");
        Console.WriteLine("  - CCE shines when you *compose* many such small frozen specialists instead of one large net.");

    }

    private static void AddNativeSearchPath()
    {
        string current = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
        AddIfNativeExists(Environment.CurrentDirectory, ref current);
        AddIfNativeExists(AppContext.BaseDirectory, ref current);
        AddIfNativeExists(Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..")), ref current);
        Environment.SetEnvironmentVariable("PATH", current);
    }

    private static void AddIfNativeExists(string directory, ref string path)
    {
        if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) return;
        if (!File.Exists(Path.Combine(directory, "cce.dll")) &&
            !File.Exists(Path.Combine(directory, "cce.so")) &&
            !File.Exists(Path.Combine(directory, "cce.dylib")))
            return;

        string normalized = Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (path.Contains(normalized, StringComparison.OrdinalIgnoreCase)) return;
        path = normalized + Path.PathSeparator + path;
    }
}
