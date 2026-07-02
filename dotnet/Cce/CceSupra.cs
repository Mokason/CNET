using System;
using System.Runtime.InteropServices;
using System.Text;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// High-level wrapper for Supra (decomposed CCE transformer, supports FP / int8 / packed 1.6-bit).
/// The internal specialists forest can be extracted as CceForest for use with
/// CceModel, CceSpecialistLibrary, perceptual leaves, router, and contract-based composition.
/// </summary>
public sealed class CceSupraA2A : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;

    public CceSupraA2A(string weightsDirOrRepo = ".")
    {
        var rc = CceNative.CceSupraA2aLoad(out _handle, weightsDirOrRepo);
        if (rc != CceNative.CceResult.Ok || _handle == IntPtr.Zero)
            throw new InvalidOperationException("Failed to load SupraA2A (need model.safetensors + tokenizer.json in cache or dir; see supra_console).");
    }

    /// <summary>
    /// Load a 1.6-bit packed Supra artifact (produced by cce_supra_export_packed).
    /// The resulting model runs via the packed trit specialists (w_trit path) and its
    /// CCE forest of branches is accessible for Router / Planner / Contracts / Perceptual / Glyph composition.
    /// </summary>
    public static CceSupraA2A LoadPacked(string packedPath)
    {
        if (string.IsNullOrWhiteSpace(packedPath)) throw new ArgumentException("packedPath required", nameof(packedPath));
        var inst = new CceSupraA2A();
        var rc = CceNative.CceSupraA2aLoadPacked(out inst._handle, packedPath);
        if (rc != CceNative.CceResult.Ok || inst._handle == IntPtr.Zero)
            throw new InvalidOperationException($"Failed to load packed 1.6-bit Supra: {rc}.");
        return inst;
    }

    private CceSupraA2A() { }

    /// <summary>
    /// Expose the underlying CCE forest of specialists (including packed 1.6-bit w_trit branches).
    /// The returned forest automatically keeps this SupraA2A alive (via lifetime owner) so you don't have to.
    /// Pass to CceSpecialistLibrary, CceModel, CcePerceptual flows, CceRouter, or CceContract registration
    /// for full Router/Planner/Contracts/Perceptual/Glyph composition from pure C#.
    /// </summary>
    public CceForest GetSpecialistsForest()
    {
        ThrowIfDisposed();
        IntPtr f = CceNative.CceSupraA2aGetForest(_handle);
        if (f == IntPtr.Zero)
            throw new InvalidOperationException("No internal forest available (model not fully loaded or packed path missing specialists).");

        // Attach this instance as lifetime owner. The returned forest will keep
        // the SupraA2A (and its native packed/decomp resources including w_trit) alive
        // as long as the forest is referenced. This removes the "must keep Supra alive" burden.
        var forest = CceForest.FromNativeHandle(f, owns: false);
        forest.AttachLifetimeOwner(this);
        return forest;
    }

    public string ChatStep(string userText)
    {
        ThrowIfDisposed();
        var sb = new StringBuilder(2048);
        var rc = CceNative.CceSupraA2aChatStep(_handle, userText, sb, sb.Capacity);
        if (rc < 0) throw new InvalidOperationException("ChatStep failed");
        return sb.ToString();
    }

    public string CompleteText(string prompt, int maxNew = 64, float temp = 0.8f, int topk = 40)
    {
        ThrowIfDisposed();
        var sb = new StringBuilder(8192);
        var rc = CceNative.CceSupraA2aCompleteText(_handle, prompt, sb, sb.Capacity, maxNew, temp, topk);
        if (rc < 0) throw new InvalidOperationException("CompleteText failed");
        return sb.ToString();
    }

    public string GenerateVisualTokens(string prompt)
    {
        return CompleteText($"<TEXT>{prompt}</TEXT><IMAGE>", 64, 1.0f, 0);
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            CceNative.CceSupraA2aFree(_handle);
            _handle = IntPtr.Zero;
            _disposed = true;
        }
    }

    private void ThrowIfDisposed()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(CceSupraA2A));
    }
}

