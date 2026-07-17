// Managed CNET inference harness session.
//
// Public, reusable API. Uses a SafeHandle whose ReleaseHandle path calls
// cnet_harness_close exactly once. Generation results are wrapped in another
// SafeHandle whose ReleaseHandle calls cnet_harness_generation_free exactly
// once — the raw pointer is never exposed, so double-free from managed code
// is not possible.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace CNET.Cce.CnetHarness;

internal sealed class CnetHarnessSessionHandle : SafeHandle
{
    private readonly ICnetHarnessNative _native;

    public CnetHarnessSessionHandle(ICnetHarnessNative native, IntPtr raw)
        : base(IntPtr.Zero, ownsHandle: true)
    {
        _native = native;
        SetHandle(raw);
    }

    public override bool IsInvalid => handle == IntPtr.Zero;
    public IntPtr Raw => handle;

    protected override bool ReleaseHandle()
    {
        int rc = _native.Close(handle);
        return rc == (int)CnetHarnessStatus.Ok;
    }
}

internal sealed class CnetHarnessGenerationHandle : SafeHandle
{
    private readonly ICnetHarnessNative _native;

    public CnetHarnessGenerationHandle(ICnetHarnessNative native, IntPtr raw)
        : base(IntPtr.Zero, ownsHandle: true)
    {
        _native = native;
        SetHandle(raw);
    }

    public override bool IsInvalid => handle == IntPtr.Zero;
    public IntPtr Raw => handle;

    protected override bool ReleaseHandle()
    {
        _native.GenerationFree(handle);
        return true;
    }
}

/// <summary>
/// A CNET inference harness session. Wraps the versioned C ABI. Callers
/// dispose the session to release the native lease and llama context.
/// </summary>
public sealed class CnetHarnessSession : IDisposable
{
    private readonly ICnetHarnessNative _native;
    private readonly CnetHarnessSessionHandle _handle;
    private readonly object _gate = new();
    private bool _disposed;

    internal CnetHarnessSession(ICnetHarnessNative native,
                                 CnetHarnessSessionHandle handle)
    {
        _native = native;
        _handle = handle;
    }

    /// <summary>Open a new session. Throws <see cref="CnetHarnessException"/> on any non-OK native status.</summary>
    public static CnetHarnessSession Open(CnetHarnessConfig config)
        => Open(config, new LibraryCnetHarnessNative());

