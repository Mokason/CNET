using System;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Optional reverse-mode autograd context (tape).
/// Use ONLY for CceDiffMode.Exact on small heads/tails.
/// Local/hybrid CCE learning does not use this.
/// </summary>
public sealed class CceAutogradContext : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;

    public CceAutogradContext(int maxNodes = 4096)
    {
        if (maxNodes <= 0) throw new ArgumentOutOfRangeException(nameof(maxNodes));
        int rc = CceNative.CceAgCreateRaw(out _handle, (nuint)maxNodes);
        if (rc != 0)
            throw new InvalidOperationException($"Failed to create autograd ctx: {rc}");
    }

    public CceAutogradTensor CreateTensor(float[] data, int rows, int cols, bool requiresGrad = false)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradContext));
        int rc = CceNative.CceAgTensorFromArrayRaw(_handle, data, rows, cols, requiresGrad ? 1 : 0, out IntPtr t);
        if (rc != 0)
            throw new InvalidOperationException($"Tensor creation failed: {rc}");
        return new CceAutogradTensor(this, t, rows, cols, requiresGrad);
    }

    public void ZeroGrad()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradContext));
        CceNative.CceAgZeroGrad(_handle);
    }

    public void Backward(CceAutogradTensor loss)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradContext));
        ArgumentNullException.ThrowIfNull(loss);
        int rc = CceNative.CceAgBackwardRaw(_handle, loss.Handle);
        if (rc != 0)
            throw new InvalidOperationException($"Backward failed: {rc}");
    }

    public void SgdStep(CceAutogradTensor[] parameters, float lr)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradContext));
        if (parameters == null || parameters.Length == 0) return;

        IntPtr[] handles = new IntPtr[parameters.Length];
        for (int i = 0; i < parameters.Length; i++)
            handles[i] = parameters[i].Handle;

        unsafe
        {
            fixed (IntPtr* p = handles)
            {
                int rc = CceNative.CceAgSgdStepRaw((IntPtr)p, (nuint)parameters.Length, lr);
                if (rc != 0)
                    throw new InvalidOperationException($"SgdStep failed: {rc}");
            }
        }
    }

    public CceAutogradTensor MatMul(CceAutogradTensor a, CceAutogradTensor b)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradContext));
        ArgumentNullException.ThrowIfNull(a);
        ArgumentNullException.ThrowIfNull(b);
        int rc = CceNative.CceAgMatmulRaw(_handle, a.Handle, b.Handle, out IntPtr y);
        if (rc != 0)
            throw new InvalidOperationException($"MatMul failed: {rc}");
        return new CceAutogradTensor(this, y, a.Rows, b.Cols, a.RequiresGrad || b.RequiresGrad);
    }

    public CceAutogradTensor AddBias(CceAutogradTensor x, CceAutogradTensor bias)
    {
        int rc = CceNative.CceAgAddBiasRaw(_handle, x.Handle, bias.Handle, out IntPtr y);
        if (rc != 0) throw new InvalidOperationException($"AddBias: {rc}");
        return new CceAutogradTensor(this, y, x.Rows, x.Cols, x.RequiresGrad || bias.RequiresGrad);
    }

    public CceAutogradTensor Relu(CceAutogradTensor x)
    {
        int rc = CceNative.CceAgReluRaw(_handle, x.Handle, out IntPtr y);
        if (rc != 0) throw new InvalidOperationException($"Relu: {rc}");
        return new CceAutogradTensor(this, y, x.Rows, x.Cols, x.RequiresGrad);
    }

    public CceAutogradTensor Exp(CceAutogradTensor x)
    {
        int rc = CceNative.CceAgExpRaw(_handle, x.Handle, out IntPtr y);
        if (rc != 0) throw new InvalidOperationException($"Exp: {rc}");
        return new CceAutogradTensor(this, y, x.Rows, x.Cols, x.RequiresGrad);
    }

    public CceAutogradTensor Log(CceAutogradTensor x)
    {
        int rc = CceNative.CceAgLogRaw(_handle, x.Handle, out IntPtr y);
        if (rc != 0) throw new InvalidOperationException($"Log: {rc}");
        return new CceAutogradTensor(this, y, x.Rows, x.Cols, x.RequiresGrad);
    }

    public CceAutogradTensor MseLoss(CceAutogradTensor pred, CceAutogradTensor target)
    {
        int rc = CceNative.CceAgMseLossRaw(_handle, pred.Handle, target.Handle, out IntPtr loss);
        if (rc != 0) throw new InvalidOperationException($"MseLoss: {rc}");
        return new CceAutogradTensor(this, loss, 1, 1, pred.RequiresGrad);
    }

    public CceAutogradTensor SoftmaxCrossEntropy(CceAutogradTensor logits, CceAutogradTensor target)
    {
        int rc = CceNative.CceAgSoftmaxCrossEntropyRaw(_handle, logits.Handle, target.Handle, out IntPtr loss);
        if (rc != 0) throw new InvalidOperationException($"SoftmaxCE: {rc}");
        return new CceAutogradTensor(this, loss, 1, 1, logits.RequiresGrad);
    }

    public void AdamStep(CceAutogradTensor[] parameters, float lr, float beta1 = 0.9f, float beta2 = 0.999f, float eps = 1e-8f, int t = 1)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradContext));
        if (parameters == null || parameters.Length == 0) return;
        IntPtr[] handles = new IntPtr[parameters.Length];
        for (int i = 0; i < parameters.Length; i++) handles[i] = parameters[i].Handle;
        unsafe
        {
            fixed (IntPtr* p = handles)
            {
                int rc = CceNative.CceAgAdamStepRaw((IntPtr)p, (nuint)parameters.Length, lr, beta1, beta2, eps, t);
                if (rc != 0) throw new InvalidOperationException($"AdamStep failed: {rc}");
            }
        }
    }

    public void SetGpu(IntPtr gpuCtx)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceAutogradContext));
        int rc = CceNative.CceAgSetGpuRaw(_handle, gpuCtx);
        if (rc != 0) throw new InvalidOperationException($"SetGpu failed: {rc}");
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            CceNative.CceAgDestroy(_handle);
            _handle = IntPtr.Zero;
        }
        _disposed = true;
    }
}

// All raw LibraryImport declarations for autograd live in the main CceNative.cs.

