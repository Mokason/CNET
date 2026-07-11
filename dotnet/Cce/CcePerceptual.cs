using System;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// .NET wrapper for CCE Perceptual Leaves (glyph, 7-segment, grid, block domains).
/// These provide router + goodness + margin based classification suitable for
/// contract-based composition and Glyph Habitat style flows.
/// 
/// Use together with CceForest (e.g. from packed 1.6-bit Supra via CceSupraA2A.GetSpecialistsForest)
/// and CceModel / CceRouter for full Router / Planner / Contracts access from .NET.
/// </summary>
public sealed class CcePerceptual : IDisposable
{
    private IntPtr _forest;
    private CceNative.CceRouterNative _router;
    private bool _disposed;
    private readonly int _inputDim;
    private readonly int _numClasses;

    private CcePerceptual(IntPtr forest, int inputDim, int numClasses)
    {
        _forest = forest;
        _inputDim = inputDim;
        _numClasses = numClasses;
        _router = new CceNative.CceRouterNative { temperature = 1.0f, novelty_threshold = 0.1f, top_k = 3 };
    }

    /// <summary>
    /// Create a general perceptual leaf forest (e.g. for glyphs or custom features).
    /// </summary>
    public static CcePerceptual Create(int inputDim, int numClasses, string domainPrefix = "percept",
                                       int maxBranches = 32, CceDiffMode diffMode = CceDiffMode.Local)
    {
        if (inputDim <= 0 || numClasses <= 0) throw new ArgumentException("Invalid dims");
        var rt = new CceNative.CceRouterNative { temperature = 1.0f, top_k = 3 };
        var rc = CceNative.CcePerceptualCreate(out IntPtr f, ref rt, inputDim, numClasses,
            domainPrefix ?? "percept", maxBranches, (CceNative.CceDiffMode)diffMode);
        if (rc != 0 || f == IntPtr.Zero)
            throw new InvalidOperationException($"Perceptual create failed: {rc}");
        var p = new CcePerceptual(f, inputDim, numClasses);
        p._router = rt;
        return p;
    }

    /// <summary>
    /// Convenience for 7-segment style (inputDim ~7-35).
    /// </summary>
    public static CcePerceptual Create7Seg(int maxBranches = 16, CceDiffMode diffMode = CceDiffMode.Local)
    {
        var rt = new CceNative.CceRouterNative { temperature = 1.0f, top_k = 3 };
        var rc = CceNative.CcePerceptual7segCreate(out IntPtr f, ref rt, maxBranches);
        if (rc != 0 || f == IntPtr.Zero)
            throw new InvalidOperationException($"7seg perceptual create failed: {rc}");
        var p = new CcePerceptual(f, 35, 10); // common glyph/7seg flattened size in examples
        p._router = rt;
        return p;
    }

    public void Train()
    {
        ThrowIfDisposed();
        int rc = (_inputDim <= 10)
            ? CceNative.CcePerceptual7segTrain(_forest)
            : CceNative.CcePerceptualTrain(_forest, _inputDim);
        if (rc != 0) throw new InvalidOperationException("Perceptual train failed");
    }

    /// <summary>
    /// Forward through the perceptual forest. Returns class, router signals for margin/abstention gates,
    /// and optional evidence distribution for late-binding contract ports.
    /// </summary>
    public (int bestClass, float routerScore, float branchGoodness, float[]? evidence) Forward(ReadOnlySpan<float> input, int topk = 0)
    {
        ThrowIfDisposed();
        float[] inArr = input.ToArray();
        float[] outOne = new float[_numClasses];
        float rs, bg;
        int cls;
        float[]? ev = (topk > 0) ? new float[topk] : null;

        int rc = (_inputDim <= 10)
            ? CceNative.CcePerceptual7segForward(_forest, ref _router, inArr, inArr.Length, outOne, _numClasses, out rs, out bg, out cls, ev ?? Array.Empty<float>(), topk)
            : CceNative.CcePerceptualForward(_forest, ref _router, inArr, inArr.Length, outOne, _numClasses, out rs, out bg, out cls, ev ?? Array.Empty<float>(), topk);

        if (rc != 0) throw new InvalidOperationException("Perceptual forward failed");
        return (cls, rs, bg, ev);
    }

    public float EffectiveMargin(float baseMargin, float routerScore, float branchGoodness)
        => CceNative.CcePerceptualEffectiveMargin(baseMargin, routerScore, branchGoodness);

    /// <summary>
    /// Get the raw CCE forest for further composition (add to CceModel, use with full router/planner/contracts).
    /// </summary>
    public CceForest GetForest()
    {
        ThrowIfDisposed();
        // Borrowed view (perceptual forest owned by this instance)
        return CceForest.FromNativeHandle(_forest, owns: false);
    }

    public void Dispose()
    {
        if (!_disposed && _forest != IntPtr.Zero)
        {
            CceNative.CceForestClose(_forest);
            _forest = IntPtr.Zero;
            _disposed = true;
        }
    }

    private void ThrowIfDisposed()
    {
        if (_disposed || _forest == IntPtr.Zero) throw new ObjectDisposedException(nameof(CcePerceptual));
    }
}