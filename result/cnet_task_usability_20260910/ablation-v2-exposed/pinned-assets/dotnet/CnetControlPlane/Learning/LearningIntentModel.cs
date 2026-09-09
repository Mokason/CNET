namespace CnetControlPlane.Learning;

internal sealed record LearningIntentReport(int TrainExamples, int CalibrationReady, int CalibrationWrongReady, float Threshold);

// Linear bag-of-character-ngrams. Labels are request roles, not answers.
internal sealed class LearningIntentModel
{
    internal const int Dim = 384;
    internal const int Classes = 4;
    private readonly float[] weights;
    private readonly float[] bias;
    internal float Threshold { get; }

    private LearningIntentModel(float[] weights, float[] bias, float threshold)
    {
        this.weights = weights;
        this.bias = bias;
        Threshold = threshold;
    }

    internal static int ClassOf(string status, string? operation) => status == "ready" && operation == "upper" ? 0
        : status == "ready" && operation == "lower" ? 1
        : status == "clarify" ? 2 : 3;

    internal int Classify(string text, out float confidence)
    {
        Span<float> logits = stackalloc float[Classes];
        Span<int> used = stackalloc int[96];
        var count = FillFeatures(Mask(text), used);
        for (var c = 0; c < Classes; c++)
        {
            var score = bias[c];
            for (var i = 0; i < count; i++) score += weights[used[i] * Classes + c];
            logits[c] = score;
        }
        var max = logits[0];
        var arg = 0;
        for (var c = 1; c < Classes; c++) if (logits[c] > max) { max = logits[c]; arg = c; }
        var sum = 0f;
        for (var c = 0; c < Classes; c++) sum += MathF.Exp(logits[c] - max);
        confidence = 1f / sum;
        return arg;
    }

    internal static (LearningIntentModel Model, LearningIntentReport Report) Train(
        IReadOnlyList<LearningIntentExample> train, IReadOnlyList<LearningIntentExample> calibration, uint seed = 20260910)
    {
        var weights = new float[Dim * Classes];
        var bias = new float[Classes];
        var rng = new Random(unchecked((int)seed));
        var order = Enumerable.Range(0, train.Count).ToArray();
        const int epochs = 12;
        var lr = 0.25f;
        Span<float> logits = stackalloc float[Classes];
        Span<int> used = stackalloc int[96];
        Span<float> prob = stackalloc float[Classes];
        for (var epoch = 0; epoch < epochs; epoch++)
        {
            Shuffle(order, rng);
            var step = lr / (1f + epoch);
            foreach (var index in order)
            {
                var example = train[index];
                var target = ClassOf(example.Status, example.Operation);
                var count = FillFeatures(Mask(example.Text), used);
                for (var c = 0; c < Classes; c++)
                {
                    var score = bias[c];
                    for (var i = 0; i < count; i++) score += weights[used[i] * Classes + c];
                    logits[c] = score;
                }
                var max = logits[0];
                for (var c = 1; c < Classes; c++) if (logits[c] > max) max = logits[c];
                var z = 0f;
                for (var c = 0; c < Classes; c++) { prob[c] = MathF.Exp(logits[c] - max); z += prob[c]; }
                for (var c = 0; c < Classes; c++)
                {
                    var grad = prob[c] / z - (c == target ? 1f : 0f);
                    bias[c] -= step * grad;
                    for (var i = 0; i < count; i++) weights[used[i] * Classes + c] -= step * grad;
                }
            }
        }
        var model = new LearningIntentModel(weights, bias, 0);
        var threshold = Calibrate(model, calibration);
        model = new LearningIntentModel(weights, bias, threshold);
        var predictedReady = 0;
        var wrongReady = 0;
        foreach (var example in calibration)
        {
            var predicted = model.Classify(example.Text, out var confidence);
            if (predicted > 1 || confidence < model.Threshold) continue;
            if (example.Status == "ready" && predicted == ClassOf(example.Status, example.Operation)) predictedReady++;
            else wrongReady++;
        }
        return (model, new(train.Count, predictedReady, wrongReady, threshold));
    }

    private static float Calibrate(LearningIntentModel model, IReadOnlyList<LearningIntentExample> corpus)
    {
        var maxWrong = 0f;
        foreach (var example in corpus)
        {
            var predicted = model.Classify(example.Text, out var confidence);
            var target = ClassOf(example.Status, example.Operation);
            if (predicted <= 1 && predicted != target && confidence > maxWrong) maxWrong = confidence;
        }
        return Math.Clamp(maxWrong + 0.02f, 0.35f, 0.999f);
    }

    internal static string Mask(string text)
    {
        var chars = text.ToCharArray();
        for (var i = 0; i < chars.Length; i++)
        {
            var quote = chars[i];
            if (quote is not ('\'' or '"')) continue;
            if (i > 0 && char.IsLetterOrDigit(chars[i - 1]) && quote == '\'') continue;
            var end = text.IndexOf(quote, i + 1);
            if (end < 0) break;
            for (var k = i + 1; k < end; k++) chars[k] = 'C';
            i = end;
        }
        return new string(chars);
    }

    private static int FillFeatures(string text, Span<int> used)
    {
        var count = 0;
        for (var n = 2; n <= 4; n++)
        {
            for (var i = 0; i + n <= text.Length; i++)
            {
                var bucket = Bucket(text.AsSpan(i, n));
                var seen = false;
                for (var k = 0; k < count; k++) if (used[k] == bucket) { seen = true; break; }
                if (seen) continue;
                if (count == used.Length) return count;
                used[count++] = bucket;
            }
        }
        return count;
    }

    private static int Bucket(ReadOnlySpan<char> gram)
    {
        uint hash = 2166136261;
        foreach (var c in gram)
        {
            hash ^= char.ToLowerInvariant(c);
            hash *= 16777619;
        }
        return (int)(hash % Dim);
    }

    private static void Shuffle(int[] order, Random rng)
    {
        for (var i = order.Length - 1; i > 0; i--)
        {
            var j = rng.Next(i + 1);
            (order[i], order[j]) = (order[j], order[i]);
        }
    }
}
