using System;
using System.Collections.Generic;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class TrainingExtrasTests
{
    [Fact]
    public void Fit_Invokes_OnEpochEnd_Each_Epoch()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-callback");   // empty model: TrainBatch returns -1 but does not throw

        var seen = new List<int>();
        var callbacks = new CceCallbacks
        {
            OnEpochEnd = info => { seen.Add(info.Epoch); return true; }
        };

        model.Fit(ds, new CceTrainingConfig { MaxEpochs = 3, ShuffleEachEpoch = false }, callbacks);

        Assert.Equal(new[] { 0, 1, 2 }, seen.ToArray());
    }

    [Fact]
    public void Fit_EarlyStops_When_Callback_Returns_False()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-earlystop");

        int epochs = 0;
        var callbacks = new CceCallbacks
        {
            OnEpochEnd = _ => { epochs++; return false; }   // stop after first
        };

        model.Fit(ds, new CceTrainingConfig { MaxEpochs = 10, ShuffleEachEpoch = false }, callbacks);

        Assert.Equal(1, epochs);
    }

    [Fact]
    public void Fit_Invokes_OnBatchEnd_For_Each_Batch()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-batch-callback");

        var seen = new List<BatchInfo>();
        var callbacks = new CceCallbacks
        {
            OnBatchEnd = info => { seen.Add(info); return true; }
        };

        model.Fit(ds, new CceTrainingConfig { MaxEpochs = 2, ShuffleEachEpoch = false }, callbacks);

        Assert.Equal(4, seen.Count);
        Assert.Equal(0, seen[0].Epoch);
        Assert.Equal(0, seen[0].Batch);
        Assert.Equal(8, seen[0].BatchSize);
        Assert.Equal(1, seen[3].Epoch);
        Assert.Equal(1, seen[3].Batch);
    }

    [Fact]
    public void Fit_OnBatchEnd_Can_EarlyStop()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-batch-earlystop");

        int batches = 0;
        var callbacks = new CceCallbacks
        {
            OnBatchEnd = _ => { batches++; return false; }
        };

        model.Fit(ds, new CceTrainingConfig { MaxEpochs = 5, ShuffleEachEpoch = false }, callbacks);

        Assert.Equal(1, batches);
    }

    [Fact]
    public void FitHistory_Returns_PerEpoch_Records()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-history");

        CceTrainingHistory history = model.FitHistory(
            ds,
            new CceTrainingConfig { MaxEpochs = 3, ShuffleEachEpoch = false });

        Assert.Equal(3, history.EpochCount);
        Assert.Equal(3, history.Epochs.Count);
        Assert.Equal(6, history.Batches.Count);
        Assert.Equal(2, history.LastEpoch?.Epoch);
        Assert.Equal(1, history.LastBatch?.Batch);
        Assert.Equal(history.LastEpoch!.Value.TrainLoss, history.FinalLoss);
    }

    [Fact]
    public void FitHistory_Preserves_Callback_EarlyStop()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-history-stop");

        CceTrainingHistory history = model.FitHistory(
            ds,
            new CceTrainingConfig { MaxEpochs = 10, ShuffleEachEpoch = false },
            callbacks: new CceCallbacks { OnEpochEnd = _ => false });

        Assert.Equal(1, history.EpochCount);
        Assert.Equal(0, history.LastEpoch?.Epoch);
    }

    [Fact]
    public void FitHistory_Captures_ValidationLoss_When_ValidationSet_Is_Provided()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_val_loss_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 4);
            forest.AddLinearBranch("val-loss", inputDim: 4, hiddenDim: 6, outputDim: 2);
            using var model = new CceModel("fit-history-val-loss");
            model.Add(forest, "f");

            float[] inputs =
            {
                1f, 0f, 0f, 0f,
                0f, 1f, 0f, 0f,
            };
            int[] labels = { 0, 1 };
            using var ds = CceDataset.FromLabels(inputs, labels, 2, 4, 2, 2);

            CceTrainingHistory history = model.FitHistory(
                ds,
                new CceTrainingConfig { MaxEpochs = 1, ShuffleEachEpoch = false },
                validation: ds);

            Assert.NotNull(history.LastEpoch?.ValLoss);
            Assert.True(double.IsFinite(history.LastEpoch!.Value.ValLoss!.Value));
            Assert.True(history.LastEpoch.Value.ValLoss.Value >= 0);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void TrainingConfig_LossType_Defaults_To_MSE()
    {
        var cfg = new CceTrainingConfig();
        Assert.Equal(CceLossType.MeanSquaredError, cfg.Loss);

        var ce = cfg with { Loss = CceLossType.CrossEntropy };
        Assert.Equal(CceLossType.CrossEntropy, ce.Loss);
    }

    [Fact]
    public void TrainingPresets_Expose_Cce_Aligned_Defaults()
    {
        CceTrainingConfig local = CceTrainingPresets.LocalFast(maxEpochs: 12, initialLr: 0.02f);
        CceTrainingConfig hybrid = CceTrainingPresets.HybridQuality(maxEpochs: 20, initialLr: 0.015f);
        CceTrainingConfig exact = CceTrainingPresets.ExactFineTune(maxEpochs: 5, initialLr: 0.005f);

        Assert.Equal(CceDiffMode.Local, local.DiffMode);
        Assert.Equal(CceLossType.MeanSquaredError, local.Loss);
        Assert.Equal(12, local.MaxEpochs);
        Assert.NotNull(local.Scheduler);
        Assert.Equal(CceSchedulerType.Cosine, local.Scheduler!.Type);
        Assert.Equal(0.02f, local.Scheduler.InitialLr);

        Assert.Equal(CceDiffMode.Hybrid, hybrid.DiffMode);
        Assert.Equal(CceLossType.CrossEntropy, hybrid.Loss);
        Assert.NotNull(hybrid.Scheduler);
        Assert.Equal(CceSchedulerType.Cosine, hybrid.Scheduler!.Type);

        Assert.Equal(CceDiffMode.Exact, exact.DiffMode);
        Assert.Equal(CceLossType.CrossEntropy, exact.Loss);
        Assert.Equal(1.0f, exact.GradClip);
    }

    [Fact]
    public void EarlyStopping_Minimizes_ValidationLoss_With_Patience()
    {
        var earlyStopping = new CceEarlyStopping(
            patience: 2,
            minDelta: 0.01,
            monitor: CceEarlyStoppingMonitor.ValidationLoss);

        Assert.True(earlyStopping.OnEpochEnd(new EpochInfo(0, 1.0, null, 0f, 1.0)));
        Assert.True(earlyStopping.OnEpochEnd(new EpochInfo(1, 0.9, null, 0f, 0.995)));
        Assert.False(earlyStopping.OnEpochEnd(new EpochInfo(2, 0.8, null, 0f, 0.994)));
    }

    [Fact]
    public void CceDataLoader_And_Synthetics_Produce_Usable_Batches()
    {
        using var blobs = CceDataset.FromGaussianBlobs(128, 6, 3, batchSize: 16, seed: 7);
        using var loader = blobs.AsDataLoader(materialize: true);

        int count = 0;
        CceMaterializedBatch first = default;
        foreach (var mb in loader)
        {
            if (count == 0) first = mb;
            count++;
            Assert.Equal(16, mb.BatchSize);
            Assert.True(mb.Inputs.Length > 0);
        }
        Assert.True(count > 0);
        Assert.Equal(blobs.BatchSize, first.BatchSize);

        // Also exercise a regression synthetic
        using var reg = CceDataset.ForLinearRegression(64, 4, 2, 8);
        Assert.Equal(64, reg.SampleCount);
        Assert.Equal(2, reg.OutputDim);
    }

    [Fact]
    public void Additional_LossTypes_Are_Computable()
    {
        // Build tiny model + data so Forward works. Attach a minimal branch.
        using var ds = CceDataset.FromGaussianBlobs(32, 4, 2, 8, seed: 1);
        using var forest = CceForest.Open(System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"loss_test_{System.Guid.NewGuid():N}.cce"), 4);
        forest.AddLinearBranch("head", inputDim: 4, hiddenDim: 8, outputDim: 2);
        using var m = new CceModel("loss-test");
        m.Add(forest, "f");
        m.SetDiffMode(CceDiffMode.Local);

        // Just ensure the host metrics path doesn't throw for the new loss types
        _ = CceMetrics.Loss(m, ds, CceLossType.BinaryCrossEntropy);
        _ = CceMetrics.Loss(m, ds, CceLossType.Huber);
    }

    [Fact]
    public void EndToEnd_Synthetic_Training_Smoke_Matches_Report_Path()
    {
        // Mirrors the benchmark report path using the public synthetic + FitHistory surface.
        using var ds = CceDataset.FromGaussianBlobs(128, 6, 3, 16, seed: 2026);
        using var forest = CceForest.Open(System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"smoke_{System.Guid.NewGuid():N}.cce"), 4);
        forest.AddLinearBranch("h", 6, 8, 3);
        using var model = new CceModel("smoke-report");
        model.Add(forest, "f");
        model.SetDiffMode(CceDiffMode.Hybrid);
        var hist = model.FitHistory(ds, new CceTrainingConfig { MaxEpochs = 2, Loss = CceLossType.CrossEntropy });
        float acc = CceMetrics.Accuracy(model, ds);
        Assert.True(hist.FinalLoss >= 0);
        Assert.True(acc >= 0f && acc <= 1f);
    }

    [Fact]
    public void CceModel_GPU_API_Does_Not_Throw_On_Any_Device()
    {
        using var model = new CceModel("gpu-smoke");
        // These must be safe even on machines without CUDA
        model.SetDevice(CceDevice.Cpu);
        model.SetDevice(CceDevice.Auto);
        _ = model.TryUseGpu();          // should return false or succeed silently
        model.UseDevice(CceDevice.Cuda); // extension, must not throw
    }

    [Fact]
    public void CceSpecialistLibrary_Load_Seal_MemoryReport_Works()
    {
        string dir = Path.Combine(Path.GetTempPath(), $"libtest_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        string f1 = Path.Combine(dir, "s1.cce");
        string f2 = Path.Combine(dir, "s2.cce");

        try
        {
            using (var fa = CceForest.Open(f1, 4)) { fa.AddLinearBranch("a", 4, 4, 2); fa.Seal(); }
            using (var fb = CceForest.Open(f2, 4)) { fb.AddLinearBranch("b", 4, 4, 2); fb.Seal(); }

            using var lib = new CceSpecialistLibrary();
            lib.LoadForest(f1, "one", sealImmediately: true);
            lib.LoadDirectory(dir, sealImmediately: true);
            lib.SealAll();

            Assert.True(lib.Count >= 1);
            _ = lib.GetMemoryReport();
            _ = lib.GetApproximateParameterBytes();
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void WeightImport_Roundtrip_Does_Not_Throw()
    {
        string path = Path.Combine(Path.GetTempPath(), $"wimport_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, 4);
            int b = forest.AddLinearBranch("head", 6, 4, 3);
            float[] w = new float[3 * 4];   // head is hidden(4) -> 3
            float[] bias = new float[3];
            // Just import zeros (valid)
            forest.SetLinearBranchWeights(b, w, inDim: 4, outDim: 3, bias);

            forest.Seal();

            // Use model-level routed inference (works after seal)
            using var model = new CceModel("wimport-test");
            model.Add(forest, "f");
            var (lbl, conf) = model.Infer(new float[6]);
            Assert.True(lbl >= -1); // just that it didn't crash
            _ = conf;
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void EarlyStopping_Maximizes_ValidationAccuracy()
    {
        var earlyStopping = new CceEarlyStopping(
            patience: 1,
            minDelta: 0.01,
            monitor: CceEarlyStoppingMonitor.ValidationAccuracy);

        Assert.True(earlyStopping.OnEpochEnd(new EpochInfo(0, 1.0, 0.50f, 0f)));
        Assert.False(earlyStopping.OnEpochEnd(new EpochInfo(1, 0.9, 0.505f, 0f)));

        Assert.True(earlyStopping.ShouldStop);
        Assert.Equal(0.50, earlyStopping.BestValue!.Value, precision: 5);
    }

    [Fact]
    public void EarlyStopping_Treats_NonFinite_Monitor_As_NoImprovement()
    {
        var earlyStopping = new CceEarlyStopping(
            patience: 1,
            monitor: CceEarlyStoppingMonitor.TrainLoss);

        Assert.False(earlyStopping.OnEpochEnd(new EpochInfo(0, double.NaN, null, 0f)));
        Assert.True(earlyStopping.ShouldStop);
        Assert.Null(earlyStopping.BestValue);
    }

    [Fact]
    public void Callbacks_Combine_Invokes_All_Handlers_And_Stops_When_Any_Stop()
    {
        int firstEpoch = 0;
        int secondEpoch = 0;
        int firstBatch = 0;
        int secondBatch = 0;

        var first = new CceCallbacks
        {
            OnEpochEnd = _ => { firstEpoch++; return true; },
            OnBatchEnd = _ => { firstBatch++; return true; }
        };
        var second = new CceCallbacks
        {
            OnEpochEnd = _ => { secondEpoch++; return false; },
            OnBatchEnd = _ => { secondBatch++; return false; }
        };

        CceCallbacks combined = CceCallbacks.Combine(first, second);

        Assert.False(combined.OnEpochEnd!(new EpochInfo(0, 1.0, null, 0f)));
        Assert.False(combined.OnBatchEnd!(new BatchInfo(0, 0, 8, 1.0, 0f)));
        Assert.Equal(1, firstEpoch);
        Assert.Equal(1, secondEpoch);
        Assert.Equal(1, firstBatch);
        Assert.Equal(1, secondBatch);
    }

    [Fact]
    public void SetScheduler_Rejects_Disposed_Scheduler()
    {
        using var model = new CceModel("disposed-scheduler");
        var scheduler = new CceScheduler(new CceSchedulerConfig(CceSchedulerType.Cosine, 0.01f));
        scheduler.Dispose();

        Assert.Throws<ObjectDisposedException>(() => model.SetScheduler(scheduler));
    }

    [Fact]
    public void Model_Retains_Attached_Scheduler_For_Native_Training()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_sched_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 4);
            forest.AddLinearBranch("sched_branch", inputDim: 4, hiddenDim: 6, outputDim: 2);
            using var model = new CceModel("scheduler-retain");
            model.Add(forest, "f");
            AttachSchedulerWithoutKeepingLocal(model);

            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();

            float[] inputs = { 1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f };
            int[] labels = { 0, 1 };
            using var ds = CceDataset.FromLabels(inputs, labels, 2, 4, 2, 2);

            double loss = model.Train(ds, new CceTrainingConfig { MaxEpochs = 2, ShuffleEachEpoch = false });

            Assert.True(double.IsFinite(loss));
            Assert.True(loss >= 0);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    private static void AttachSchedulerWithoutKeepingLocal(CceModel model)
    {
        var scheduler = new CceScheduler(new CceSchedulerConfig(CceSchedulerType.Warmup, 0.02f));
        model.SetScheduler(scheduler);
    }

    [Fact]
    public void Forward_On_Empty_Model_Reports_Native_Invalid_State()
    {
        using var model = new CceModel("forward-empty");

        Assert.Throws<InvalidOperationException>(() =>
            model.Forward(new float[] { 1, 0, 0, 0 }, outputDim: 2));
    }

    [Fact]
    public void Metrics_TopK_Rejects_Invalid_K()
    {
        var inputs = new float[4 * 2];
        var targets = new float[4 * 2];
        using var ds = CceDataset.FromArrays(inputs, targets, 4, 2, 2, 2);
        using var model = new CceModel("topk-invalid");

        Assert.Throws<ArgumentOutOfRangeException>(() => CceMetrics.TopK(model, ds, k: 0));
    }
}
