using System;

namespace CNET.Cce;

/// <summary>
/// Device / backend selection for CCE.
/// 
/// Today the heavy lifting (CUDA/OpenCL) lives in the native C build.
/// When you build the native library with CCE_USE_CUDA=1 the learner and certain
/// block operations can accelerate automatically for supported cascades.
///
/// This type + CceModel.UseDevice(...) provides the future .NET hook surface.
/// </summary>
public enum CceDevice
{
    /// <summary>Default CPU path (always available).</summary>
    Cpu = 0,

    /// <summary>Prefer CUDA if the native was built with CUDA support and a GPU is present.</summary>
    Cuda = 1,

    /// <summary>Prefer OpenCL (emitter exists; runtime dispatch may fall back).</summary>
    OpenCl = 2,

    /// <summary>Let the native pick the best available accelerator (CUDA > OpenCL > CPU).</summary>
    Auto = 3,
}

public static class CceDeviceExtensions
{
    /// <summary>
    /// Requests that the model use the given device for training (primarily GPU acceleration of local/hybrid credit assignment).
    /// 
    /// For CceDevice.Cuda / Auto this will attempt to initialize a CUDA context (if the native was built with CUDA support)
    /// and wire it into the training dispatch. This lets small/medium CCE specialists benefit from GPU without a full PyTorch stack.
    /// 
    /// Safe to call even on CPU-only builds (no-op or graceful fallback).
    /// </summary>
    public static void UseDevice(this CceModel model, CceDevice device)
    {
        ArgumentNullException.ThrowIfNull(model);
        model.SetDevice(device);
    }

    /// <summary>
    /// Shorthand for UseDevice(CceDevice.Cuda).
    /// </summary>
    public static bool TryEnableCuda(this CceModel model)
    {
        ArgumentNullException.ThrowIfNull(model);
        return model.TryUseGpu();
    }
}
