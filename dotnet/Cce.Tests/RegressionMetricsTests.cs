using System;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class RegressionMetricsTests
{
    [Fact]
    public void RegressionMetrics_Use_Raw_Routed_Outputs()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_regression_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 4);
            forest.AddLinearBranch("regressor", inputDim: 3, hiddenDim: 5, outputDim: 2);

            using var model = new CceModel("regression-metrics");
            model.Add(forest, "regressors");

            float[] inputs =
            {
                1f, 0f, 0.5f,
                0f, 1f, -0.5f,
            };
            float[] targets =
            {
                0.25f, -0.75f,
                0.5f, 0.125f,
            };
            using var dataset = CceDataset.FromArrays(inputs, targets, nSamples: 2, inDim: 3, outDim: 2, batchSize: 2);

            double mse = CceMetrics.RegressionMeanSquaredError(model, dataset);
            double mae = CceMetrics.RegressionMeanAbsoluteError(model, dataset);
            double r2 = CceMetrics.R2Score(model, dataset);
            CceRegressionReport report = CceMetrics.RegressionReport(model, dataset);
            CceRegressionReport modelReport = model.EvaluateRegression(dataset);

            Assert.True(double.IsFinite(mse));
            Assert.True(double.IsFinite(mae));
            Assert.True(double.IsFinite(r2));
            Assert.True(mse >= 0);
            Assert.True(mae >= 0);
            Assert.Equal(2, report.SampleCount);
            Assert.Equal(2, report.OutputDim);
            Assert.Equal(mse, report.MeanSquaredError);
            Assert.Equal(mae, report.MeanAbsoluteError);
            Assert.Equal(r2, report.R2Score);
            Assert.Equal(report.SampleCount, modelReport.SampleCount);
            Assert.Equal(report.OutputDim, modelReport.OutputDim);
            Assert.Equal(report.MeanSquaredError, modelReport.MeanSquaredError);
            Assert.Equal(report.MeanAbsoluteError, modelReport.MeanAbsoluteError);
            Assert.Equal(report.R2Score, modelReport.R2Score);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }
}
