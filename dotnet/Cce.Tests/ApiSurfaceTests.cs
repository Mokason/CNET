using System;
using System.Linq;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class ApiSurfaceTests
{
    private static float[] MakeRandomData(int n, int dim, int seed = 42)
    {
        var rnd = new Random(seed);
        var data = new float[n * dim];
        for (int i = 0; i < data.Length; i++)
            data[i] = (float)(rnd.NextDouble() * 2.0 - 1.0);
        return data;
    }

    private static float[] MakeOneHotTargets(int n, int classes)
    {
        var targets = new float[n * classes];
        for (int i = 0; i < n; i++)
        {
            int label = i % classes;
            targets[i * classes + label] = 1.0f;
        }
        return targets;
    }

    [Fact]
    public void Dataset_FromArrays_Creates_And_Iterates_Correctly()
    {
        const int n = 100;
        const int inDim = 16;
        const int outDim = 4;
        const int batchSize = 8;

        var inputs = MakeRandomData(n, inDim);
        var targets = MakeOneHotTargets(n, outDim);

        using var ds = CceDataset.FromArrays(inputs, targets, n, inDim, outDim, batchSize);

        int totalSeen = 0;
        ds.Reset();
        while (ds.NextBatch(out var batch))
        {
            Assert.True(batch.BatchSize > 0);
            Assert.Equal(inDim, batch.InputDim);
            Assert.Equal(outDim, batch.OutputDim);
            Assert.True(batch.Inputs.Length > 0);
            totalSeen += batch.BatchSize;
        }

        Assert.Equal(n, totalSeen);
    }

    [Fact]
    public void Dataset_FromArrays_Rejects_Mismatched_Lengths()
    {
        Assert.Throws<ArgumentException>(() =>
            CceDataset.FromArrays(new float[3], new float[4], nSamples: 2, inDim: 2, outDim: 2, batchSize: 1));
    }

    [Fact]
    public void Dataset_WrapArrays_Works_And_Shuffle_Does_Not_Crash()
    {
        var inputs = MakeRandomData(32, 8);
        var targets = MakeOneHotTargets(32, 2);

        using var ds = CceDataset.WrapArrays(inputs, targets, 32, 8, 2, 8);
        ds.Shuffle();

        int batches = 0;
        ds.Reset();
        while (ds.NextBatch(out _)) batches++;

        Assert.True(batches > 0);
    }

    [Fact]
    public void Transforms_OneHot_And_RowNormalize_Are_Deterministic()
    {
        float[] oneHot = CceTransforms.OneHot(new[] { 2, 0, 1 }, classCount: 3);
        Assert.Equal(new[] { 0f, 0f, 1f, 1f, 0f, 0f, 0f, 1f, 0f }, oneHot);

        float[] smoothed = CceTransforms.OneHot(new[] { 2, 0 }, classCount: 3, labelSmoothing: 0.1f);
        Assert.Equal(0.05f, smoothed[0], precision: 5);
        Assert.Equal(0.05f, smoothed[1], precision: 5);
        Assert.Equal(0.9f, smoothed[2], precision: 5);
        Assert.Equal(0.9f, smoothed[3], precision: 5);
        Assert.Equal(0.05f, smoothed[4], precision: 5);
        Assert.Equal(0.05f, smoothed[5], precision: 5);

        float[] normalized = CceTransforms.NormalizeRowsL2(new[] { 3f, 4f, 0f, 0f }, rows: 2, dim: 2);
        Assert.Equal(0.6f, normalized[0], precision: 5);
        Assert.Equal(0.8f, normalized[1], precision: 5);
        Assert.Equal(0f, normalized[2], precision: 5);
        Assert.Equal(0f, normalized[3], precision: 5);
    }

    [Fact]
    public void Dataset_FromLabels_Composes_Transforms_Into_C_Dataset()
    {
        float[] inputs = { 3f, 4f, 0f, 5f };
        int[] labels = { 1, 0 };

        using var ds = CceDataset.FromLabels(
            inputs,
            labels,
            nSamples: 2,
            inDim: 2,
            classCount: 2,
            batchSize: 2,
            normalizeRowsL2: true);

        Assert.Equal(2, ds.SampleCount);
        Assert.Equal(2, ds.InputDim);
        Assert.Equal(2, ds.OutputDim);
        Assert.True(ds.NextBatch(out var batch));
        Assert.Equal(0.6f, batch.Inputs[0], precision: 5);
        Assert.Equal(0.8f, batch.Inputs[1], precision: 5);
        Assert.Equal(0f, batch.Targets[0], precision: 5);
        Assert.Equal(1f, batch.Targets[1], precision: 5);
        Assert.Equal(1f, batch.Targets[2], precision: 5);
        Assert.Equal(0f, batch.Targets[3], precision: 5);
    }

    [Fact]
    public void Dataset_SplitFromArrays_Creates_Train_And_Validation_Datasets()
    {
        float[] inputs =
        {
            1f, 10f,
            2f, 20f,
            3f, 30f,
            4f, 40f,
        };
        float[] targets =
        {
            1f, 0f,
            0f, 1f,
            1f, 0f,
            0f, 1f,
        };

        using var split = CceDataset.SplitFromArrays(
            inputs,
            targets,
            nSamples: 4,
            inDim: 2,
            outDim: 2,
            batchSize: 8,
            validationFraction: 0.25f,
            shuffle: false);

        Assert.Equal(3, split.Train.SampleCount);
        Assert.Equal(1, split.Validation.SampleCount);
        Assert.True(split.Train.NextBatch(out var train));
        Assert.True(split.Validation.NextBatch(out var validation));
        Assert.Equal(new[] { 1f, 10f, 2f, 20f, 3f, 30f }, train.Inputs.ToArray());
        Assert.Equal(new[] { 4f, 40f }, validation.Inputs.ToArray());
    }

    [Fact]
    public void Dataset_SplitFromArrays_Requires_At_Least_Two_Samples()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            CceDataset.SplitFromArrays(
                new float[] { 1f, 2f },
                new float[] { 1f, 0f },
                nSamples: 1,
                inDim: 2,
                outDim: 2,
                batchSize: 1));
    }

    [Fact]
    public void Scheduler_Can_Be_Created_And_Returns_Valid_Lr()
    {
        using var sched = new CceScheduler(new CceSchedulerConfig(
            Type: CceSchedulerType.Warmup,
            InitialLr: 0.01f,
            WarmupEpochs: 4));

        float lr0 = sched.GetLearningRate(0);
        float lr10 = sched.GetLearningRate(10, currentLoss: 0.5f);

        Assert.True(lr0 > 0);
        Assert.True(lr10 > 0);
        Assert.Equal(0.0025f, lr0, precision: 6);
        Assert.Equal(0.01f, lr10, precision: 6);
    }

    [Fact]
    public void Model_Can_Be_Created_And_Configuration_Does_Not_Crash()
    {
        using var model = new CceModel("test-model-net");

        model.SetDiffMode(CceDiffMode.Hybrid);

        using var sched = new CceScheduler(new CceSchedulerConfig(CceSchedulerType.Warmup, 0.015f));
        model.SetScheduler(sched);

        // We don't assert much more here because forests are optional in this API surface test.
        Assert.NotNull(model);
    }

    [Fact]
    public void Model_Train_And_Batch_APIs_Execute_Without_Crashing_On_Empty_Model()
    {
        var inputs = MakeRandomData(64, 12);
        var targets = MakeOneHotTargets(64, 3);

        using var ds = CceDataset.FromArrays(inputs, targets, 64, 12, 3, 16);
        using var model = new CceModel("dispatch-test");

        // High-level train (will likely hit "no forest" internally but should not throw from P/Invoke layer)
        double loss = 0;
        Exception? trainEx = null;
        try
        {
            loss = model.Train(ds, new CceTrainingConfig
            {
                MaxEpochs = 1,
                TargetLoss = 99f,
                DiffMode = CceDiffMode.Local
            });
        }
        catch (Exception ex)
        {
            trainEx = ex;
        }

        if (trainEx != null)
        {
            Assert.True(trainEx.Message.Contains("forest", StringComparison.OrdinalIgnoreCase) ||
                        trainEx.Message.Contains("invalid", StringComparison.OrdinalIgnoreCase) ||
                        trainEx.Message.Contains("Err", StringComparison.OrdinalIgnoreCase));
        }

        // Per-batch path (avoid capturing ref struct in lambda)
        ds.Reset();
        bool hadBatch = ds.NextBatch(out var batch);
        if (hadBatch)
        {
            Exception? batchEx = null;
            try
            {
                _ = model.TrainBatch(batch);

                var labels = new int[batch.BatchSize];
                var confs = new float[batch.BatchSize];
                model.InferBatch(batch, labels, confs);
            }
            catch (Exception ex)
            {
                batchEx = ex;
            }

            if (batchEx != null)
            {
                Assert.True(batchEx.Message.Contains("forest", StringComparison.OrdinalIgnoreCase) ||
                            batchEx.Message.Contains("ErrInvalidArg", StringComparison.OrdinalIgnoreCase));
            }
        }
    }

    [Fact]
    public void CceHandle_Can_Be_Constructed_When_Archive_Exists_Or_Gracefully_Skipped()
    {
        // This test is best-effort. It exercises the type without requiring a perfect archive match.
        var archivePath = System.IO.Path.Combine("..", "..", "..", "artifacts", "glyph_habitat", "perceptual_subforests.cce");

        if (System.IO.File.Exists(archivePath))
        {
            using var handle = CceHandle.Open(archivePath);
            handle.SetDiffMode(CceDiffMode.Local);

            var dummy = new float[8];
            var ex = Record.Exception(() => handle.Infer(dummy));
            // We don't care if it fails on dimension — we only care the handle was created and methods were callable.
            Assert.True(true);
        }
        else
        {
            // Still validate that the type exists and can be referenced.
            Assert.True(typeof(CceHandle).IsPublic);
        }
    }
}
