using System;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Managed wrapper over the CCE router (SSMax + goodness + novelty based specialist selection).
/// Used by perceptual leaves and CceModel training dispatch. Can be used directly with forests
/// (including 1.6-bit packed ones) for custom routing in contract-based or glyph compositions.
/// </summary>
public sealed class CceRouter
{
    private CceNative.CceRouterNative _native;

    public float Temperature { get => _native.temperature; set => _native.temperature = value; }
    public float NoveltyThreshold { get => _native.novelty_threshold; set => _native.novelty_threshold = value; }
    public int TopK { get => _native.top_k; set => _native.top_k = value; }

    public CceRouter(float temperature = 1.0f, int topK = 3, float noveltyThreshold = 0.1f)
    {
        _native = new CceNative.CceRouterNative { temperature = temperature, top_k = topK, novelty_threshold = noveltyThreshold };
        var rc = CceNative.CceRouterInit(ref _native, temperature, topK);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Router init failed: {rc}");
    }

    /// <summary>
    /// Route an input vector against a (possibly packed) forest. Returns best branch and score.
    /// </summary>
    public (int branch, float score) Route(CceForest forest, ReadOnlySpan<float> input)
    {
        if (forest == null || forest.IsInvalid) throw new ArgumentException("Invalid forest");
        float[] inArr = input.ToArray();
        var rc = CceNative.CceRouterRoute(ref _native, forest.DangerousHandle, inArr, inArr.Length, out int br, out float sc);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Route failed: {rc}");
        return (br, sc);
    }

    internal ref CceNative.CceRouterNative NativeRef => ref _native;
}