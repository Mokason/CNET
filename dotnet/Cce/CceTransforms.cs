using System;

namespace CNET.Cce;

/// <summary>Small host-side transforms for preparing flat CCE training arrays.</summary>
public static class CceTransforms
{
    /// <summary>
    /// Converts integer labels to a row-major one-hot target matrix.
    /// Optional label smoothing distributes <paramref name="labelSmoothing"/> across non-target classes.
    /// </summary>
    public static float[] OneHot(ReadOnlySpan<int> labels, int classCount, float labelSmoothing = 0f)
    {
        if (classCount <= 0) throw new ArgumentOutOfRangeException(nameof(classCount));
        if (labelSmoothing < 0f || labelSmoothing >= 1f) throw new ArgumentOutOfRangeException(nameof(labelSmoothing));
        if (classCount == 1 && labelSmoothing > 0f)
            throw new ArgumentOutOfRangeException(nameof(labelSmoothing), "Label smoothing requires at least two classes.");

        var targets = new float[labels.Length * classCount];
        float onValue = 1.0f - labelSmoothing;
        float offValue = labelSmoothing > 0f ? labelSmoothing / (classCount - 1) : 0f;
        for (int i = 0; i < labels.Length; i++)
        {
            int label = labels[i];
            if (label < 0 || label >= classCount)
                throw new ArgumentOutOfRangeException(nameof(labels), $"Label {label} is outside [0, {classCount}).");
            int offset = i * classCount;
            if (offValue > 0f)
            {
                for (int c = 0; c < classCount; c++)
                    targets[offset + c] = offValue;
            }
            targets[offset + label] = onValue;
        }
        return targets;
    }

    /// <summary>L2-normalizes each row of a flat row-major matrix. Zero rows remain zero.</summary>
    public static float[] NormalizeRowsL2(ReadOnlySpan<float> values, int rows, int dim, float epsilon = 1e-12f)
    {
        ValidateMatrix(values.Length, rows, dim);
        if (epsilon < 0) throw new ArgumentOutOfRangeException(nameof(epsilon));

        float[] output = values.ToArray();
        for (int r = 0; r < rows; r++)
        {
            int offset = r * dim;
            double sumSq = 0.0;
            for (int d = 0; d < dim; d++)
            {
                float v = output[offset + d];
                sumSq += (double)v * v;
            }

            double norm = Math.Sqrt(sumSq);
            if (norm <= epsilon) continue;

            float inv = (float)(1.0 / norm);
            for (int d = 0; d < dim; d++)
                output[offset + d] *= inv;
        }
        return output;
    }

    internal static void ValidateMatrix(int valueCount, int rows, int dim)
    {
        if (rows <= 0) throw new ArgumentOutOfRangeException(nameof(rows));
        if (dim <= 0) throw new ArgumentOutOfRangeException(nameof(dim));
        if (valueCount != rows * dim)
            throw new ArgumentException("Flat value count must equal rows * dim.");
    }
}
