using System;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class PredictionApiTests
{
    [Fact]
    public void PredictHelpers_Use_Raw_Routed_Outputs()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 4);
            forest.AddLinearBranch("predictor", inputDim: 4, hiddenDim: 6, outputDim: 3);

            using var model = new CceModel("predict-api");
            model.Add(forest, "predictors");

            float[] input = { 1f, 0.25f, -0.5f, 0.75f };

            int label = model.Predict(input, outputDim: 3);
            float[] probabilities = model.PredictProbabilities(input, outputDim: 3);

            Assert.InRange(label, 0, 2);
            Assert.Equal(3, probabilities.Length);
            Assert.All(probabilities, p => Assert.True(float.IsFinite(p)));
            Assert.InRange(probabilities[0] + probabilities[1] + probabilities[2], 0.999f, 1.001f);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void PredictBatch_Returns_One_Label_Per_Sample()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 4);
            forest.AddLinearBranch("batch-predictor", inputDim: 4, hiddenDim: 6, outputDim: 3);

            using var model = new CceModel("predict-batch-api");
            model.Add(forest, "predictors");

            float[] inputs =
            {
                1f, 0f, 0f, 0f,
                0f, 1f, 0f, 0f,
            };
            float[] targets =
            {
                1f, 0f, 0f,
                0f, 1f, 0f,
            };
            using var dataset = CceDataset.FromArrays(inputs, targets, nSamples: 2, inDim: 4, outDim: 3, batchSize: 2);
            Assert.True(dataset.NextBatch(out var batch));

            int[] labels = model.PredictBatch(batch, outputDim: 3);
            float[] probabilities = model.PredictProbabilitiesBatch(batch, outputDim: 3, out int actualOutputDim);
            CceClassificationReport report = CceMetrics.ClassificationReport(model, dataset, topK: 2);
            CceClassificationReport modelReport = model.Evaluate(dataset, topK: 2);
            double mse = CceMetrics.Loss(model, dataset, CceLossType.MeanSquaredError);
            double modelMse = model.EvaluateLoss(dataset, CceLossType.MeanSquaredError);
            double ce = CceMetrics.Loss(model, dataset, CceLossType.CrossEntropy);
            int[,] confusion = CceMetrics.ConfusionMatrix(model, dataset);
            int observed = 0;
            for (int r = 0; r < confusion.GetLength(0); r++)
            for (int c = 0; c < confusion.GetLength(1); c++)
                observed += confusion[r, c];

            Assert.Equal(2, labels.Length);
            Assert.All(labels, label => Assert.InRange(label, 0, 2));
            Assert.Equal(3, actualOutputDim);
            Assert.Equal(6, probabilities.Length);
            for (int s = 0; s < 2; s++)
            {
                float rowSum = probabilities[s * 3] + probabilities[s * 3 + 1] + probabilities[s * 3 + 2];
                Assert.InRange(rowSum, 0.999f, 1.001f);
            }
            Assert.Equal(2, report.SampleCount);
            Assert.Equal(3, report.ClassCount);
            Assert.InRange(report.Accuracy, 0f, 1f);
            Assert.InRange(report.TopK, 0f, 1f);
            Assert.Equal(report.SampleCount, modelReport.SampleCount);
            Assert.Equal(report.ClassCount, modelReport.ClassCount);
            Assert.Equal(report.Accuracy, modelReport.Accuracy);
            Assert.Equal(report.TopK, modelReport.TopK);
            Assert.True(double.IsFinite(mse));
            Assert.Equal(mse, modelMse);
            Assert.True(double.IsFinite(ce));
            Assert.True(mse >= 0);
            Assert.True(ce >= 0);
            Assert.Equal(3, confusion.GetLength(0));
            Assert.Equal(3, confusion.GetLength(1));
            Assert.Equal(2, observed);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void ForwardBatch_Compacts_When_Output_Capacity_Exceeds_Actual_Dim()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 4);
            forest.AddLinearBranch("compact-forward", inputDim: 4, hiddenDim: 6, outputDim: 2);

            using var model = new CceModel("forward-batch-compact");
            model.Add(forest, "predictors");

            float[] inputs =
            {
                1f, 0f, 0f, 0f,
                0f, 1f, 0f, 0f,
            };
            float[] targets =
            {
                1f, 0f,
                0f, 1f,
            };
            using var dataset = CceDataset.FromArrays(inputs, targets, nSamples: 2, inDim: 4, outDim: 2, batchSize: 2);
            Assert.True(dataset.NextBatch(out var batch));

            float[] outputs = model.ForwardBatch(batch, outputDim: 4, out int actualOutputDim);

            Assert.Equal(2, actualOutputDim);
            Assert.Equal(4, outputs.Length);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }
}
