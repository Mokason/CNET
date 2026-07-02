using System;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Managed wrapper around the native CCE scheduler.
/// Can be attached to a <see cref="CceModel"/> or <see cref="CceHandle"/>.
/// </summary>
public sealed class CceScheduler : IDisposable
{
    private IntPtr _native;
    private bool _disposed;

    public CceSchedulerType Type { get; }
    public float InitialLr { get; }

    public float CurrentLr => ReadNative().CurrentLr;

    public CceScheduler(CceSchedulerConfig config)
    {
        Type = config.Type;
        InitialLr = config.InitialLr;
        _native = Marshal.AllocHGlobal(Marshal.SizeOf<Interop.CceNative.CceSchedulerNative>());

        CceNative.InitScheduler(_native, (Interop.CceNative.CceSchedType)config.Type, config.InitialLr);

        var native = ReadNative();
        native.WarmupEpochs = config.WarmupEpochs;
        native.DecayFactor = config.DecayFactor;
        native.StepSize = config.StepSize;
        native.PlateauFactor = config.PlateauFactor;
        native.PlateauPatience = config.PlateauPatience;
        WriteNative(native);
    }

    /// <summary>
    /// Gets the learning rate for the given epoch using the native schedule logic.
    /// </summary>
    public float GetLearningRate(int epoch, float currentLoss = 0f)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return CceNative.CceSchedulerGetLr(_native, epoch, currentLoss);
    }

    internal IntPtr DangerousHandle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return _native;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        if (_native != IntPtr.Zero)
        {
            Marshal.FreeHGlobal(_native);
            _native = IntPtr.Zero;
        }
        _disposed = true;
        GC.SuppressFinalize(this);
    }

    ~CceScheduler() => Dispose();

    private Interop.CceNative.CceSchedulerNative ReadNative()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return Marshal.PtrToStructure<Interop.CceNative.CceSchedulerNative>(_native);
    }

    private void WriteNative(Interop.CceNative.CceSchedulerNative native)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Marshal.StructureToPtr(native, _native, false);
    }
}
