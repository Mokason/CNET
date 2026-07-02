using System;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Safe builder for one CCE specialist cascade. The resulting cascade is moved into a <see cref="CceForest"/>
/// as one branch, preserving CCE's compositional branch/forest architecture.
/// </summary>
public sealed class CceCascadeBuilder : SafeHandle
{
    private bool _moved;

    private CceCascadeBuilder() : base(IntPtr.Zero, ownsHandle: true) { }

    public override bool IsInvalid => handle == IntPtr.Zero;

    internal IntPtr DangerousHandle => handle;

    /// <summary>Creates an empty cascade with room for the requested number of blocks.</summary>
    public static CceCascadeBuilder Create(int maxBlocks)
    {
        if (maxBlocks <= 0) throw new ArgumentOutOfRangeException(nameof(maxBlocks));

        var rc = CceNative.CceCascadeCreate(out IntPtr h, maxBlocks);
        if (rc != CceNative.CceResult.Ok || h == IntPtr.Zero)
            throw new InvalidOperationException($"CceCascadeBuilder.Create failed: {rc}");

        var builder = new CceCascadeBuilder();
        builder.SetHandle(h);
        return builder;
    }

    /// <summary>Adds a sigmoid linear block.</summary>
    public CceCascadeBuilder AddLinear(int inputDim, int outputDim, float initScale = 0.01f)
    {
        ThrowIfInvalid();
        if (inputDim <= 0) throw new ArgumentOutOfRangeException(nameof(inputDim));
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        var rc = CceNative.CceCascadeAddLinear(handle, inputDim, outputDim, initScale);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"AddLinear failed: {rc}");
        return this;
    }

    /// <summary>Adds a final linear head block that emits raw routed outputs/logits.</summary>
    public CceCascadeBuilder AddLinearHead(int inputDim, int outputDim, float initScale = 0.01f)
    {
        ThrowIfInvalid();
        if (inputDim <= 0) throw new ArgumentOutOfRangeException(nameof(inputDim));
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        var rc = CceNative.CceCascadeAddLinearHead(handle, inputDim, outputDim, initScale);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"AddLinearHead failed: {rc}");
        return this;
    }

    /// <summary>Adds a patch contract block. The following block should accept <c>patchSize * patchSize * channels</c> inputs.</summary>
    public CceCascadeBuilder AddPatch(int patchSize, int stride, int channels)
    {
        ThrowIfInvalid();
        if (patchSize <= 0) throw new ArgumentOutOfRangeException(nameof(patchSize));
        if (stride <= 0) throw new ArgumentOutOfRangeException(nameof(stride));
        if (channels <= 0) throw new ArgumentOutOfRangeException(nameof(channels));

        var rc = CceNative.CceCascadeAddPatch(handle, patchSize, stride, channels);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"AddPatch failed: {rc}");
        return this;
    }

    /// <summary>Moves this built cascade into <paramref name="forest"/> as a named branch.</summary>
    public int AddTo(CceForest forest, string name)
    {
        ArgumentNullException.ThrowIfNull(forest);
        return forest.AddCascadeBranch(name, this);
    }

    internal void ThrowIfInvalid()
    {
        if (IsClosed || IsInvalid || _moved)
            throw new ObjectDisposedException(nameof(CceCascadeBuilder));
    }

    internal void MarkMoved()
    {
        _moved = true;
    }

    protected override bool ReleaseHandle()
    {
        if (!_moved && handle != IntPtr.Zero)
            CceNative.CceCascadeDestroy(handle);
        return true;
    }
}