    internal static CnetHarnessSession Open(CnetHarnessConfig config,
                                             ICnetHarnessNative native)
    {
        ArgumentNullException.ThrowIfNull(config);
        ArgumentNullException.ThrowIfNull(native);
        ValidateConfig(config);

        IntPtr modelIdPtr = MarshalUtf8(config.ModelId);
        IntPtr modelPathPtr = MarshalUtf8(config.ModelPath);
        try
        {
            NativeConfig nc = new()
            {
                AbiVersion = AbiConstants.AbiVersion,
                StructSize = (uint)Marshal.SizeOf<NativeConfig>(),
                ModelId = modelIdPtr,
                ModelPath = modelPathPtr,
                ResourceMask = config.ResourceMask,
                BudgetBytes = config.BudgetBytes,
                MainGpu = config.MainGpu,
                NCtx = config.ContextTokens,
                NBatch = config.BatchTokens,
                NThreads = config.Threads,
                AicimoNumOps = config.AicimoNumOps,
                AicimoBaseDim = config.AicimoBaseDim,
            };

            int rc = native.Open(in nc, out IntPtr raw);
            if (rc != (int)CnetHarnessStatus.Ok || raw == IntPtr.Zero)
            {
                throw MakeException(native, rc, "cnet_harness_open");
            }

            var safeHandle = new CnetHarnessSessionHandle(native, raw);
            return new CnetHarnessSession(native, safeHandle);
        }
        finally
        {
            if (modelIdPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(modelIdPtr);
            if (modelPathPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(modelPathPtr);
        }
    }

    /// <summary>Perform one synchronous, non-streaming generation.</summary>
    public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
    {
        lock (_gate)
        {
            return GenerateCore(options);
        }
    }

    private CnetHarnessGenerationResult GenerateCore(CnetHarnessGenerateOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        ObjectDisposedException.ThrowIf(_disposed, this);
        ValidateGenerateOptions(options);

        IntPtr systemPtr = options.System is null
            ? IntPtr.Zero : MarshalUtf8(options.System);
        IntPtr userPtr = MarshalUtf8(options.User);
        IntPtr rolePtr = MarshalUtf8(options.Role);
        try
        {
            NativeGenerateOptions nopts = new()
            {
                AbiVersion = AbiConstants.AbiVersion,
                StructSize = (uint)Marshal.SizeOf<NativeGenerateOptions>(),
                System = systemPtr,
                User = userPtr,
                Role = rolePtr,
                MaxTokens = options.MaxTokens,
                Seed = options.Seed,
                Sampling = options.Sampling,
            };

            int rc = _native.Generate(_handle.Raw, in nopts, out IntPtr genPtr);
            if (rc != (int)CnetHarnessStatus.Ok || genPtr == IntPtr.Zero)
            {
                throw MakeException(_native, rc, "cnet_harness_generate");
            }

            using var genHandle = new CnetHarnessGenerationHandle(_native, genPtr);
            var layout = _native.ReadGeneration(genHandle.Raw);
            return new CnetHarnessGenerationResult(
                layout.Text,
                layout.PromptTokens,
                layout.GeneratedTokens,
                layout.PromptMs,
                layout.GenerationMs,
                layout.SelectedAdapter,
                layout.RouteUncertainty,
                layout.EffectiveSampling,
                layout.AicimoOverride,
                layout.EffectiveTemperature,
                layout.EffectiveTopP,
                layout.EffectiveTopK,
                layout.EffectiveMinP);
        }
        finally
        {
            if (systemPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(systemPtr);
            Marshal.FreeCoTaskMem(userPtr);
            Marshal.FreeCoTaskMem(rolePtr);
        }
    }

    /// <summary>Perform the AICIMO decision without generating any tokens.</summary>
    public CnetHarnessRouteInfo ProbeRoute(string role,
        CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto)
    {
        lock (_gate)
        {
            return ProbeRouteCore(role, overrideMode);
        }
    }

    private CnetHarnessRouteInfo ProbeRouteCore(string role,
        CnetHarnessSamplingMode overrideMode)
    {
        ArgumentException.ThrowIfNullOrEmpty(role);
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!SamplingModeValid(overrideMode))
            throw new ArgumentOutOfRangeException(nameof(overrideMode));

        NativeRouteInfo info = new()
        {
            AbiVersion = AbiConstants.AbiVersion,
            StructSize = (uint)Marshal.SizeOf<NativeRouteInfo>(),
        };
        int rc = _native.ProbeRoute(_handle.Raw, role, overrideMode, ref info);
        if (rc != (int)CnetHarnessStatus.Ok)
        {
            throw MakeException(_native, rc, "cnet_harness_probe_route");
        }
        if (info.AbiVersion != AbiConstants.AbiVersion ||
            info.StructSize != (uint)Marshal.SizeOf<NativeRouteInfo>())
        {
            throw new CnetHarnessException(CnetHarnessStatus.InvalidState,
                $"native route ABI mismatch: version={info.AbiVersion}, size={info.StructSize}");
        }
        return new CnetHarnessRouteInfo(
            info.SelectedAdapter,
            info.RouteUncertainty,
            info.EffectiveSampling,
            info.EffectiveTemperature,
            info.EffectiveTopP,
            info.EffectiveTopK,
            info.EffectiveMinP);
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            _handle.Dispose();
        }
    }

    private const ulong KnownResourceMask =
        (ulong)(CnetHarnessResource.Cpu | CnetHarnessResource.Gpu0 |
                CnetHarnessResource.Gpu1 | CnetHarnessResource.Gpu2 |
                CnetHarnessResource.Gpu3);

    private static void ValidateConfig(CnetHarnessConfig c)
    {
        if (string.IsNullOrEmpty(c.ModelId))
            throw new ArgumentException("ModelId is required", nameof(c));
        if (string.IsNullOrEmpty(c.ModelPath))
            throw new ArgumentException("ModelPath is required", nameof(c));
        ulong mask = c.ResourceMask;
        if (mask == 0ul || (mask & (mask - 1ul)) != 0ul ||
            (mask & ~KnownResourceMask) != 0ul)
        {
            throw new ArgumentException(
                "ResourceMask must be exactly one of CnetHarnessResource.{Cpu,Gpu0..Gpu3}",
                nameof(c));
        }
        if (c.BudgetBytes == 0ul)
            throw new ArgumentException("BudgetBytes must be > 0", nameof(c));
        if (c.ContextTokens < 256u)
            throw new ArgumentException("ContextTokens must be >= 256", nameof(c));
        if (c.BatchTokens == 0u || c.BatchTokens > c.ContextTokens)
            throw new ArgumentException("BatchTokens must be in (0, ContextTokens]", nameof(c));
        if (c.Threads == 0u || c.Threads > 1024u)
            throw new ArgumentException("Threads must be in (0, 1024]", nameof(c));
        if (c.AicimoNumOps < 4u || c.AicimoNumOps > 256u)
            throw new ArgumentException("AicimoNumOps must be in [4, 256]", nameof(c));
        if (c.AicimoBaseDim < 8u || c.AicimoBaseDim > 8192u)
            throw new ArgumentException("AicimoBaseDim must be in [8, 8192]", nameof(c));
    }

    private static void ValidateGenerateOptions(CnetHarnessGenerateOptions o)
    {
        if (string.IsNullOrEmpty(o.User))
            throw new ArgumentException("User is required", nameof(o));
        if (string.IsNullOrEmpty(o.Role))
            throw new ArgumentException("Role is required", nameof(o));
        if (o.MaxTokens == 0u || o.MaxTokens > 65536u)
            throw new ArgumentException("MaxTokens must be in (0, 65536]", nameof(o));
        if (!SamplingModeValid(o.Sampling))
            throw new ArgumentOutOfRangeException(nameof(o.Sampling));
    }

    private static bool SamplingModeValid(CnetHarnessSamplingMode mode) =>
        mode is CnetHarnessSamplingMode.Auto or
            CnetHarnessSamplingMode.Deterministic or
            CnetHarnessSamplingMode.Focused or
            CnetHarnessSamplingMode.Balanced or
            CnetHarnessSamplingMode.Exploratory;

    private static CnetHarnessException MakeException(
        ICnetHarnessNative native, int rc, string operation)
    {
        var status = (CnetHarnessStatus)rc;
        string reason = native.ErrorString(rc);
        return new CnetHarnessException(status, $"{operation} failed: {reason} ({status})");
    }

    private static IntPtr MarshalUtf8(string value)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        IntPtr buf = Marshal.AllocCoTaskMem(bytes.Length + 1);
        Marshal.Copy(bytes, 0, buf, bytes.Length);
        Marshal.WriteByte(buf, bytes.Length, 0);
        return buf;
    }
}
