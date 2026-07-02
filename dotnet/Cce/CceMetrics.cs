using System;

namespace CNET.Cce;

/// <summary>Basic classifier evaluation report over a CCE dataset.</summary>
public sealed record CceClassificationReport(
    int SampleCount,
    int ClassCount,
    float Accuracy,
    float TopK,
    int[,] ConfusionMatrix);

/// <summary>Basic regression evaluation report over routed CCE outputs.</summary>
public sealed record CceRegressionReport(
    int SampleCount,
    int OutputDim,
    double MeanSquaredError,
    double MeanAbsoluteError,
    double R2Score);

/// <summary>Host-side evaluation metrics (no native changes).</summary>
public static class CceMetrics
{
    /// <summary>Top-1 accuracy: raw routed output argmax vs target argmax over the whole dataset.</summary>
    public static float Accuracy(CceModel model, CceDataset dataset)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(dataset);

        int correct = 0, total = 0;
        dataset.Reset();
        while (dataset.NextBatch(out var batch))
        {
            var logits = model.ForwardBatch(batch, dataset.OutputDim, out int actualDim);
            if (actualDim != dataset.OutputDim)
                throw new InvalidOperationException($"Forward output dim {actualDim} does not match dataset target dim {dataset.OutputDim}");

            for (int s = 0; s < batch.BatchSize; s++)
            {
                var row = logits.AsSpan(s * actualDim, actualDim);
                var tgt = batch.Targets.Slice(s * batch.OutputDim, batch.OutputDim);
                int tgtLabel = ArgMax(tgt);
                if (ArgMax(row) == tgtLabel) correct++;
                total++;
            }
        }
        return total > 0 ? (float)correct / total : 0f;
    }

    /// <summary>Top-k accuracy using raw routed cascade outputs.</summary>
    public static float TopK(CceModel model, CceDataset dataset, int k)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(dataset);
        if (k <= 0) throw new ArgumentOutOfRangeException(nameof(k));
        if (k > dataset.OutputDim) throw new ArgumentOutOfRangeException(nameof(k));

        int correct = 0, total = 0;
        dataset.Reset();
        while (dataset.NextBatch(out var batch))
        {
            var logits = model.ForwardBatch(batch, dataset.OutputDim, out int actualDim);
            if (actualDim != dataset.OutputDim)
                throw new InvalidOperationException($"Forward output dim {actualDim} does not match dataset target dim {dataset.OutputDim}");

            for (int s = 0; s < batch.BatchSize; s++)
            {
                var row = logits.AsSpan(s * actualDim, actualDim);
                var tgt = batch.Targets.Slice(s * batch.OutputDim, batch.OutputDim);
                int tgtLabel = ArgMax(tgt);
                if (ContainsInTopK(row, tgtLabel, k)) correct++;
                total++;
            }
        }
        return total > 0 ? (float)correct / total : 0f;
    }

    /// <summary>
    /// Builds a target-label by predicted-label confusion matrix using raw routed output argmax.
    /// Rows are target labels, columns are predicted labels.
    /// </summary>
    public static int[,] ConfusionMatrix(CceModel model, CceDataset dataset)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(dataset);
        if (dataset.OutputDim <= 0) throw new ArgumentOutOfRangeException(nameof(dataset), "Dataset output dimension must be positive.");

        int[,] matrix = new int[dataset.OutputDim, dataset.OutputDim];
        dataset.Reset();
        while (dataset.NextBatch(out var batch))
        {
            var logits = model.ForwardBatch(batch, dataset.OutputDim, out int actualDim);
            if (actualDim != dataset.OutputDim)
                throw new InvalidOperationException($"Forward output dim {actualDim} does not match dataset target dim {dataset.OutputDim}");

            for (int s = 0; s < batch.BatchSize; s++)
            {
                var row = logits.AsSpan(s * actualDim, actualDim);
                var tgt = batch.Targets.Slice(s * batch.OutputDim, batch.OutputDim);
                int target = ArgMax(tgt);
                int predicted = ArgMax(row);
                if ((uint)predicted < (uint)dataset.OutputDim)
                    matrix[target, predicted]++;
            }
        }
        return matrix;
    }

    /// <summary>
    /// Computes mean validation/training loss from raw routed outputs over the whole dataset.
    /// Supports mean-squared error over output vectors and cross-entropy over softmax logits.
    /// </summary>
    public static double Loss(CceModel model, CceDataset dataset, CceLossType lossType = CceLossType.MeanSquaredError)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(dataset);

        double sum = 0.0;
        int samples = 0;
        dataset.Reset();
        while (dataset.NextBatch(out var batch))
        {
            var logits = model.ForwardBatch(batch, dataset.OutputDim, out int actualDim);
            if (actualDim != dataset.OutputDim)
                throw new InvalidOperationException($"Forward output dim {actualDim} does not match dataset target dim {dataset.OutputDim}");

            for (int s = 0; s < batch.BatchSize; s++)
            {
                var output = logits.AsSpan(s * actualDim, actualDim);
                var target = batch.Targets.Slice(s * batch.OutputDim, batch.OutputDim);
                sum += lossType switch
                {
                    CceLossType.MeanSquaredError => VectorMeanSquaredError(output, target),
                    CceLossType.CrossEntropy => CrossEntropy(output, target),
                    CceLossType.BinaryCrossEntropy => BinaryCrossEntropy(output, target),
                    CceLossType.Huber => HuberLoss(output, target, delta: 1f),
                    _ => throw new ArgumentOutOfRangeException(nameof(lossType)),
                };
                samples++;
            }
        }

        return samples > 0 ? sum / samples : 0.0;
    }

    /// <summary>
    /// Computes a compact classifier report using the managed metrics surface.
    /// This intentionally stays host-side: the native engine only supplies routed predictions/logits.
    /// </summary>
    public static CceClassificationReport ClassificationReport(CceModel model, CceDataset dataset, int topK = 1)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(dataset);
        if (topK <= 0) throw new ArgumentOutOfRangeException(nameof(topK));
        if (topK > dataset.OutputDim) throw new ArgumentOutOfRangeException(nameof(topK));

        return new CceClassificationReport(
            dataset.SampleCount,
            dataset.OutputDim,
            Accuracy(model, dataset),
            TopK(model, dataset, topK),
            ConfusionMatrix(model, dataset));
    }

    /// <summary>Mean squared error over all routed output elements in the dataset.</summary>
    public static double RegressionMeanSquaredError(CceModel model, CceDataset dataset) =>
        RegressionSums(model, dataset).MeanSquaredError;

    /// <summary>Mean absolute error over all routed output elements in the dataset.</summary>
    public static double RegressionMeanAbsoluteError(CceModel model, CceDataset dataset) =>
        RegressionSums(model, dataset).MeanAbsoluteError;

    /// <summary>Coefficient of determination over all routed output elements in the dataset.</summary>
    public static double R2Score(CceModel model, CceDataset dataset) =>
        RegressionSums(model, dataset).R2Score;

    /// <summary>Computes a compact regression report using raw routed outputs.</summary>
    public static CceRegressionReport RegressionReport(CceModel model, CceDataset dataset)
    {
        var sums = RegressionSums(model, dataset);
        return new CceRegressionReport(
            dataset.SampleCount,
            dataset.OutputDim,
            sums.MeanSquaredError,
            sums.MeanAbsoluteError,
            sums.R2Score);
    }

    private static int ArgMax(ReadOnlySpan<float> v)
    {
        int best = 0; float bv = v.Length > 0 ? v[0] : 0f;
        for (int i = 1; i < v.Length; i++) if (v[i] > bv) { bv = v[i]; best = i; }
        return best;
    }

    private static bool ContainsInTopK(ReadOnlySpan<float> values, int label, int k)
    {
        int better = 0;
        float target = values[label];
        for (int i = 0; i < values.Length; i++)
        {
            if (i != label && values[i] > target) better++;
        }
        return better < k;
    }

    private static double VectorMeanSquaredError(ReadOnlySpan<float> output, ReadOnlySpan<float> target)
    {
        double sum = 0.0;
        for (int i = 0; i < output.Length; i++)
        {
            double d = output[i] - target[i];
            sum += d * d;
        }
        return sum / output.Length;
    }

    private static double CrossEntropy(ReadOnlySpan<float> logits, ReadOnlySpan<float> target)
    {
        float max = logits[0];
        for (int i = 1; i < logits.Length; i++)
            if (logits[i] > max) max = logits[i];

        double sumExp = 0.0;
        for (int i = 0; i < logits.Length; i++)
            sumExp += Math.Exp(logits[i] - max);

        double logSumExp = max + Math.Log(sumExp);
        double loss = 0.0;
        for (int i = 0; i < logits.Length; i++)
            loss -= target[i] * (logits[i] - logSumExp);
        return loss;
    }

    private readonly record struct RegressionAccumulator(
        double MeanSquaredError,
        double MeanAbsoluteError,
        double R2Score);

    private static RegressionAccumulator RegressionSums(CceModel model, CceDataset dataset)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(dataset);

        double squaredError = 0.0;
        double absoluteError = 0.0;
        double targetSum = 0.0;
        double targetSumSq = 0.0;
        long count = 0;

        dataset.Reset();
        while (dataset.NextBatch(out var batch))
        {
            var outputs = model.ForwardBatch(batch, dataset.OutputDim, out int actualDim);
            if (actualDim != dataset.OutputDim)
                throw new InvalidOperationException($"Forward output dim {actualDim} does not match dataset target dim {dataset.OutputDim}");

            for (int s = 0; s < batch.BatchSize; s++)
            {
                var output = outputs.AsSpan(s * actualDim, actualDim);
                var target = batch.Targets.Slice(s * batch.OutputDim, batch.OutputDim);
                for (int i = 0; i < actualDim; i++)
                {
                    double t = target[i];
                    double d = output[i] - t;
                    squaredError += d * d;
                    absoluteError += Math.Abs(d);
                    targetSum += t;
                    targetSumSq += t * t;
                    count++;
                }
            }
        }

        if (count == 0)
            return new RegressionAccumulator(0.0, 0.0, 0.0);

        double mean = targetSum / count;
        double totalVariance = targetSumSq - count * mean * mean;
        double r2 = totalVariance <= 0.0
            ? (squaredError <= 0.0 ? 1.0 : 0.0)
            : 1.0 - squaredError / totalVariance;

        return new RegressionAccumulator(
            squaredError / count,
            absoluteError / count,
            r2);
    }

    private static double BinaryCrossEntropy(ReadOnlySpan<float> logits, ReadOnlySpan<float> target)
    {
        // Numerically stable sigmoid + BCE
        double loss = 0.0;
        for (int i = 0; i < logits.Length; i++)
        {
            double y = target[i];
            double x = logits[i];
            // log(sigmoid(x)) = -softplus(-x) ; log(1-sigmoid) = -softplus(x)
            double sp = Softplus(x);
            double spNeg = Softplus(-x);
            loss += y * spNeg + (1.0 - y) * sp;
        }
        return loss / Math.Max(1, logits.Length);
    }

    private static double Softplus(double x) => x > 20 ? x : Math.Log(1.0 + Math.Exp(x));

    private static double HuberLoss(ReadOnlySpan<float> output, ReadOnlySpan<float> target, float delta)
    {
        double sum = 0.0;
        double d2 = delta * delta;
        for (int i = 0; i < output.Length; i++)
        {
            double e = output[i] - target[i];
            double ae = Math.Abs(e);
            if (ae <= delta)
                sum += 0.5 * e * e;
            else
                sum += delta * (ae - 0.5 * delta);
        }
        return sum / Math.Max(1, output.Length);
    }
}
