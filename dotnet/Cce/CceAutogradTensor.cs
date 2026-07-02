using System;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Autograd tensor handle. Safe wrapper around native cce_ag_tensor*.
/// </summary>
public sealed class CceAutogradTensor : IDisposable
{
    private readonly CceAutogradContext _owner;
    internal IntPtr Handle { get; private set; }
    private bool _disposed;

    internal CceAutogradTensor(CceAutogradContext owner, IntPtr handle, int rows, int cols, bool requiresGrad)
    {
        _owner = owner;
        Handle = handle;
        Rows = rows;
        Cols = cols;
        RequiresGrad = requiresGrad;
    }

    public int Rows { get; }
    public int Cols { get; }
    public bool RequiresGrad { get; }

    public float[] GetData()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradTensor));
        IntPtr ptr = CceNative.CceAgData(Handle);
        if (ptr == IntPtr.Zero) return Array.Empty<float>();
        int n = Rows * Cols;
        float[] arr = new float[n];
        Marshal.Copy(ptr, arr, 0, n);
        return arr;
    }

    public float[] GetGrad()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradTensor));
        if (!RequiresGrad) return Array.Empty<float>();
        IntPtr ptr = CceNative.CceAgGrad(Handle);
        if (ptr == IntPtr.Zero) return Array.Empty<float>();
        int n = Rows * Cols;
        float[] arr = new float[n];
        Marshal.Copy(ptr, arr, 0, n);
        return arr;
    }

    public void Dispose()
    {
        // Native memory is owned by the context; we just drop the handle reference.
        Handle = IntPtr.Zero;
        _disposed = true;
    }
}
